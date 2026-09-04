// bake.lsp — stage1 implementation: the language server.
//
// Lifecycle (initialize/initialized/shutdown/exit), full document
// sync (didOpen/didChange/didClose), diagnostics, completion,
// definition/references. Compiles run in-process through
// bake.toolchain.compilation with the buffer remapped, against
// the command from bake.buildsystem.query — the same argv a
// build would run. Preamble regions compile once into PCHs
// (content-keyed under the bake cache) so keystroke compiles
// parse the body only; a low-priority background thread fills
// the project index (bake.lsp.index) for cross-file queries.

module;

#include <kota/async/io/loop.h>
#include <kota/ipc/codec/json.h>
#include <kota/ipc/lsp/protocol.h>
#include <kota/ipc/lsp/uri.h>

module bake.lsp;

import std;
import bake.util;
import bake.toolchain.target;
import bake.toolchain.compilation;
import bake.toolchain.runtime;
import bake.buildsystem.query;
import bake.lsp.index;

namespace bake {

namespace proto = kota::ipc::protocol;

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
    unsigned size = 0;     // clang-exact preamble bounds
    bool ends_at_start_of_line = false;

    bool valid() const { return !key.empty(); }
};

// The cheap preamble approximation used for cache keys: bytes up
// to the first line that is neither blank nor a #-directive. It
// never extends past Clang's real preamble; the consuming compile
// re-derives the exact bounds and drops the PCH on mismatch, so
// key misses only cost a rebuild, never correctness.
std::string cheap_prefix(const std::string& text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        std::size_t end = eol == std::string::npos ? text.size() : eol;
        std::string_view line(text.data() + pos, end - pos);
        while (!line.empty() && (line.back() == '\r')) line.remove_suffix(1);
        bool blank = line.find_first_not_of(" \t\v\f") ==
                     std::string_view::npos;
        bool directive = !line.empty() && line.front() == '#';
        if (!blank && !directive) break;
        if (eol == std::string::npos) return text;
        pos = eol + 1;
    }
    return text.substr(0, pos);
}

// Module imports cannot ride in a PCH alongside module PCMs; such
// preambles compile without one.
bool prefix_has_module_import(const std::string& prefix) {
    std::size_t pos = 0;
    while (pos < prefix.size()) {
        std::size_t eol = prefix.find('\n', pos);
        std::size_t end = eol == std::string::npos ? prefix.size() : eol;
        std::string_view line(prefix.data() + pos, end - pos);
        while (!line.empty() && (line.empty() || line.front() == ' '))
            line.remove_prefix(1);
        if (line.starts_with("import ") ||
            line.starts_with("export import "))
            return true;
        pos = eol == std::string::npos ? prefix.size() : eol + 1;
    }
    return false;
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
    // Characters are UTF-16 units; the fixture languages here are
    // ASCII-dominant, so units map 1:1 to bytes off the BMP.
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

class Server {
public:
    Server(kota::event_loop& loop, kota::ipc::JsonPeer& peer)
        : m_loop(loop), m_peer(peer) {}

    ~Server() { stop_background_index(); }

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
                start_background_index();
            });

        m_peer.on_notification([this]([[maybe_unused]] const proto::ExitParams& params) {
            // Exit code is protocol-driven: a clean handshake exit maps to
            // 0; the event loop's stop return value is not the server's
            // exit semantics. EOF without exit (crash/disconnect) leaves
            // the flag unset and lsp_main reports failure.
            stop_background_index();
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
                publish_diagnostics(path);
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
                publish_diagnostics(path);
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
                if (!ready() || !m_db) co_return nullptr;

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

                std::map<std::string, std::string> remapped;
                std::string file_spelling = command_file_spelling(
                    *entry, path);
                remapped[file_spelling] = doc->second.text;
                remapped[path] = doc->second.text;

                auto results = run_code_complete(
                    syntax_command(entry->command), remapped,
                    file_spelling, line, column);
                proto::CompletionList list;
                list.is_incomplete = false;
                for (auto& r : results) {
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
                if (!ready() || !m_db || !m_index) co_return nullptr;

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
                    m_index->index_tu(*entry, text);
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
                if (!ready() || !m_db || !m_index)
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
                    m_index->index_tu(*entry, text);
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
                            m_index->index_tu(*decl_entry, nullptr);
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
        auto db = build_compile_db(m_root, detect_host_target(), "dev",
                                   /*offline=*/true);
        if (!db) {
            std::println(std::cerr, "bake lsp: no compile commands: {}",
                         db.error());
            m_db.reset();
            return;
        }
        m_db = std::move(*db);
        m_index = std::make_unique<ProjectIndex>(
            get_toolchain_cache_root() / m_db->target_triple /
            "lsp-index");
        std::println(std::cerr, "bake lsp: {} compile entries, "
                                "{} index shards from cache",
                     m_db->entries.size(), m_index->shard_count());
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
        return get_toolchain_cache_root() / m_db->target_triple /
               "lsp-preamble" / "o" / key;
    }

    // Compile the file's current preamble into the cache and
    // record its state; returns false when the PCH cannot be
    // built (falling back to whole-file compiles).
    bool build_preamble(const CompileEntry& entry,
                         const std::string& path,
                         const std::string& file_spelling,
                         const std::string& text,
                         const std::string& key,
                         PreambleState& state) {
        Path dir = preamble_cache_dir(key);
        if (!dir.exists()) dir.mkdir_recursive();
        Path pch = dir / "preamble.pch";

        std::map<std::string, std::string> remapped;
        remapped[file_spelling] = text;
        remapped[path] = text;

        auto build = run_preamble_build(
            syntax_command(entry.command), remapped, file_spelling,
            pch.string());
        if (!build.ok) {
            std::println(std::cerr,
                         "bake lsp: preamble PCH build failed (status {}):",
                         build.outcome.status);
            for (const auto& d : build.outcome.diagnostics)
                std::println(std::cerr, "  [{}]: {}:{}", d.severity,
                             d.message, d.file);
            std::error_code ec;
            std::filesystem::remove_all(dir.fs(), ec);
            state.key.clear();
            return false;
        }
        state.key = key;
        state.pch_path = pch.string();
        state.size = build.preamble_size;
        state.ends_at_start_of_line = build.preamble_ends_at_start_of_line;
        return true;
    }

    static bool pch_load_failed(const CompileOutcome& outcome) {
        // Clang phrases stale/stale-mismatched PCHs as errors that
        // mention "PCH"; a wrong guess only costs one retry.
        for (const auto& diag : outcome.diagnostics) {
            if (diag.severity != "error" && diag.severity != "fatal")
                continue;
            if (diag.message.find("PCH") != std::string::npos)
                return true;
        }
        return false;
    }

    void publish_diagnostics(const std::string& path) {
        proto::PublishDiagnosticsParams params;
        params.uri = path_to_uri(path);

        if (m_db) {
            if (const CompileEntry* entry = m_db->find(Path(path))) {
                auto doc = m_documents.find(path);
                const std::string& text =
                    doc != m_documents.end() ? doc->second.text
                                             : std::string();

                std::string file_spelling =
                    command_file_spelling(*entry, path);
                std::map<std::string, std::string> remapped;
                if (doc != m_documents.end()) {
                    remapped[file_spelling] = text;
                    remapped[path] = text;
                }

                std::vector<std::string> command =
                    syntax_command(entry->command);

                // Preamble PCH: skipped for module-importing
                // preambles (PCH and PCM loading are exclusive).
                CompileOptions options;
                std::string prefix = cheap_prefix(text);
                if (doc != m_documents.end() && !prefix.empty() &&
                    !prefix_has_module_import(prefix)) {
                    std::string joined_command;
                    for (const auto& arg : entry->command)
                        joined_command += arg + '\n';
                    std::string key = SHA256::to_hex(SHA256::compute(
                        prefix + std::string(1, '\0') + joined_command));
                    PreambleState& state = m_preambles[path];
                    bool have = state.valid() && state.key == key &&
                                std::filesystem::exists(
                                    std::filesystem::path(state.pch_path));
                    if (!have)
                        have = build_preamble(*entry, path, file_spelling,
                                              text, key, state);
                    if (have)
                        options.pch = PchLoad{state.pch_path, state.size,
                                              state.ends_at_start_of_line};
                }

                CompileOutcome outcome =
                    run_syntax_check(command, remapped, options);

                // Disk headers changed under the recorded stamps:
                // rebuild the PCH once, then retry.
                if (!options.pch.path.empty() &&
                    pch_load_failed(outcome)) {
                    std::string key = m_preambles[path].key;
                    std::error_code ec;
                    std::filesystem::remove_all(
                        std::filesystem::path(options.pch.path).parent_path(),
                        ec);
                    m_preambles.erase(path);
                    CompileOptions retry;
                    if (!key.empty() &&
                        build_preamble(*entry, path, file_spelling, text,
                                       key, m_preambles[path]))
                        retry.pch = PchLoad{
                            m_preambles[path].pch_path,
                            m_preambles[path].size,
                            m_preambles[path].ends_at_start_of_line};
                    outcome = run_syntax_check(command, remapped, retry);
                }

                // Main-file diagnostics only: a header's diagnostics
                // belong to the header's own buffer. The comparison is
                // canonical on both sides: clang reports the resolved path
                // (/private/var on macOS) while the editor URI may carry
                // the unresolved spelling.
                std::string canonical_main = canonical_path_string(path);
                for (const auto& diag : outcome.diagnostics) {
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
    }

    void start_background_index() {
        if (!m_db || !m_index) return;
        CompileDb db = *m_db;
        std::string root = canonical_path_string(m_root.string());
        m_index_worker = std::async(std::launch::async,
            [this, db = std::move(db), root]() {
                std::size_t indexed = 0;
                for (const auto& entry : db.entries) {
                    if (m_shutdown.load()) return;
                    std::string canonical =
                        canonical_path_string(entry.file.string());
                    if (!canonical.starts_with(root + "/")) continue;
                    if (m_index->has_current_shard(entry)) continue;
                    m_index->index_tu(entry, nullptr);
                    ++indexed;
                    std::println(std::cerr,
                                 "bake lsp: indexed {} ({} shards)",
                                 entry.file.filename_string(),
                                 m_index->shard_count());
                }
                if (indexed > 0) m_index->enforce_quota();
                std::println(std::cerr,
                             "bake lsp: background indexing done "
                             "({} newly indexed, {} shards, {} occurrences)",
                             indexed, m_index->shard_count(),
                             m_index->occurrence_count());
            });
    }

    void stop_background_index() {
        m_shutdown.store(true);
        // The worker's next entry check observes the flag; the
        // future's destructor joins it.
        m_index_worker = {};
    }

    kota::event_loop& m_loop;
    kota::ipc::JsonPeer& m_peer;
    Lifecycle m_lifecycle = Lifecycle::Uninitialized;
    bool m_clean_exit = false;

    Path m_root;
    std::optional<CompileDb> m_db;
    std::unique_ptr<ProjectIndex> m_index;
    std::atomic<bool> m_shutdown{false};
    std::future<void> m_index_worker;
    std::map<std::string, Document> m_documents;
    std::map<std::string, PreambleState> m_preambles;
};

} // namespace

int lsp_main([[maybe_unused]] const std::vector<std::string>& args) {
    kota::event_loop loop;
    auto transport = kota::ipc::StreamTransport::open_stdio(loop);
    if (!transport) {
        std::println(std::cerr, "bake lsp: failed to open stdio: {}",
                     transport.error().message);
        return 1;
    }

    kota::ipc::JsonPeer peer(loop, std::move(*transport));
    Server server(loop, peer);
    server.register_handlers();

    loop.schedule(peer.run());
    int rc = loop.run();
    return server.clean_exit() ? 0 : (rc != 0 ? rc : 1);
}

} // namespace bake
