// bake.lsp — stage1 implementation: the language server.
//
// Lifecycle (initialize/initialized/shutdown/exit), full document
// sync (didOpen/didChange/didClose) and diagnostics. Compiles run
// in-process through bake.toolchain.compilation with
// the buffer remapped, against the command from
// bake.buildsystem.query — the same argv a build would run.

module;

#include <cstdio>

#include <kota/async/io/loop.h>
#include <kota/ipc/codec/json.h>
#include <kota/ipc/lsp/protocol.h>
#include <kota/ipc/lsp/uri.h>

module bake.lsp;

import std;
import bake.util;
import bake.toolchain.target;
import bake.toolchain.compilation;
import bake.buildsystem.query;

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

struct Document {
    std::string path;
    std::string text;
    proto::integer version = 0;
};

class Server {
public:
    Server(kota::event_loop& loop, kota::ipc::JsonPeer& peer)
        : m_loop(loop), m_peer(peer) {}

    void register_handlers() {
        // ── lifecycle ──

        m_peer.on_request(
            [this](kota::ipc::JsonPeer::RequestContext&,
                   const proto::InitializeParams& params)
                -> kota::ipc::RequestResult<proto::InitializeParams> {
                if (m_phase != Lifecycle::Uninitialized) {
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

                m_phase = Lifecycle::Initializing;

                proto::InitializeResult result;
                result.capabilities.text_document_sync =
                    proto::TextDocumentSyncOptions{
                        .open_close = true,
                        .change = proto::TextDocumentSyncKind::Full,
                    };
                result.server_info =
                    proto::ServerInfo{.name = "bake lsp"};
                co_return result;
            });

        m_peer.on_request(
            [this](kota::ipc::JsonPeer::RequestContext&,
                   const proto::ShutdownParams&)
                -> kota::ipc::RequestResult<proto::ShutdownParams> {
                m_phase = Lifecycle::Shutdown;
                co_return nullptr;
            });

        m_peer.on_notification(
            [this]([[maybe_unused]] const proto::InitializedParams& params) {
                // The compile database is built once the handshake
                // completes — commands come from the graph on disk.
                m_phase = Lifecycle::Ready;
                rebuild_compile_db();
            });

        m_peer.on_notification([this]([[maybe_unused]] const proto::ExitParams& params) {
            // Exit code is protocol-driven: a clean handshake exit maps to
            // 0; the event loop's stop return value is not the server's
            // exit semantics. EOF without exit (crash/disconnect) leaves
            // the flag unset and lsp_main reports failure.
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
                m_documents[open.path] = std::move(open);
                publish_diagnostics(m_documents.at(uri_to_path(doc.uri)).path);
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
                // Clear the published set for the closed file.
                proto::PublishDiagnosticsParams clear;
                clear.uri = path_to_uri(path);
                m_peer.send_notification(clear);
            });
    }

private:
    enum class Lifecycle { Uninitialized, Initializing, Ready, Shutdown };

    bool ready() const { return m_phase == Lifecycle::Ready; }

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
        std::println(std::cerr, "bake lsp: {} compile entries",
                     m_db->entries.size());
    }

    // Adapt a build command into a syntax-check invocation:
    // -c → -fsyntax-only, and the -o pair is dropped (no outputs).
    std::vector<std::string> syntax_command(
            const std::vector<std::string>& command) {
        std::vector<std::string> argv;
        argv.reserve(command.size());
        for (std::size_t i = 0; i < command.size(); ++i) {
            if (command[i] == "-c") {
                argv.push_back("-fsyntax-only");
                continue;
            }
            if (command[i] == "-o" && i + 1 < command.size()) {
                ++i;
                continue;
            }
            if (starts_with(command[i], "-o")) {
                if (command[i].size() == 2) continue;
                continue;
            }
            if (starts_with(command[i], "-fmodule-output"))
                continue;  // BMI production is the build system's job
            argv.push_back(command[i]);
        }
        return argv;
    }

    void publish_diagnostics(const std::string& path) {
        proto::PublishDiagnosticsParams params;
        params.uri = path_to_uri(path);

        if (m_db) {
            if (const CompileEntry* entry = m_db->find(Path(path))) {
                std::map<std::string, std::string> remapped;
                auto doc = m_documents.find(path);
                if (doc != m_documents.end())
                    remapped[path] = doc->second.text;

                CompileOutcome outcome =
                    run_syntax_check(syntax_command(entry->command),
                                     remapped);


                // Main-file diagnostics only: a header's diagnostics
                // belong to the header's own buffer. The comparison is
                // canonical on both sides: clang reports the resolved path
                // (/private/var on macOS) while the editor URI may carry
                // the unresolved spelling.
                std::filesystem::path canonical_main =
                    std::filesystem::weakly_canonical(
                        std::filesystem::path(path));
                for (const auto& diag : outcome.diagnostics) {
                    if (!diag.file.empty() &&
                        std::filesystem::weakly_canonical(
                            std::filesystem::path(diag.file)) !=
                            canonical_main)
                        continue;

                    proto::Diagnostic out;
                    proto::Position pos;
                    pos.line = diag.line > 0 ? diag.line - 1 : 0;
                    pos.character = diag.column > 0 ? diag.column - 1 : 0;
                    out.range = proto::Range{.start = pos, .end = pos};

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

    kota::event_loop& m_loop;
    kota::ipc::JsonPeer& m_peer;
    Lifecycle m_phase = Lifecycle::Uninitialized;
    bool m_clean_exit = false;

public:
    bool clean_exit() const { return m_clean_exit; }

private:
    Path m_root;
    std::optional<CompileDb> m_db;
    std::map<std::string, Document> m_documents;
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
