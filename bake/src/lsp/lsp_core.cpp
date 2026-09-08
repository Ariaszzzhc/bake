module;

#include <kota/async/io/loop.h>
#include <kota/ipc/codec/json.h>
#include <kota/ipc/lsp/protocol.h>
#include <kota/ipc/lsp/uri.h>

module bake.lsp;

import std;
import bake.cli;
import bake.core.analysis;
import bake.core.build;
import bake.core.index;
import bake.core.project;
import bake.core.runtime;
import bake.util;

namespace bake {

namespace proto = kota::ipc::protocol;

namespace {

std::string uri_to_path(const std::string& uri) {
    if (!starts_with(uri, "file://")) return uri;
    auto parsed = kota::ipc::lsp::URI::parse(uri);
    if (!parsed) return uri;
    auto decoded = parsed->percent_decode(parsed->path());
    return decoded ? *decoded : std::string(parsed->path());
}

std::string path_to_uri(const std::string& path) {
    auto uri = kota::ipc::lsp::URI::from_file_path(path);
    return uri ? uri->str() : path;
}

std::string canonical_path(const std::string& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path).lexically_normal();
    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    return error ? absolute.generic_string() : canonical.generic_string();
}

struct Document {
    std::string uri;
    std::string text;
    std::int64_t version = 0;
};

std::size_t position_to_offset(const std::string& text,
                               unsigned line, unsigned character) {
    std::size_t offset = 0;
    for (unsigned current = 0; current < line && offset < text.size(); ++current) {
        auto newline = text.find('\n', offset);
        if (newline == std::string::npos) return text.size();
        offset = newline + 1;
    }
    return std::min(text.size(), offset + character);
}

bool word_character(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
}

std::string word_at(const std::string& text, unsigned line,
                    unsigned character) {
    std::size_t offset = position_to_offset(text, line, character);
    std::size_t begin = offset;
    std::size_t end = offset;
    while (begin > 0 && word_character(text[begin - 1])) --begin;
    while (end < text.size() && word_character(text[end])) ++end;
    return text.substr(begin, end - begin);
}

proto::CompletionItemKind completion_kind(std::string_view kind) {
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

proto::Position position(unsigned line, unsigned column) {
    return proto::Position{line > 0 ? line - 1 : 0,
                           column > 0 ? column - 1 : 0};
}

proto::Location location(const IndexLocation& value) {
    proto::Position start = position(value.line, value.column);
    return proto::Location{path_to_uri(value.file),
                           proto::Range{.start = start, .end = start}};
}

proto::Error analysis_error(const AnalysisError& error) {
    return proto::Error(
        error.code == AnalysisErrorCode::cancelled ||
                error.code == AnalysisErrorCode::stale
            ? proto::ErrorCode::RequestCancelled
            : proto::ErrorCode::RequestFailed,
        error.message);
}

struct ServerOptions {
    std::optional<std::string> target;
    std::string profile = "dev";
    std::size_t index_jobs = 3;
};

class Server {
public:
    Server(kota::event_loop& loop, kota::ipc::JsonPeer& peer,
           ServerOptions options)
        : m_loop(loop), m_peer(peer), m_options(std::move(options)),
          m_runtime(loop, RuntimeLimits{
              .max_concurrency = m_options.index_jobs + 1,
              .background_concurrency = m_options.index_jobs}),
          m_projects(m_runtime), m_builds(m_runtime) {}

    void register_handlers() {
        m_peer.on_request(
            [this](kota::ipc::JsonPeer::RequestContext&,
                   const proto::InitializeParams& params)
                -> kota::ipc::RequestResult<proto::InitializeParams> {
                if (m_lifecycle != Lifecycle::uninitialized)
                    co_return kota::outcome_error(
                        proto::Error{"server already initialized"});
                const auto& initialize = params.lsp__initialize_params;
                if (initialize.root_uri && !initialize.root_uri->empty())
                    m_root = Path(uri_to_path(*initialize.root_uri));
                else if (initialize.root_path && *initialize.root_path &&
                         !(**initialize.root_path).empty())
                    m_root = Path(**initialize.root_path);
                if (m_root.string().empty())
                    co_return kota::outcome_error(proto::Error{
                        "bake lsp requires a workspace root"});
                m_lifecycle = Lifecycle::opening;
                proto::InitializeResult result;
                result.capabilities.text_document_sync =
                    proto::TextDocumentSyncOptions{
                        .open_close = true,
                        .change = proto::TextDocumentSyncKind::Full,
                        .save = true};
                result.capabilities.completion_provider =
                    proto::CompletionOptions{};
                result.capabilities.definition_provider = true;
                result.capabilities.references_provider = true;
                result.server_info = proto::ServerInfo{.name = "bake lsp"};
                co_return result;
            });

        m_peer.on_request(
            [this](kota::ipc::JsonPeer::RequestContext&,
                   const proto::ShutdownParams&)
                -> kota::ipc::RequestResult<proto::ShutdownParams> {
                m_lifecycle = Lifecycle::shutdown;
                co_return nullptr;
            });

        m_peer.on_notification(
            [this](const proto::InitializedParams&) {
                if (m_lifecycle == Lifecycle::opening)
                    m_loop.schedule(refresh_project());
            });

        m_peer.on_notification([this](const proto::ExitParams&) {
            if (m_exiting) return;
            m_exiting = true;
            m_loop.schedule(finish_exit());
        });

        m_peer.on_notification(
            [this](const proto::DidOpenTextDocumentParams& params) {
                const auto& incoming = params.text_document;
                std::string path = canonical_path(uri_to_path(incoming.uri));
                m_documents[path] = Document{
                    incoming.uri, incoming.text, incoming.version};
                if (!ready()) {
                    if (m_lifecycle == Lifecycle::failed)
                        publish_failure(path, m_project_error);
                    return;
                }
                auto updated = m_analysis->update_document(
                    m_session, Path(path), incoming.text, incoming.version);
                if (!updated) {
                    std::println(std::cerr, "bake lsp: {}", updated.error().message);
                    return;
                }
                schedule_impacted(*updated);
            });

        m_peer.on_notification(
            [this](const proto::DidChangeTextDocumentParams& params) {
                std::string path = canonical_path(
                    uri_to_path(params.text_document.uri));
                auto document = m_documents.find(path);
                if (document == m_documents.end()) return;
                if (!params.content_changes.empty()) {
                    const auto& change = params.content_changes.back();
                    if (const auto* whole = std::get_if<
                            proto::TextDocumentContentChangeWholeDocument>(
                            &change))
                        document->second.text = whole->text;
                }
                document->second.version = params.text_document.version;
                if (!ready()) return;
                auto updated = m_analysis->update_document(
                    m_session, Path(path), document->second.text,
                    document->second.version);
                if (!updated) {
                    std::println(std::cerr, "bake lsp: {}", updated.error().message);
                    return;
                }
                schedule_impacted(*updated);
            });

        m_peer.on_notification(
            [this](const proto::DidCloseTextDocumentParams& params) {
                std::string path = canonical_path(
                    uri_to_path(params.text_document.uri));
                std::vector<Path> impacted;
                if (ready()) {
                    auto closed = m_analysis->close_document(
                        m_session, Path(path));
                    if (closed) impacted = std::move(*closed);
                }
                std::string uri = params.text_document.uri;
                if (auto document = m_documents.find(path);
                    document != m_documents.end())
                    uri = document->second.uri;
                m_documents.erase(path);
                ++m_diagnostic_generation[path];
                proto::PublishDiagnosticsParams clear;
                clear.uri = std::move(uri);
                m_peer.send_notification(clear);
                schedule_impacted(impacted);
            });

        m_peer.on_notification(
            [this](const proto::DidSaveTextDocumentParams&) {
                schedule_refresh();
            });

        m_peer.on_notification(
            [this](const proto::DidChangeWatchedFilesParams&) {
                schedule_refresh();
            });

        m_peer.on_request(
            [this](kota::ipc::JsonPeer::RequestContext& context,
                   const proto::CompletionParams& params)
                -> kota::ipc::RequestResult<proto::CompletionParams> {
                if (!ready())
                    co_return kota::outcome_error(proto::Error(
                        proto::ErrorCode::RequestFailed,
                        m_lifecycle == Lifecycle::failed
                            ? "Bake project failed to open"
                            : "Bake project is preparing"));
                if (context.cancelled())
                    co_return kota::outcome_error(proto::Error(
                        proto::ErrorCode::RequestCancelled,
                        "completion request cancelled"));
                auto analysis = m_analysis;
                const auto& point = params.text_document_position_params;
                std::string path = canonical_path(uri_to_path(
                    point.text_document.uri));
                auto document = m_documents.find(path);
                if (document == m_documents.end()) co_return nullptr;
                const unsigned line = static_cast<unsigned>(point.position.line);
                const unsigned column =
                    static_cast<unsigned>(point.position.character);
                std::string prefix = word_at(
                    document->second.text, line, column);
                auto result = co_await analysis->complete(
                    m_session, Path(path), line + 1, column + 1);
                if (!result)
                    co_return kota::outcome_error(
                        analysis_error(result.error()));
                if (analysis != m_analysis ||
                    result->stamp.request.snapshot != analysis->snapshot_id())
                    co_return kota::outcome_error(proto::Error(
                        proto::ErrorCode::RequestFailed,
                        "Bake project changed during completion"));
                proto::CompletionList list;
                list.is_incomplete = false;
                for (const auto& candidate : result->items) {
                    if (!prefix.empty() &&
                        !candidate.label.starts_with(prefix))
                        continue;
                    proto::CompletionItem item;
                    item.label = candidate.label;
                    item.kind = completion_kind(candidate.kind);
                    item.detail = candidate.detail.empty()
                        ? candidate.kind : candidate.detail;
                    item.sort_text = std::format(
                        "{:06}{}", std::clamp(candidate.priority, 0, 999999),
                        candidate.label);
                    list.items.push_back(std::move(item));
                }
                co_return list;
            });

        m_peer.on_request(
            [this](kota::ipc::JsonPeer::RequestContext& context,
                   const proto::DefinitionParams& params)
                -> kota::ipc::RequestResult<proto::DefinitionParams> {
                if (!ready())
                    co_return kota::outcome_error(proto::Error(
                        proto::ErrorCode::RequestFailed,
                        m_lifecycle == Lifecycle::failed
                            ? "Bake project failed to open"
                            : "Bake project is preparing"));
                if (context.cancelled())
                    co_return kota::outcome_error(proto::Error(
                        proto::ErrorCode::RequestCancelled,
                        "definition request cancelled"));
                auto analysis = m_analysis;
                const auto& point = params.text_document_position_params;
                std::string path = canonical_path(uri_to_path(
                    point.text_document.uri));
                auto document = m_documents.find(path);
                std::string fallback;
                if (document != m_documents.end())
                    fallback = word_at(
                        document->second.text,
                        static_cast<unsigned>(point.position.line),
                        static_cast<unsigned>(point.position.character));
                auto result = co_await analysis->definitions(
                    m_session, Path(path),
                    static_cast<unsigned>(point.position.line) + 1,
                    static_cast<unsigned>(point.position.character) + 1,
                    std::move(fallback));
                if (!result)
                    co_return kota::outcome_error(
                        analysis_error(result.error()));
                if (analysis != m_analysis ||
                    result->stamp.request.snapshot != analysis->snapshot_id())
                    co_return kota::outcome_error(proto::Error(
                        proto::ErrorCode::RequestFailed,
                        "Bake project changed during definition"));
                std::vector<proto::Location> locations;
                for (const auto& value : result->locations)
                    locations.push_back(location(value));
                co_return proto::Definition{std::move(locations)};
            });

        m_peer.on_request(
            [this](kota::ipc::JsonPeer::RequestContext& context,
                   const proto::ReferenceParams& params)
                -> kota::ipc::RequestResult<proto::ReferenceParams> {
                if (!ready())
                    co_return kota::outcome_error(proto::Error(
                        proto::ErrorCode::RequestFailed,
                        m_lifecycle == Lifecycle::failed
                            ? "Bake project failed to open"
                            : "Bake project is preparing"));
                if (context.cancelled())
                    co_return kota::outcome_error(proto::Error(
                        proto::ErrorCode::RequestCancelled,
                        "references request cancelled"));
                auto analysis = m_analysis;
                const auto& point = params.text_document_position_params;
                std::string path = canonical_path(uri_to_path(
                    point.text_document.uri));
                auto result = co_await analysis->references(
                    m_session, Path(path),
                    static_cast<unsigned>(point.position.line) + 1,
                    static_cast<unsigned>(point.position.character) + 1,
                    params.context.include_declaration);
                if (!result)
                    co_return kota::outcome_error(
                        analysis_error(result.error()));
                if (analysis != m_analysis ||
                    result->stamp.request.snapshot != analysis->snapshot_id())
                    co_return kota::outcome_error(proto::Error(
                        proto::ErrorCode::RequestFailed,
                        "Bake project changed during references"));
                std::vector<proto::Location> locations;
                for (const auto& value : result->locations)
                    locations.push_back(location(value));
                co_return locations;
            });
    }

    bool clean_exit() const { return m_clean_exit; }

    kota::task<> close_after_disconnect() {
        m_exiting = true;
        if (m_analysis) m_analysis->close_session(m_session);
        co_await m_runtime.shutdown();
        m_loop.stop();
        co_return;
    }

private:
    enum class Lifecycle { uninitialized, opening, ready, failed, shutdown };

    bool ready() const {
        return m_lifecycle == Lifecycle::ready && bool(m_analysis);
    }

    ProjectRequest project_request() const {
        ProjectRequest request;
        request.root = m_root;
        request.profile = m_options.profile;
        request.target_triple = m_options.target;
        request.offline = true;
        request.compiler_executable = Path(bake_exe_path());
        request.compiler_version = cli::bake_version();
        return request;
    }

    void schedule_refresh() {
        if (m_exiting || m_lifecycle == Lifecycle::shutdown) return;
        if (m_refreshing) {
            m_refresh_pending = true;
            return;
        }
        m_loop.schedule(refresh_project());
    }

    kota::task<> refresh_project() {
        if (m_refreshing) {
            m_refresh_pending = true;
            co_return;
        }
        m_refreshing = true;
        m_lifecycle = Lifecycle::opening;
        auto project = co_await m_projects.open(project_request());
        if (m_exiting || m_lifecycle == Lifecycle::shutdown) {
            m_refreshing = false;
            co_return;
        }
        if (!project) {
            m_lifecycle = Lifecycle::failed;
            m_project_error = project.error().message;
            std::println(std::cerr, "bake lsp: project open failed: {}",
                         m_project_error);
            for (const auto& [path, document] : m_documents) {
                (void)document;
                publish_failure(path, m_project_error);
            }
        } else {
            m_project_error.clear();
            std::println(std::cerr, "bake lsp: project {}",
                         format_project_metrics(project->metrics));
            for (auto& [path, generation] : m_diagnostic_generation) {
                (void)path;
                ++generation;
            }
            if (m_analysis) m_analysis->close_session(m_session);
            auto replacement = std::make_shared<CoreAnalysisService>(
                m_runtime, m_builds, std::move(*project));
            AnalysisSessionId replacement_session =
                replacement->open_session();
            for (const auto& [path, document] : m_documents) {
                auto updated = replacement->update_document(
                    replacement_session, Path(path), document.text,
                    document.version);
                if (!updated)
                    std::println(std::cerr, "bake lsp: {}",
                                 updated.error().message);
            }
            m_analysis = std::move(replacement);
            m_session = std::move(replacement_session);
            m_lifecycle = Lifecycle::ready;
            for (const auto& [path, document] : m_documents) {
                (void)document;
                schedule_diagnostics(path);
            }
            m_loop.schedule(run_background_index(m_analysis, m_session));
        }
        m_refreshing = false;
        if (std::exchange(m_refresh_pending, false)) schedule_refresh();
        co_return;
    }

    void schedule_diagnostics(const std::string& path) {
        const std::uint64_t generation = ++m_diagnostic_generation[path];
        m_loop.schedule(publish_diagnostics(path, generation));
    }

    static kota::task<> run_background_index(
            std::shared_ptr<CoreAnalysisService> analysis,
            AnalysisSessionId session) {
        co_await analysis->background_index(session);
    }

    void schedule_impacted(const std::vector<Path>& paths) {
        for (const auto& path : paths) {
            const std::string canonical = canonical_path(path.string());
            if (m_documents.contains(canonical))
                schedule_diagnostics(canonical);
        }
    }

    std::string document_uri(const std::string& path) const {
        const std::string canonical = canonical_path(path);
        auto document = m_documents.find(canonical);
        return document == m_documents.end()
            ? path_to_uri(path) : document->second.uri;
    }

    void publish_failure(const std::string& path,
                         const std::string& message) {
        proto::Diagnostic diagnostic;
        diagnostic.range = proto::Range{};
        diagnostic.severity = proto::DiagnosticSeverity::Error;
        diagnostic.message = message.empty()
            ? "Bake project or analysis preparation failed" : message;
        diagnostic.source = "bake";
        proto::PublishDiagnosticsParams params;
        params.uri = document_uri(path);
        params.diagnostics.push_back(std::move(diagnostic));
        m_peer.send_notification(params);
    }

    kota::task<> publish_diagnostics(std::string path,
                                     std::uint64_t generation) {
        auto analysis = m_analysis;
        auto result = co_await analysis->diagnostics(m_session, Path(path));
        if (generation != m_diagnostic_generation[path] ||
            analysis != m_analysis)
            co_return;
        proto::PublishDiagnosticsParams params;
        params.uri = document_uri(path);
        if (result) {
            for (const auto& diagnostic : result->diagnostics) {
                if (!diagnostic.file.empty() &&
                    canonical_path(diagnostic.file) != path)
                    continue;
                proto::Diagnostic value;
                proto::Position start = position(
                    diagnostic.line, diagnostic.column);
                value.range = proto::Range{.start = start, .end = start};
                if (diagnostic.severity == "fatal" ||
                    diagnostic.severity == "error")
                    value.severity = proto::DiagnosticSeverity::Error;
                else if (diagnostic.severity == "warning")
                    value.severity = proto::DiagnosticSeverity::Warning;
                else if (diagnostic.severity == "remark")
                    value.severity = proto::DiagnosticSeverity::Information;
                value.message = diagnostic.message;
                value.source = "bake";
                params.diagnostics.push_back(std::move(value));
            }
        } else if (result.error().code == AnalysisErrorCode::cancelled ||
                   result.error().code == AnalysisErrorCode::stale) {
            co_return;
        } else {
            std::println(std::cerr, "bake lsp: analysis failed for {}: {}",
                         path, result.error().message);
            proto::Diagnostic diagnostic;
            diagnostic.range = proto::Range{};
            diagnostic.severity = proto::DiagnosticSeverity::Error;
            diagnostic.message = result.error().message;
            diagnostic.source = "bake";
            params.diagnostics.push_back(std::move(diagnostic));
        }
        m_peer.send_notification(params);
        co_return;
    }

    kota::task<> finish_exit() {
        if (m_analysis) m_analysis->close_session(m_session);
        co_await m_runtime.shutdown();
        m_clean_exit = m_lifecycle == Lifecycle::shutdown;
        m_loop.stop();
        co_return;
    }

    kota::event_loop& m_loop;
    kota::ipc::JsonPeer& m_peer;
    ServerOptions m_options;
    CoreRuntime m_runtime;
    ProjectCore m_projects;
    CoreBuildService m_builds;
    std::shared_ptr<CoreAnalysisService> m_analysis;
    AnalysisSessionId m_session;
    Lifecycle m_lifecycle = Lifecycle::uninitialized;
    Path m_root;
    std::map<std::string, Document> m_documents;
    std::map<std::string, std::uint64_t> m_diagnostic_generation;
    bool m_clean_exit = false;
    bool m_exiting = false;
    bool m_refreshing = false;
    bool m_refresh_pending = false;
    std::string m_project_error;
};

}

int lsp_main(const std::vector<std::string>& args) {
    ServerOptions options;
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    options.index_jobs = std::clamp<std::size_t>(hardware / 2, 1, 6);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& argument = args[i];
        auto value = [&]() {
            auto equal = argument.find('=');
            return equal == std::string::npos
                ? (i + 1 < args.size() ? args[i + 1] : std::string())
                : argument.substr(equal + 1);
        };
        auto is = [&](std::string_view name) {
            return argument == name ||
                   argument.starts_with(std::string(name) + "=");
        };
        if (is("--target")) {
            options.target = value();
            if (!argument.contains('=') && i + 1 < args.size()) ++i;
        } else if (is("--profile")) {
            options.profile = value();
            if (!argument.contains('=') && i + 1 < args.size()) ++i;
        } else if (is("--index-jobs")) {
            long count = std::atol(value().c_str());
            if (count > 0) options.index_jobs = static_cast<std::size_t>(count);
            if (!argument.contains('=') && i + 1 < args.size()) ++i;
        } else {
            std::println(std::cerr, "bake lsp: unknown option '{}'", argument);
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
    int result = loop.run();
    if (!server.clean_exit()) {
        loop.schedule(server.close_after_disconnect());
        (void)loop.run();
    }
    return server.clean_exit() ? 0 : (result == 0 ? 1 : result);
}

}
