// bake.lsp — master implementation: the language server.
//
// The master owns the editor-facing protocol, the compile-command
// view (bake.buildsystem.query) and the symbol index (bake.lsp.index).
// Every Clang frontend run happens in a worker process (`bake
// lsp-worker`) over bake.lsp.proto:
//
//   - one dedicated interactive worker serves the latency-sensitive
//     lane (preamble builds, keystroke diagnostics, completion),
//     serialized FIFO;
//   - N index workers run the background indexing queue in parallel;
//   - a crashed worker is respawned (with backoff); its in-flight
//     request fails and the caller retries or degrades.
//
// The event loop never blocks on a compile — handlers co_await
// worker round-trips, and stale diagnostics are dropped by a
// per-document generation counter.

module;

#include <kota/async/io/loop.h>
#include <kota/async/io/process.h>
#include <kota/ipc/codec/json.h>
#include <kota/ipc/lsp/protocol.h>
#include <kota/ipc/lsp/uri.h>

module bake.lsp;

import std;
import bake.util;
import bake.json;
import bake.toolchain.target;
import bake.buildsystem.query;
import bake.toolchain.compilation;  // SymbolOccurrence record
import bake.lsp.proto;
import bake.lsp.index;

namespace bake {

namespace proto = kota::ipc::protocol;
namespace wire = lsp_proto;

namespace {

// file:// URI → filesystem path (percent-decoded).
std::string uri_to_path(const std::string& uri) {
    if (!starts_with(uri, "file://")) return uri;
    auto parsed = kota::ipc::lsp::URI::parse(uri);
    if (!parsed) return uri;
    auto decoded = parsed->percent_decode(parsed->path());
    return decoded ? *decoded : std::string(parsed->path());
}

// filesystem path → file:// URI.
std::string path_to_uri(const std::string& path) {
    auto uri = kota::ipc::lsp::URI::from_file_path(path);
    if (!uri) return path;
    return uri->str();
}

std::string canonical_path_string(const std::string& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(
        std::filesystem::path(path), ec);
    if (ec) return path;
    return canonical.generic_string();
}

struct Document {
    std::string path;
    std::string text;
    proto::integer version = 0;
};

// Cached preamble PCH state for one document.
struct PreambleState {
    std::string key;       // content key the PCH was built from
    std::string pch_path;  // cache file
    unsigned size = 0;     // effective region size (clang-exact or capped)
    bool ends_at_start_of_line = false;
    std::size_t region_limit = std::size_t(-1);  // cap it was built under

    bool valid() const { return !key.empty(); }
};

// The cheap preamble approximation used for cache keys: bytes up to
// the first line that is neither blank, a #-directive nor a comment.
// It never extends past Clang's real preamble (comments do not end
// one); the consuming compile re-derives the exact bounds and drops
// the PCH on mismatch, so key misses only cost a rebuild, never
// correctness.
std::string cheap_prefix(const std::string& text) {
    std::size_t pos = 0;
    bool in_block_comment = false;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        std::size_t end = eol == std::string::npos ? text.size() : eol;
        std::string_view line(text.data() + pos, end - pos);
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);

        if (in_block_comment) {
            if (line.find("*/") == std::string_view::npos) {
                if (eol == std::string::npos) return text;
                pos = eol + 1;
                continue;
            }
            in_block_comment = false;
            if (eol == std::string::npos) return text;
            pos = eol + 1;
            continue;  // code after */ on the same line: stop being cheap
        }

        bool blank = line.find_first_not_of(" \t\v\f") ==
                     std::string_view::npos;
        bool directive = !line.empty() && line.front() == '#';
        bool line_comment = line.starts_with("//");
        bool opens_block = line.starts_with("/*");
        if (!blank && !directive && !line_comment && !opens_block) break;
        if (opens_block && line.find("*/") == std::string_view::npos)
            in_block_comment = true;
        if (eol == std::string::npos) return text;
        pos = eol + 1;
    }
    return text.substr(0, pos);
}

// Byte offset of the first module construct (`module;`, a module
// declaration, or an import) — the PCH region cap for module TUs: a
// PCH cannot carry module imports, so it covers only the plain
// preprocessor territory before them and the imports re-run against
// real module files. npos means no cap (natural bounds).
std::size_t module_region_limit(const std::string& text) {
    std::size_t pos = 0;
    bool in_block_comment = false;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        std::size_t end = eol == std::string::npos ? text.size() : eol;
        std::string_view line(text.data() + pos, end - pos);
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        if (in_block_comment) {
            auto close = line.find("*/");
            if (close == std::string_view::npos) {
                pos = eol == std::string::npos ? text.size() : eol + 1;
                continue;
            }
            in_block_comment = false;
            line = line.substr(close + 2);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        bool line_comment = line.starts_with("//");
        if (line_comment || line.empty()) {
            pos = eol == std::string::npos ? text.size() : eol + 1;
            continue;
        }
        if (line.starts_with("/*")) {
            in_block_comment = true;
            pos = eol == std::string::npos ? text.size() : eol + 1;
            continue;
        }
        if (line.starts_with("module;") || line.starts_with("module ") ||
            line.starts_with("import ") || line.starts_with("export module ") ||
            line.starts_with("export import "))
            return pos;
        pos = eol == std::string::npos ? text.size() : eol + 1;
    }
    return std::size_t(-1);
}

// LSP 0-based (line, character) → byte offset in the text.
std::size_t position_to_offset(const std::string& text,
                               unsigned line, unsigned character) {
    std::size_t offset = 0;
    for (unsigned l = 0; l < line && offset < text.size(); ++l) {
        std::size_t nl = text.find('\n', offset);
        if (nl == std::string::npos) return text.size();
        offset = nl + 1;
    }
    return offset + character > text.size() ? text.size()
                                            : offset + character;
}

bool word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string word_at(const std::string& text, unsigned line,
                    unsigned character) {
    std::size_t offset = position_to_offset(text, line, character);
    std::size_t end = offset;
    while (end < text.size() && word_char(text[end])) ++end;
    std::size_t begin = offset;
    while (begin > 0 && word_char(text[begin - 1])) --begin;
    return text.substr(begin, end - begin);
}

proto::CompletionItemKind completion_kind(const std::string& kind) {
    if (kind == "Function") return proto::CompletionItemKind::Function;
    if (kind == "CXXMethod") return proto::CompletionItemKind::Method;
    if (kind == "CXXConstructor" || kind == "CXXDestructor")
        return proto::CompletionItemKind::Constructor;
    if (kind == "ConversionFunction")
        return proto::CompletionItemKind::Function;
    if (kind == "Var" || kind == "ParmVar" || kind == "VarTemplate" ||
        kind == "Decomposition")
        return proto::CompletionItemKind::Variable;
    if (kind == "Field") return proto::CompletionItemKind::Field;
    if (kind == "Class" || kind == "ClassTemplate")
        return proto::CompletionItemKind::Class;
    if (kind == "CXXRecord" || kind == "Struct" || kind == "Union" ||
        kind == "TemplateTypeParm")
        return proto::CompletionItemKind::Struct;
    if (kind == "Enum") return proto::CompletionItemKind::Enum;
    if (kind == "EnumConstant") return proto::CompletionItemKind::EnumMember;
    if (kind == "Typedef" || kind == "TypeAlias" ||
        kind == "TypeAliasTemplate")
        return proto::CompletionItemKind::Interface;
    if (kind == "Namespace" || kind == "NamespaceAlias")
        return proto::CompletionItemKind::Module;
    if (kind == "macro") return proto::CompletionItemKind::Constant;
    if (kind == "keyword") return proto::CompletionItemKind::Keyword;
    if (kind == "pattern") return proto::CompletionItemKind::Snippet;
    return proto::CompletionItemKind::Text;
}

proto::Position to_position(unsigned line, unsigned column) {
    proto::Position pos;
    pos.line = line > 0 ? line - 1 : 0;
    pos.character = column > 0 ? column - 1 : 0;
    return pos;
}

proto::Location to_location(const IndexLocation& loc) {
    proto::Location out;
    out.uri = path_to_uri(loc.file);
    proto::Position start = to_position(loc.line, loc.column);
    out.range = proto::Range{.start = start, .end = start};
    return out;
}

// Manual-unlock scope guard for kota::mutex (its lock is co_awaited,
// so std::scoped_lock cannot manage it).
class MutexUnlock {
public:
    explicit MutexUnlock(kota::mutex& m) : m_mutex(m), m_armed(true) {}
    MutexUnlock(const MutexUnlock&) = delete;
    MutexUnlock& operator=(const MutexUnlock&) = delete;
    ~MutexUnlock() {
        if (m_armed) m_mutex.unlock();
    }
private:
    kota::mutex& m_mutex;
    bool m_armed;
};

// ── worker process ───────────────────────────────────────────

// One `bake lsp-worker` child: request in (stdin pipe), framed
// response out (stdout pipe), one in-flight request at a time.
// A reader task ("pump") routes responses into the pending slot.
class WorkerProcess {
public:
    using ExitCallback = std::function<void()>;

    WorkerProcess(kota::event_loop& loop, std::string role)
        : m_loop(loop), m_role(std::move(role)) {}

    bool start(ExitCallback on_exit) {
        kota::process::options opts;
        opts.file = bake_exe_path();
        opts.args = {opts.file, "lsp-worker"};
        opts.streams = {
            kota::process::stdio::pipe(/*readable=*/true, /*writable=*/false),
            kota::process::stdio::pipe(/*readable=*/false, /*writable=*/true),
            kota::process::stdio::inherit(),
        };
        auto spawned = kota::process::spawn(opts, m_loop);
        if (!spawned) {
            std::println(std::cerr, "bake lsp: {} worker spawn failed: {}",
                         m_role, spawned.error().message());
            return false;
        }
        m_proc = std::move(spawned->proc);
        m_in = std::move(spawned->stdin_pipe);
        m_out = std::move(spawned->stdout_pipe);
        m_on_exit = std::move(on_exit);
        m_alive = true;
        m_loop.schedule(pump());
        return true;
    }

    bool alive() const { return m_alive; }
    bool busy() const { return m_busy; }

    // Serialized FIFO request lane. Fails fast when the worker is
    // dead (respawn is the pool's business).
    kota::task<wire::WireResponse> call(wire::WireRequest req) {
        co_await m_call_mutex.lock();
        MutexUnlock unlock(m_call_mutex);

        if (!m_alive) {
            wire::WireResponse dead;
            dead.id = req.id;
            dead.ok = false;
            dead.error = m_role + " worker is not running";
            co_return dead;
        }
        m_busy = true;
        m_pending.done.reset();
        req.id = ++s_request_ids;
        auto frame = wire::encode_request(req);
        auto written = co_await m_in->write(
            std::span<const char>(frame.data(), frame.size()));
        if (written.has_error()) {
            m_busy = false;
            wire::WireResponse failed;
            failed.id = req.id;
            failed.ok = false;
            failed.error = "worker pipe write failed: " +
                           std::string(written.error().message());
            co_return failed;
        }
        co_await m_pending.done.wait();
        m_busy = false;
        co_return m_pending.resp;
    }

    void kill_hard() {
        if (!m_alive) return;
        m_alive = false;
        m_proc.kill(SIGKILL);
    }

private:
    kota::task<> pump() {
        for (;;) {
            auto chunk = co_await m_out->read();
            if (chunk.has_error()) break;  // EOF or pipe error: worker gone
            m_read_buf += chunk.value();
            for (;;) {
                auto payload = wire::take_frame(m_read_buf);
                if (!payload) break;  // need more data (or desync: bail)
                auto resp = wire::decode_response(*payload);
                if (!resp) {
                    std::println(std::cerr,
                                 "bake lsp: {} worker sent an undecodable "
                                 "response",
                                 m_role);
                    break;
                }
                m_pending.resp = std::move(*resp);
                m_pending.done.set();
            }
        }
        m_alive = false;
        // Fail the in-flight request, if any.
        wire::WireResponse gone;
        gone.ok = false;
        gone.error = m_role + " worker exited";
        m_pending.resp = std::move(gone);
        m_pending.done.set();
        if (m_on_exit) {
            auto cb = std::move(m_on_exit);
            m_on_exit = nullptr;
            cb();
        }
        co_return;
    }

    struct Pending {
        wire::WireResponse resp;
        kota::event done;
    };

    static std::atomic<std::uint64_t> s_request_ids;

    kota::event_loop& m_loop;
    std::string m_role;
    kota::process m_proc;
    std::optional<kota::pipe> m_in;
    std::optional<kota::pipe> m_out;
    kota::mutex m_call_mutex;
    Pending m_pending;
    bool m_alive = false;
    bool m_busy = false;
    ExitCallback m_on_exit;
    std::string m_read_buf;
};

std::atomic<std::uint64_t> WorkerProcess::s_request_ids{0};

// ── worker pool ──────────────────────────────────────────────

// The asymmetric pool: one interactive worker for the latency lane,
// N index workers for the background queue. Crashes respawn under
// backoff (3 per 10s); a pool that keeps dying stops respawning and
// its lanes fail fast with the last error.
class WorkerPool {
public:
    WorkerPool(kota::event_loop& loop, std::size_t index_workers)
        : m_loop(loop), m_index_count(index_workers) {}

    void start() {
        m_interactive = spawn_one("interactive");
        for (std::size_t i = 0; i < m_index_count; ++i)
            m_index.push_back(spawn_one("index-" + std::to_string(i)));
    }

    void shutdown() {
        m_stopped = true;
        if (m_interactive) m_interactive->kill_hard();
        for (auto& w : m_index) w->kill_hard();
    }

    // Latency lane: FIFO through the dedicated interactive worker.
    kota::task<wire::WireResponse> interactive(wire::WireRequest req) {
        if (!m_interactive || !m_interactive->alive()) try_respawn(true, 0);
        if (!m_interactive) {
            wire::WireResponse dead;
            dead.ok = false;
            dead.error = "interactive worker unavailable (crash loop)";
            co_return dead;
        }
        co_return co_await m_interactive->call(std::move(req));
    }

    // Any idle index worker, else the interactive worker.
    kota::task<wire::WireResponse> any(wire::WireRequest req) {
        for (auto& w : m_index) {
            if (w && w->alive() && !w->busy()) {
                co_return co_await w->call(std::move(req));
            }
        }
        co_return co_await interactive(std::move(req));
    }

    WorkerProcess* index_worker(std::size_t i) {
        if (i >= m_index.size()) return nullptr;
        if (!m_index[i]->alive()) try_respawn(false, i);
        return m_index[i].get();
    }

    std::size_t index_count() const { return m_index.size(); }

private:
    std::unique_ptr<WorkerProcess> spawn_one(std::string role) {
        auto worker = std::make_unique<WorkerProcess>(m_loop, role);
        const bool is_interactive = role == "interactive";
        std::size_t slot = 0;
        for (std::size_t i = 0; i < m_index_count; ++i)
            if (role == "index-" + std::to_string(i)) slot = i;
        if (!worker->start([this, is_interactive, slot, role] {
                on_worker_exit(is_interactive, slot, role);
            })) {
            return nullptr;
        }
        return worker;
    }

    void on_worker_exit(bool is_interactive, std::size_t slot,
                        const std::string& role) {
        if (m_stopped) return;
        auto now = std::chrono::steady_clock::now();
        m_crashes.push_back(now);
        while (!m_crashes.empty() &&
               now - m_crashes.front() > std::chrono::seconds(10))
            m_crashes.pop_front();
        std::println(std::cerr, "bake lsp: {} worker exited ({} crash(es) "
                                "in the last 10s)",
                     role, m_crashes.size());
        if (m_crashes.size() >= 3) {
            std::println(std::cerr,
                         "bake lsp: {} worker crash loop — not respawning",
                         role);
            return;
        }
        try_respawn(is_interactive, slot);
    }

    void try_respawn(bool is_interactive, std::size_t slot) {
        auto next = spawn_one(is_interactive ? "interactive"
                                             : "index-" + std::to_string(slot));
        if (!next) return;
        if (is_interactive)
            m_interactive = std::move(next);
        else
            m_index[slot] = std::move(next);
    }

    kota::event_loop& m_loop;
    std::size_t m_index_count;
    std::unique_ptr<WorkerProcess> m_interactive;
    std::vector<std::unique_ptr<WorkerProcess>> m_index;
    std::deque<std::chrono::steady_clock::time_point> m_crashes;
    bool m_stopped = false;
};

// ── server ───────────────────────────────────────────────────

struct ServerOptions {
    std::string target_triple;       // empty → native
    std::string profile = "dev";
    std::size_t index_workers = 3;   // derived from cores in lsp_main
};

class Server {
public:
    Server(kota::event_loop& loop, kota::ipc::JsonPeer& peer,
           ServerOptions options)
        : m_loop(loop), m_peer(peer), m_options(std::move(options)) {}

    ~Server() {
        m_shutdown.store(true);
        if (m_pool) m_pool->shutdown();
    }

    void register_handlers() {
        // ── lifecycle ──

        m_peer.on_request(
            [this](kota::ipc::JsonPeer::RequestContext&,
                   const proto::InitializeParams& params)
                -> kota::ipc::RequestResult<proto::InitializeParams> {
                if (m_lifecycle != Lifecycle::Uninitialized) {
                    co_return kota::outcome_error(
                        proto::Error{"server already initialized"});
                }

                const auto& init = params.lsp__initialize_params;
                if (init.root_uri.has_value() &&
                    !init.root_uri->empty())
                    m_root = Path(uri_to_path(*init.root_uri));
                else if (init.root_path.has_value() &&
                         init.root_path->has_value() &&
                         !init.root_path->value().empty())
                    m_root = Path(**init.root_path);

                if (m_root.string().empty()) {
                    co_return kota::outcome_error(proto::Error{
                        "bake lsp requires a workspace root "
                        "(rootUri/rootPath)"});
                }

                m_lifecycle = Lifecycle::Initializing;

                proto::InitializeResult result;
                result.capabilities.text_document_sync =
                    proto::TextDocumentSyncOptions{
                        .open_close = true,
                        .change = proto::TextDocumentSyncKind::Full,
                    };
                result.capabilities.completion_provider =
                    proto::CompletionOptions{};
                result.capabilities.definition_provider = true;
                result.capabilities.references_provider = true;
                result.server_info =
                    proto::ServerInfo{.name = "bake lsp"};
                co_return result;
            });

        m_peer.on_request(
            [this](kota::ipc::JsonPeer::RequestContext&,
                   const proto::ShutdownParams&)
                -> kota::ipc::RequestResult<proto::ShutdownParams> {
                m_lifecycle = Lifecycle::Shutdown;
                co_return nullptr;
            });

        m_peer.on_notification(
            [this]([[maybe_unused]] const proto::InitializedParams& params) {
                // The compile database is built once the handshake
                // completes — commands come from the graph on disk.
                m_lifecycle = Lifecycle::Ready;
                rebuild_compile_db();
            });

        m_peer.on_notification([this]([[maybe_unused]] const proto::ExitParams& params) {
            // Exit code is protocol-driven: a clean handshake exit maps to
            // 0; EOF without exit (crash/disconnect) leaves the flag unset
            // and lsp_main reports failure.
            m_shutdown.store(true);
            if (m_pool) m_pool->shutdown();
            m_clean_exit = true;
            m_loop.stop();
        });

        // ── document sync ──

        m_peer.on_notification(
            [this](const proto::DidOpenTextDocumentParams& params) {
                if (!ready()) return;
                const auto& doc = params.text_document;
                Document open;
                open.path = uri_to_path(doc.uri);
                open.text = doc.text;
                open.version = doc.version;
                std::string path = open.path;
                m_documents[path] = std::move(open);
                schedule_publish(path);
            });

        m_peer.on_notification(
            [this](const proto::DidChangeTextDocumentParams& params) {
                if (!ready()) return;
                auto path = uri_to_path(params.text_document.uri);
                auto it = m_documents.find(path);
                if (it == m_documents.end()) {
                    std::println(std::cerr,
                                 "bake lsp: didChange without didOpen: {}",
                                 path);
                    return;
                }
                // Full sync: the last change carries the whole text.
                // (Partial changes never arrive — we advertise Full.)
                if (!params.content_changes.empty()) {
                    const auto& change = params.content_changes.back();
                    if (auto* whole =
                            std::get_if<
                                proto::TextDocumentContentChangeWholeDocument>(
                                &change))
                        it->second.text = whole->text;
                }
                it->second.version = params.text_document.version;
                schedule_publish(path);
            });

        m_peer.on_notification(
            [this](const proto::DidCloseTextDocumentParams& params) {
                if (!ready()) return;
                auto path = uri_to_path(params.text_document.uri);
                m_documents.erase(path);
                m_preambles.erase(path);
                // Clear the published set for the closed file.
                proto::PublishDiagnosticsParams clear;
                clear.uri = path_to_uri(path);
                m_peer.send_notification(clear);
            });

        // ── completion ──

        m_peer.on_request(
            [this](kota::ipc::JsonPeer::RequestContext&,
                   const proto::CompletionParams& params)
                -> kota::ipc::RequestResult<proto::CompletionParams> {
                if (!ready() || !m_db || !m_pool) co_return nullptr;

                const auto& position =
                    params.text_document_position_params;
                std::string path =
                    uri_to_path(position.text_document.uri);
                auto doc = m_documents.find(path);
                if (doc == m_documents.end()) co_return nullptr;
                const CompileEntry* entry = m_db->find(Path(path));
                if (!entry) co_return nullptr;

                unsigned line =
                    static_cast<unsigned>(position.position.line) + 1;
                unsigned column =
                    static_cast<unsigned>(position.position.character) + 1;

                std::string prefix = word_at(
                    doc->second.text,
                    static_cast<unsigned>(position.position.line),
                    static_cast<unsigned>(position.position.character));

                wire::WireRequest req;
                req.op = "complete";
                req.argv = syntax_command(entry->command);
                std::string file_spelling = command_file_spelling(*entry, path);
                req.remap[file_spelling] = doc->second.text;
                req.remap[path] = doc->second.text;
                req.main_file = file_spelling;
                req.line = line;
                req.column = column;

                auto resp = co_await m_pool->interactive(std::move(req));
                if (!resp.ok) {
                    std::println(std::cerr, "bake lsp: completion failed: {}",
                                 resp.error);
                    co_return nullptr;
                }

                proto::CompletionList list;
                list.is_incomplete = false;
                for (auto& r : resp.items) {
                    if (!prefix.empty() && !r.label.starts_with(prefix))
                        continue;
                    proto::CompletionItem item;
                    item.label = r.label;
                    item.kind = completion_kind(r.kind);
                    item.detail = r.detail.empty() ? r.kind : r.detail;
                    int priority = std::clamp(r.priority, 0, 999999);
                    item.sort_text = std::format("{:06}", priority) +
                                     r.label;
                    list.items.push_back(std::move(item));
                }
                co_return list;
            });

        // ── definition ──

        m_peer.on_request(
            [this](kota::ipc::JsonPeer::RequestContext&,
                   const proto::DefinitionParams& params)
                -> kota::ipc::RequestResult<proto::DefinitionParams> {
                if (!ready() || !m_db || !m_index || !m_pool)
                    co_return proto::Definition{};

                const auto& position =
                    params.text_document_position_params;
                std::string path =
                    uri_to_path(position.text_document.uri);
                auto doc = m_documents.find(path);
                const CompileEntry* entry = m_db->find(Path(path));

                unsigned line =
                    static_cast<unsigned>(position.position.line) + 1;
                unsigned column =
                    static_cast<unsigned>(position.position.character) + 1;

                // Resolve the symbol under the cursor; index the
                // open file on demand when nothing is loaded yet.
                std::optional<SymbolOccurrence> occ =
                    m_index->occurrence_at(path, line, column);
                if (!occ && entry) {
                    const std::string* text =
                        doc != m_documents.end() ? &doc->second.text
                                                 : nullptr;
                    co_await index_on_demand(*entry, text);
                    occ = m_index->occurrence_at(path, line, column);
                }

                std::vector<IndexLocation> locations;
                if (occ) {
                    locations = m_index->find_definitions(occ->usr);
                    if (locations.empty() && (occ->is_decl || occ->is_def))
                        locations.push_back(
                            {occ->file, occ->line, occ->column});
                    // The referenced declaration's own site is a valid
                    // target even when its file is not indexed yet.
                    if (locations.empty() && !occ->decl_file.empty() &&
                        occ->decl_line > 0)
                        locations.push_back(
                            {occ->decl_file, occ->decl_line,
                             occ->decl_column});
                } else if (doc != m_documents.end()) {
                    // Name fallback for positions outside recorded
                    // occurrences (whitespace, nested name specifiers).
                    std::string word = word_at(
                        doc->second.text,
                        static_cast<unsigned>(position.position.line),
                        static_cast<unsigned>(position.position.character));
                    if (!word.empty())
                        locations = m_index->find_definitions_by_name(word);
                }

                std::vector<proto::Location> out;
                for (const auto& loc : locations)
                    out.push_back(to_location(loc));
                co_return proto::Definition{std::move(out)};
            });

        // ── references ──

        m_peer.on_request(
            [this](kota::ipc::JsonPeer::RequestContext&,
                   const proto::ReferenceParams& params)
                -> kota::ipc::RequestResult<proto::ReferenceParams> {
                if (!ready() || !m_db || !m_index || !m_pool)
                    co_return std::vector<proto::Location>{};

                const auto& position =
                    params.text_document_position_params;
                std::string path =
                    uri_to_path(position.text_document.uri);
                auto doc = m_documents.find(path);
                const CompileEntry* entry = m_db->find(Path(path));

                unsigned line =
                    static_cast<unsigned>(position.position.line) + 1;
                unsigned column =
                    static_cast<unsigned>(position.position.character) + 1;

                std::optional<SymbolOccurrence> occ =
                    m_index->occurrence_at(path, line, column);
                if (!occ && entry) {
                    const std::string* text =
                        doc != m_documents.end() ? &doc->second.text
                                                 : nullptr;
                    co_await index_on_demand(*entry, text);
                    occ = m_index->occurrence_at(path, line, column);
                }
                if (!occ) co_return std::vector<proto::Location>{};

                // A cold index may not have seen the declaring file
                // yet; pull it in so the declaration-side reference
                // is not silently missing.
                if (!occ->decl_file.empty() && occ->decl_line > 0) {
                    if (const CompileEntry* decl_entry =
                            m_db->find(Path(occ->decl_file))) {
                        if (!m_index->has_current_shard(*decl_entry))
                            co_await index_on_demand(*decl_entry, nullptr);
                    }
                }

                auto locations = m_index->find_references(
                    occ->usr, params.context.include_declaration);

                std::vector<proto::Location> out;
                for (const auto& loc : locations)
                    out.push_back(to_location(loc));
                co_return out;
            });
    }

    bool clean_exit() const { return m_clean_exit; }

private:
    enum class Lifecycle { Uninitialized, Initializing, Ready, Shutdown };

    bool ready() const { return m_lifecycle == Lifecycle::Ready; }

    void rebuild_compile_db() {
        TargetSpec target = m_options.target_triple.empty()
                                ? detect_host_target()
                                : parse_target(m_options.target_triple);
        auto db = build_compile_db(m_root, target, m_options.profile,
                                   /*offline=*/true);
        if (!db) {
            std::println(std::cerr, "bake lsp: no compile commands: {}",
                         db.error());
            m_db.reset();
            return;
        }
        m_db = std::move(*db);
        // Project-scoped caches live beside the other build
        // intermediates: `bake clean` clears them with everything
        // else, and their keys already embed project paths.
        m_index = std::make_unique<ProjectIndex>(
            m_db->out_dir / ".bake" / "lsp-index");
        m_pool = std::make_unique<WorkerPool>(m_loop,
                                              m_options.index_workers);
        m_pool->start();
        std::println(std::cerr,
                     "bake lsp: {} compile entries ({}), {} index shards "
                     "from cache, {}+1 workers, target {}",
                     m_db->entries.size(), m_options.profile,
                     m_index->shard_count(), m_options.index_workers,
                     m_db->target_triple);
        start_background_index();
    }

    // The spelling the compile command uses for the main file —
    // remaps and completion positions must speak it too.
    std::string command_file_spelling(const CompileEntry& entry,
                                      const std::string& path) {
        std::string canonical = canonical_path_string(path);
        for (const auto& arg : entry.command) {
            if (arg.empty() || arg.front() == '-') continue;
            if (canonical_path_string(arg) == canonical) return arg;
        }
        return path;
    }

    Path preamble_cache_dir(const std::string& key) {
        return m_db->out_dir / ".bake" / "lsp-preamble" / "o" / key;
    }

    // Compile the file's current preamble into the cache through the
    // interactive worker; records the state on success, clears it on
    // failure (falling back to whole-file compiles).
    kota::task<bool> build_preamble(const CompileEntry& entry,
                                    const std::string& path,
                                    const std::string& file_spelling,
                                    const std::string& text,
                                    const std::string& key,
                                    std::size_t region_limit) {
        Path dir = preamble_cache_dir(key);
        if (!dir.exists()) dir.mkdir_recursive();
        Path pch = dir / "preamble.pch";

        wire::WireRequest req;
        req.op = "preamble";
        req.argv = syntax_command(entry.command);
        req.remap[file_spelling] = text;
        req.remap[path] = text;
        req.main_file = file_spelling;
        req.pch_out = pch.string();
        req.region_limit = region_limit;

        auto resp = co_await m_pool->interactive(std::move(req));
        PreambleState& state = m_preambles[path];
        if (!resp.ok || !resp.built) {
            std::println(std::cerr,
                         "bake lsp: preamble PCH build failed: {}",
                         resp.ok ? "status " + std::to_string(resp.status)
                                 : resp.error);
            for (const auto& d : resp.diagnostics)
                std::println(std::cerr, "  [{}]: {}:{}", d.severity,
                             d.message, d.file);
            std::error_code ec;
            std::filesystem::remove_all(dir.fs(), ec);
            state.key.clear();
            co_return false;
        }
        state.key = key;
        state.pch_path = pch.string();
        state.size = resp.preamble_size;
        state.ends_at_start_of_line = resp.ends_at_start_of_line;
        state.region_limit = region_limit;
        co_return true;
    }

    static bool pch_load_failed(const wire::WireResponse& resp) {
        // Clang phrases stale/mismatched PCHs as errors that mention
        // "PCH"; a wrong guess only costs one retry.
        for (const auto& diag : resp.diagnostics) {
            if (diag.severity != "error" && diag.severity != "fatal")
                continue;
            if (diag.message.find("PCH") != std::string::npos)
                return true;
        }
        return false;
    }

    // ── diagnostics pipeline (worker round-trips) ──

    void schedule_publish(const std::string& path) {
        unsigned gen = ++m_diag_generation[path];
        m_loop.schedule(publish_diagnostics(path, gen));
    }

    // By value: the task outlives the handler that schedules it.
    kota::task<> publish_diagnostics(std::string path, unsigned generation) {
        proto::PublishDiagnosticsParams params;
        params.uri = path_to_uri(path);

        if (m_db && m_pool) {
            if (const CompileEntry* entry = m_db->find(Path(path))) {
                auto doc = m_documents.find(path);
                const std::string& text =
                    doc != m_documents.end() ? doc->second.text
                                             : std::string();

                std::string file_spelling =
                    command_file_spelling(*entry, path);
                std::map<std::string, std::string> remap;
                if (doc != m_documents.end()) {
                    remap[file_spelling] = text;
                    remap[path] = text;
                }

                // Preamble PCH: the region is capped before the first
                // module construct (PCH and PCM loading are exclusive),
                // so module TUs keep a preprocessor-only preamble.
                wire::WirePchLoad pch;
                std::size_t region_limit = module_region_limit(text);
                if (doc != m_documents.end() && !cheap_prefix(text).empty()) {
                    std::string joined_command;
                    for (const auto& arg : entry->command)
                        joined_command += arg + '\n';
                    std::string key = SHA256::to_hex(SHA256::compute(
                        cheap_prefix(text) + std::string(1, '\0') +
                        joined_command));
                    PreambleState& state = m_preambles[path];
                    bool have = state.valid() && state.key == key &&
                                state.region_limit == region_limit &&
                                std::filesystem::exists(
                                    std::filesystem::path(state.pch_path));
                    if (!have)
                        have = co_await build_preamble(
                            *entry, path, file_spelling, text, key,
                            region_limit);
                    if (have) {
                        pch.path = state.pch_path;
                        pch.size = state.size;
                        pch.ends_at_start_of_line =
                            state.ends_at_start_of_line;
                        pch.region_limit = state.region_limit;
                    }
                }

                std::vector<std::string> command =
                    syntax_command(entry->command);
                wire::WireRequest req;
                req.op = "syntax";
                req.argv = command;
                req.remap = remap;
                req.pch = pch;
                auto resp = co_await m_pool->interactive(std::move(req));
                if (generation != m_diag_generation[path]) co_return;

                // Disk headers changed under the recorded stamps:
                // rebuild the PCH once, then retry.
                if (!pch.path.empty() && pch_load_failed(resp)) {
                    std::string key = m_preambles[path].key;
                    std::error_code ec;
                    std::filesystem::remove_all(
                        std::filesystem::path(pch.path).parent_path(), ec);
                    m_preambles.erase(path);
                    wire::WirePchLoad retry;
                    if (!key.empty() &&
                        co_await build_preamble(*entry, path, file_spelling,
                                                text, key, region_limit)) {
                        retry.path = m_preambles[path].pch_path;
                        retry.size = m_preambles[path].size;
                        retry.ends_at_start_of_line =
                            m_preambles[path].ends_at_start_of_line;
                        retry.region_limit = region_limit;
                    }
                    wire::WireRequest again;
                    again.op = "syntax";
                    again.argv = command;
                    again.remap = remap;
                    again.pch = retry;
                    resp = co_await m_pool->interactive(std::move(again));
                    if (generation != m_diag_generation[path]) co_return;
                }

                // Main-file diagnostics only: a header's diagnostics
                // belong to the header's own buffer.
                std::string canonical_main = canonical_path_string(path);
                for (const auto& diag : resp.diagnostics) {
                    if (!diag.file.empty() &&
                        canonical_path_string(diag.file) != canonical_main)
                        continue;

                    proto::Diagnostic out;
                    out.range = proto::Range{
                        .start = to_position(diag.line, diag.column),
                        .end = to_position(diag.line, diag.column)};

                    if (diag.severity == "fatal" || diag.severity == "error")
                        out.severity = proto::DiagnosticSeverity::Error;
                    else if (diag.severity == "warning")
                        out.severity = proto::DiagnosticSeverity::Warning;
                    else if (diag.severity == "remark")
                        out.severity =
                            proto::DiagnosticSeverity::Information;
                    // notes keep the default (unset) severity

                    out.message = diag.message;
                    out.source = "bake";
                    params.diagnostics.push_back(std::move(out));
                }
            }
        }
        m_peer.send_notification(params);
        co_return;
    }

    // ── indexing (worker round-trips) ──

    // Collect one TU's occurrences through an idle lane and store
    // the shard. No-op when the shard already exists.
    kota::task<bool> index_on_demand(const CompileEntry& entry,
                                     const std::string* text) {
        std::string key = m_index->shard_key_of(entry, text);
        if (m_index->has_shard(key)) co_return true;

        std::string content = text ? *text : std::string();
        wire::WireRequest req;
        req.op = "collect";
        req.argv = syntax_command(entry.command);
        std::string file_spelling = entry.file.string();
        if (text) {
            file_spelling = command_file_spelling(entry, entry.file.string());
            req.remap[file_spelling] = content;
            req.remap[entry.file.string()] = content;
        }
        req.main_file = file_spelling;

        auto resp = co_await m_pool->any(std::move(req));
        if (!resp.ok) {
            std::println(std::cerr, "bake lsp: index collect failed: {}",
                         resp.error);
            co_return false;
        }
        std::vector<SymbolOccurrence> occurrences;
        occurrences.reserve(resp.occurrences.size());
        for (auto& o : resp.occurrences) {
            SymbolOccurrence occ;
            occ.usr = std::move(o.usr);
            occ.name = std::move(o.name);
            occ.kind = std::move(o.kind);
            occ.is_decl = o.flags & 1;
            occ.is_def = o.flags & 2;
            occ.is_ref = o.flags & 4;
            occ.is_call = o.flags & 8;
            occ.file = std::move(o.file);
            occ.line = o.line;
            occ.column = o.column;
            occ.decl_file = std::move(o.decl_file);
            occ.decl_line = o.decl_line;
            occ.decl_column = o.decl_column;
            occurrences.push_back(std::move(occ));
        }
        m_index->adopt(entry, text, std::move(occurrences));
        co_return true;
    }

    void start_background_index() {
        if (!m_db || !m_index || !m_pool) return;
        std::string root = canonical_path_string(m_root.string());
        for (const auto& entry : m_db->entries) {
            std::string canonical =
                canonical_path_string(entry.file.string());
            if (!canonical.starts_with(root + "/")) continue;
            if (m_index->has_current_shard(entry)) continue;
            m_index_queue.push_back(entry);
        }
        if (m_index_queue.empty()) return;
        std::println(std::cerr, "bake lsp: background indexing {} TUs "
                                "across {} workers",
                     m_index_queue.size(), m_pool->index_count());
        for (std::size_t i = 0; i < m_pool->index_count(); ++i)
            m_loop.schedule(index_dispatcher(i));
    }

    // One dispatcher per index worker: pops the shared queue (safe
    // without a lock — the event loop is single-threaded and pops
    // happen between awaits) and runs its worker through collect
    // ops back to back.
    kota::task<> index_dispatcher(std::size_t worker_index) {
        std::size_t indexed = 0;
        ++m_active_dispatchers;
        for (;;) {
            if (m_shutdown.load()) break;
            if (m_index_queue.empty()) break;
            CompileEntry entry = std::move(m_index_queue.front());
            m_index_queue.pop_front();
            WorkerProcess* worker = m_pool->index_worker(worker_index);
            if (!worker) break;  // crash loop: leave the rest queued

            std::string content;  // read on demand for the key
            wire::WireRequest req;
            req.op = "collect";
            req.argv = syntax_command(entry.command);
            req.main_file = entry.file.string();

            auto resp = co_await worker->call(std::move(req));
            if (m_shutdown.load()) break;
            if (!resp.ok) {
                std::println(std::cerr,
                             "bake lsp: background collect failed for {}: "
                             "{}",
                             entry.file.filename_string(), resp.error);
                continue;
            }
            std::vector<SymbolOccurrence> occurrences;
            occurrences.reserve(resp.occurrences.size());
            for (auto& o : resp.occurrences) {
                SymbolOccurrence occ;
                occ.usr = std::move(o.usr);
                occ.name = std::move(o.name);
                occ.kind = std::move(o.kind);
                occ.is_decl = o.flags & 1;
                occ.is_def = o.flags & 2;
                occ.is_ref = o.flags & 4;
                occ.is_call = o.flags & 8;
                occ.file = std::move(o.file);
                occ.line = o.line;
                occ.column = o.column;
                occ.decl_file = std::move(o.decl_file);
                occ.decl_line = o.decl_line;
                occ.decl_column = o.decl_column;
                occurrences.push_back(std::move(occ));
            }
            m_index->adopt(entry, nullptr, std::move(occurrences));
            ++indexed;
            std::println(std::cerr, "bake lsp: indexed {} ({} shards)",
                         entry.file.filename_string(),
                         m_index->shard_count());
        }
        --m_active_dispatchers;
        if (m_index_queue.empty() && m_active_dispatchers == 0 &&
            !m_shutdown.load()) {
            m_index->enforce_quota();
            std::println(std::cerr,
                         "bake lsp: background indexing done "
                         "({} shards, {} occurrences)",
                         m_index->shard_count(),
                         m_index->occurrence_count());
        }
        co_return;
    }

    kota::event_loop& m_loop;
    kota::ipc::JsonPeer& m_peer;
    ServerOptions m_options;
    Lifecycle m_lifecycle = Lifecycle::Uninitialized;
    bool m_clean_exit = false;

    Path m_root;
    std::optional<CompileDb> m_db;
    std::unique_ptr<ProjectIndex> m_index;
    std::unique_ptr<WorkerPool> m_pool;
    std::atomic<bool> m_shutdown{false};
    std::map<std::string, Document> m_documents;
    std::map<std::string, PreambleState> m_preambles;
    std::map<std::string, unsigned> m_diag_generation;
    std::deque<CompileEntry> m_index_queue;
    std::size_t m_active_dispatchers = 0;
};

}  // namespace

int lsp_main(const std::vector<std::string>& args) {
    ServerOptions options;
    unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    options.index_workers =
        std::clamp<std::size_t>(hardware / 2, 1, 6);

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto value = [&]() -> std::string {
            if (arg.starts_with("--") && arg.find('=') != std::string::npos)
                return arg.substr(arg.find('=') + 1);
            return i + 1 < args.size() ? args[i + 1] : std::string();
        };
        auto is = [&](const char* name) {
            return arg == name ||
                   arg.starts_with(std::string(name) + "=");
        };
        if (is("--target")) {
            options.target_triple = value();
            if (!arg.contains('=') && i + 1 < args.size()) ++i;
        } else if (is("--profile")) {
            options.profile = value();
            if (!arg.contains('=') && i + 1 < args.size()) ++i;
        } else if (is("--index-jobs")) {
            long n = 0;
            std::size_t consumed = 1;
            if (arg.find('=') != std::string::npos) {
                n = std::atol(value().c_str());
            } else if (i + 1 < args.size()) {
                n = std::atol(args[i + 1].c_str());
                consumed = 2;
            }
            if (n >= 1) options.index_workers = static_cast<std::size_t>(n);
            i += consumed - 1;
        } else {
            std::println(std::cerr,
                         "bake lsp: unknown option '{}' (supported: "
                         "--target <triple> --profile <name> "
                         "--index-jobs <n>)",
                         arg);
            return 1;
        }
    }

    kota::event_loop loop;
    auto transport = kota::ipc::StreamTransport::open_stdio(loop);
    if (!transport) {
        std::println(std::cerr, "bake lsp: failed to open stdio: {}",
                     transport.error().message);
        return 1;
    }

    kota::ipc::JsonPeer peer(loop, std::move(*transport));
    Server server(loop, peer, std::move(options));
    server.register_handlers();

    loop.schedule(peer.run());
    int rc = loop.run();
    return server.clean_exit() ? 0 : (rc != 0 ? rc : 1);
}

}  // namespace bake
