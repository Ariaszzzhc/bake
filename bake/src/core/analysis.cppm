module;

#include <kota/async/runtime/task.h>

export module bake.core.analysis;

import std;
import bake.buildsystem.plan;
import bake.core.analysis.proto;
import bake.core.build;
import bake.core.index;
import bake.core.model;
import bake.core.project;
import bake.core.runtime;
import bake.core.snapshot;
import bake.core.state;
import bake.json;
import bake.toolchain.compilation;
import bake.util;

namespace bake {

export struct AnalysisSessionId {
    std::string value;
    auto operator<=>(const AnalysisSessionId&) const = default;
};

export struct SourceFileView {
    Path path;
    std::string content;
    std::string digest;
    std::optional<std::int64_t> document_version;
};

export struct SourceView {
    SourceViewId id;
    AnalysisSessionId session;
    std::uint64_t epoch = 0;
    std::map<std::string, SourceFileView> files;

    const SourceFileView* find(const Path& path) const {
        std::error_code error;
        auto canonical = std::filesystem::weakly_canonical(path.fs(), error);
        std::string key = error ? path.absolute().lexically_normal().string()
                                : canonical.generic_string();
        auto found = files.find(key);
        return found == files.end() ? nullptr : &found->second;
    }

    std::map<std::string, std::string> remapped_files() const {
        std::map<std::string, std::string> result;
        for (const auto& [key, file] : files)
            result.emplace(key, file.content);
        return result;
    }
};

export enum class AnalysisErrorCode { failed, cancelled, stale };

export struct AnalysisError {
    std::string message;
    AnalysisErrorCode code = AnalysisErrorCode::failed;
};

namespace analysis_detail {

std::string canonical_path(const Path& path) {
    std::error_code error;
    auto absolute = path.absolute().lexically_normal().fs();
    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    return error ? absolute.generic_string() : canonical.generic_string();
}

SourceViewId source_view_id(const ProjectContextKey& context,
                            const ProjectSnapshotId& snapshot,
                            const AnalysisSessionId& session,
                            std::uint64_t epoch,
                            const std::map<std::string, SourceFileView>& files) {
    std::string identity = "bake-source-view-v1\n" + context.value + "\n" +
                           snapshot.value + "\n" + session.value + "\n" +
                           std::to_string(epoch) + "\n";
    for (const auto& [path, file] : files) {
        identity += std::to_string(path.size()) + ":" + path + "\n";
        identity += file.digest + "\n";
        identity += file.document_version
            ? std::to_string(*file.document_version)
            : "disk";
        identity += '\n';
    }
    return SourceViewId{SHA256::hex(identity)};
}

}  // namespace analysis_detail

export class SourceViewStore {
public:
    SourceViewStore(ProjectContextKey context, ProjectSnapshotId snapshot)
        : m_context(std::move(context)), m_snapshot(std::move(snapshot)) {}

    AnalysisSessionId open_session() {
        std::lock_guard lock(m_mutex);
        AnalysisSessionId id{SHA256::hex(
            m_context.value + "\n" + m_snapshot.value + "\n" +
            std::to_string(++m_next_session))};
        m_sessions.emplace(id.value, Session{});
        return id;
    }

    std::expected<void, AnalysisError> update(
            const AnalysisSessionId& session, const Path& path,
            std::string content, std::int64_t version) {
        std::lock_guard lock(m_mutex);
        auto found = m_sessions.find(session.value);
        if (found == m_sessions.end())
            return std::unexpected(AnalysisError{
                "analysis session is closed", AnalysisErrorCode::stale});
        const std::string key = analysis_detail::canonical_path(path);
        auto current = found->second.overlays.find(key);
        if (current != found->second.overlays.end() &&
            version <= current->second.version) {
            return std::unexpected(AnalysisError{
                "document version must increase within an analysis session"});
        }
        found->second.overlays[key] = Overlay{Path(key), std::move(content),
                                              version};
        ++found->second.epoch;
        return {};
    }

    std::expected<void, AnalysisError> close_document(
            const AnalysisSessionId& session, const Path& path) {
        std::lock_guard lock(m_mutex);
        auto found = m_sessions.find(session.value);
        if (found == m_sessions.end())
            return std::unexpected(AnalysisError{
                "analysis session is closed", AnalysisErrorCode::stale});
        found->second.overlays.erase(analysis_detail::canonical_path(path));
        ++found->second.epoch;
        return {};
    }

    void close_session(const AnalysisSessionId& session) {
        std::lock_guard lock(m_mutex);
        m_sessions.erase(session.value);
    }

    std::expected<SourceView, AnalysisError> capture(
            const AnalysisSessionId& session,
            std::vector<Path> disk_dependencies = {}) const {
        std::lock_guard lock(m_mutex);
        auto found = m_sessions.find(session.value);
        if (found == m_sessions.end())
            return std::unexpected(AnalysisError{
                "analysis session is closed", AnalysisErrorCode::stale});

        std::map<std::string, SourceFileView> files;
        for (const auto& [key, overlay] : found->second.overlays) {
            files.emplace(key, SourceFileView{
                overlay.path, overlay.content, SHA256::hex(overlay.content),
                overlay.version});
        }
        for (const auto& path : disk_dependencies) {
            const std::string key = analysis_detail::canonical_path(path);
            if (files.contains(key)) continue;
            auto content = read_file(Path(key));
            if (!content)
                return std::unexpected(AnalysisError{
                    "analysis input is not readable: " + key});
            files.emplace(key, SourceFileView{
                Path(key), *content, SHA256::hex(*content), std::nullopt});
        }
        SourceView view;
        view.session = session;
        view.epoch = found->second.epoch;
        view.files = std::move(files);
        view.id = analysis_detail::source_view_id(
            m_context, m_snapshot, session, view.epoch, view.files);
        return view;
    }

    bool is_current(const SourceView& view) const {
        std::lock_guard lock(m_mutex);
        auto found = m_sessions.find(view.session.value);
        if (found == m_sessions.end() || found->second.epoch != view.epoch)
            return false;
        for (const auto& [path, file] : view.files) {
            if (file.document_version) {
                auto overlay = found->second.overlays.find(path);
                if (overlay == found->second.overlays.end() ||
                    overlay->second.version != *file.document_version ||
                    SHA256::hex(overlay->second.content) != file.digest)
                    return false;
            } else {
                auto content = read_file(Path(path));
                if (!content || SHA256::hex(*content) != file.digest)
                    return false;
            }
        }
        return true;
    }

private:
    struct Overlay {
        Path path;
        std::string content;
        std::int64_t version = 0;
    };
    struct Session {
        std::uint64_t epoch = 0;
        std::map<std::string, Overlay> overlays;
    };

    ProjectContextKey m_context;
    ProjectSnapshotId m_snapshot;
    mutable std::mutex m_mutex;
    std::uint64_t m_next_session = 0;
    std::map<std::string, Session> m_sessions;
};

export std::string preamble_identity(
        const CompilationDescription& description,
        const CompilerIdentity& compiler, const SourceView& source_view,
        std::size_t exact_preamble_size,
        const std::vector<InputIdentity>& dependencies) {
    std::string identity = "bake-preamble-v1\n";
    identity += description.context.value + "\n";
    for (const auto& argument : analysis_command(description, compiler))
        identity += std::to_string(argument.size()) + ":" + argument + "\n";
    const SourceFileView* main = source_view.find(description.source);
    if (main) {
        const auto size = std::min(exact_preamble_size, main->content.size());
        identity += SHA256::hex(std::string_view(main->content).substr(0, size));
        identity += '\n';
    }
    auto ordered_dependencies = dependencies;
    std::ranges::sort(ordered_dependencies, {}, [](const auto& input) {
        return input.path.string();
    });
    for (const auto& input : ordered_dependencies)
        if (!main || analysis_detail::canonical_path(input.path) !=
                         analysis_detail::canonical_path(main->path))
            identity += input.path.string() + "\n" + input.digest + "\n";
    return SHA256::hex(identity);
}

export struct AnalysisDiagnostics {
    AnalysisResultStamp stamp;
    std::vector<SourceDiagnostic> diagnostics;
};

export struct AnalysisCompletions {
    AnalysisResultStamp stamp;
    std::vector<CompletionItemData> items;
};

export struct AnalysisLocations {
    AnalysisResultStamp stamp;
    std::vector<IndexLocation> locations;
};

namespace analysis_detail {

std::size_t module_region_limit(std::string_view text) {
    std::size_t offset = 0;
    bool block_comment = false;
    while (offset < text.size()) {
        std::size_t newline = text.find('\n', offset);
        std::size_t end = newline == std::string_view::npos
            ? text.size() : newline;
        std::string_view line = text.substr(offset, end - offset);
        while (!line.empty() && std::isspace(
                   static_cast<unsigned char>(line.front())))
            line.remove_prefix(1);
        if (block_comment) {
            auto close = line.find("*/");
            if (close == std::string_view::npos) {
                offset = newline == std::string_view::npos
                    ? text.size() : newline + 1;
                continue;
            }
            block_comment = false;
            line.remove_prefix(close + 2);
            while (!line.empty() && std::isspace(
                       static_cast<unsigned char>(line.front())))
                line.remove_prefix(1);
        }
        if (line.starts_with("//") || line.empty()) {
            offset = newline == std::string_view::npos
                ? text.size() : newline + 1;
            continue;
        }
        if (line.starts_with("/*")) {
            block_comment = true;
            offset = newline == std::string_view::npos
                ? text.size() : newline + 1;
            continue;
        }
        if (line.starts_with("module;") || line.starts_with("module ") ||
            line.starts_with("import ") ||
            line.starts_with("export module ") ||
            line.starts_with("export import "))
            return offset;
        offset = newline == std::string_view::npos
            ? text.size() : newline + 1;
    }
    return std::size_t(-1);
}

std::string command_file_spelling(const CompilationDescription& description) {
    const auto absolute = description.source.absolute().lexically_normal();
    for (const auto& argument : description.driver_arguments) {
        Path candidate(argument);
        if (!candidate.is_cpp() && !candidate.is_c() &&
            !candidate.is_module_interface())
            continue;
        Path resolved = candidate.fs().is_absolute()
            ? candidate.lexically_normal()
            : (description.working_directory / candidate).lexically_normal();
        if (canonical_path(resolved) == canonical_path(absolute))
            return argument;
    }
    return description.source.string();
}

std::vector<Path> dependency_paths(const ProjectSnapshot& snapshot,
                                   const CompilationDescription& description) {
    std::map<std::string, const SourceUnitRecord*> providers;
    for (const auto& module : description.module_references) {
        auto producer = std::ranges::find_if(
            snapshot.build_graph.actions, [&](const BuildAction& action) {
                return std::ranges::any_of(
                    action.outputs, [&](const Path& output) {
                        return canonical_path(output) ==
                               canonical_path(module.bmi);
                    });
            });
        if (producer == snapshot.build_graph.actions.end()) continue;
        auto unit = std::ranges::find_if(
            snapshot.source_graph.units,
            [&](const SourceUnitRecord& value) {
                return value.id.value == producer->id &&
                       value.provides_module ==
                           std::optional<std::string>(module.name);
            });
        if (unit != snapshot.source_graph.units.end())
            providers[module.name] = &*unit;
    }
    std::vector<Path> result;
    std::set<std::string> visited;
    std::function<void(const SourceUnitRecord&)> add =
        [&](const SourceUnitRecord& unit) {
            if (!visited.insert(unit.id.value).second) return;
            result.push_back(unit.source.path);
            for (const auto& header : unit.headers) result.push_back(header.path);
            for (const auto& imported : unit.imports) {
                auto provider = providers.find(imported);
                if (provider != providers.end()) add(*provider->second);
            }
        };
    for (const auto& unit : snapshot.source_graph.units)
        if (unit.id == description.id) add(unit);
    std::ranges::sort(result, {}, [](const Path& path) {
        return canonical_path(path);
    });
    result.erase(std::unique(result.begin(), result.end(),
                             [](const Path& left, const Path& right) {
        return canonical_path(left) == canonical_path(right);
    }), result.end());
    return result;
}

const InputIdentity* snapshot_input(const ProjectSnapshot& snapshot,
                                    const Path& path) {
    const std::string target = canonical_path(path);
    for (const auto& source : snapshot.source_graph.units) {
        if (canonical_path(source.source.path) == target)
            return &source.source;
        auto header = std::ranges::find_if(
            source.headers, [&](const InputIdentity& candidate) {
                return canonical_path(candidate.path) == target;
            });
        if (header != source.headers.end()) return &*header;
    }
    return nullptr;
}

std::vector<InputIdentity> identities(const SourceView& view,
                                      const std::vector<Path>& paths) {
    std::vector<InputIdentity> result;
    result.reserve(paths.size());
    for (const auto& path : paths) {
        const auto* file = view.find(path);
        if (file)
            result.push_back(InputIdentity{
                file->path, file->digest, file->content.size()});
    }
    return result;
}

std::vector<SourceDiagnostic> diagnostics_from_wire(
        std::vector<analysis_proto::WireDiagnostic> input) {
    std::vector<SourceDiagnostic> result;
    result.reserve(input.size());
    for (auto& item : input)
        result.push_back(SourceDiagnostic{
            std::move(item.severity), std::move(item.file), item.line,
            item.column, std::move(item.message)});
    return result;
}

std::vector<CompletionItemData> completions_from_wire(
        std::vector<analysis_proto::WireCompletion> input) {
    std::vector<CompletionItemData> result;
    result.reserve(input.size());
    for (auto& item : input)
        result.push_back(CompletionItemData{
            std::move(item.label), std::move(item.detail),
            std::move(item.kind), item.priority});
    return result;
}

struct PreambleRecord {
    std::string context;
    std::string dependency_identity;
    std::string prefix_digest;
    analysis_proto::WirePchLoad load;
};

std::string preamble_dependencies(const SourceView& view,
                                  const Path& main_file,
                                  const std::vector<Path>& paths) {
    const std::string main = canonical_path(main_file);
    std::string identity;
    for (const auto& path : paths) {
        const std::string key = canonical_path(path);
        const auto* file = view.find(path);
        if (key != main && file)
            identity += key + "\n" + file->digest + "\n";
    }
    return SHA256::hex(identity);
}

std::string preamble_record_json(const PreambleRecord& record) {
    json::Value root = json::Value::object();
    root["schema"] = "bake-preamble-receipt-v1";
    root["context"] = record.context;
    root["dependencies"] = record.dependency_identity;
    root["prefix"] = record.prefix_digest;
    root["path"] = record.load.path;
    root["size"] = std::int64_t(record.load.size);
    root["start_of_line"] = record.load.ends_at_start_of_line;
    root["region"] = record.load.region_limit == std::size_t(-1)
        ? std::int64_t(-1)
        : static_cast<std::int64_t>(record.load.region_limit);
    return root.dump();
}

std::optional<PreambleRecord> parse_preamble_record(std::string_view content) {
    try {
        json::Value root = json::Value::parse(content);
        const auto* schema = root.find("schema");
        const auto* context = root.find("context");
        const auto* dependencies = root.find("dependencies");
        const auto* prefix = root.find("prefix");
        const auto* path = root.find("path");
        const auto* size = root.find("size");
        const auto* start = root.find("start_of_line");
        const auto* region = root.find("region");
        if (!root.is_object() || root.size() != 8 || !schema || !context ||
            !dependencies || !prefix || !path || !size || !start || !region ||
            !schema->is_string() || !context->is_string() ||
            !dependencies->is_string() || !prefix->is_string() ||
            !path->is_string() || !size->is_integer() ||
            !start->is_boolean() || !region->is_integer() ||
            schema->get<std::string>() != "bake-preamble-receipt-v1")
            return std::nullopt;
        const auto size_value = size->get<std::int64_t>();
        const auto region_value = region->get<std::int64_t>();
        if (size_value < 0 || region_value < -1) return std::nullopt;
        PreambleRecord record;
        record.context = context->get<std::string>();
        record.dependency_identity = dependencies->get<std::string>();
        record.prefix_digest = prefix->get<std::string>();
        record.load.path = path->get<std::string>();
        record.load.size = static_cast<unsigned>(size_value);
        record.load.ends_at_start_of_line = start->get<bool>();
        record.load.region_limit = region_value == -1
            ? std::size_t(-1) : static_cast<std::size_t>(region_value);
        return record;
    } catch (const json::Error&) {
        return std::nullopt;
    }
}

bool pch_load_failed(const analysis_proto::WireResponse& response) {
    return std::ranges::any_of(response.diagnostics, [](const auto& diagnostic) {
        return (diagnostic.severity == "error" ||
                diagnostic.severity == "fatal") &&
               diagnostic.message.find("PCH") != std::string::npos;
    });
}

std::expected<analysis_proto::WireResponse, AnalysisError> worker_response(
        const RuntimeResult& result) {
    if (!result.succeeded()) {
        const bool cancelled = result.state ==
                RuntimeRequestState::cancelled_before_start ||
            result.state == RuntimeRequestState::cancelled_after_start ||
            result.state == RuntimeRequestState::running_cancel_requested;
        return std::unexpected(AnalysisError{
            result.error, cancelled ? AnalysisErrorCode::cancelled
                                    : AnalysisErrorCode::failed});
    }
    const auto* reply = std::any_cast<WorkerReply>(&result.value);
    if (!reply)
        return std::unexpected(AnalysisError{
            "analysis worker returned an unexpected runtime value"});
    auto response = analysis_proto::decode_response(reply->payload);
    if (!response)
        return std::unexpected(AnalysisError{response.error()});
    if (!response->ok)
        return std::unexpected(AnalysisError{response->error});
    return std::move(*response);
}

std::expected<std::optional<IndexShard>, AnalysisError> restored_shard(
        RuntimeResult& result) {
    if (!result.succeeded()) {
        const bool cancelled = result.state ==
                RuntimeRequestState::cancelled_before_start ||
            result.state == RuntimeRequestState::cancelled_after_start ||
            result.state == RuntimeRequestState::running_cancel_requested;
        return std::unexpected(AnalysisError{
            result.error, cancelled ? AnalysisErrorCode::cancelled
                                    : AnalysisErrorCode::failed});
    }
    auto* shard = std::any_cast<std::optional<IndexShard>>(&result.value);
    if (!shard)
        return std::unexpected(AnalysisError{
            "index restore returned an unexpected runtime value"});
    return std::move(*shard);
}

}

export class CoreAnalysisService {
public:
    CoreAnalysisService(CoreRuntime& runtime, CoreBuildService& builds,
                        ProjectHandle project)
        : m_runtime(runtime), m_builds(builds), m_project(std::move(project)),
          m_views(*context_key(m_project.context), m_project.snapshot_id),
          m_index(m_project.paths.index, m_project.paths) {
        WorkerPoolOptions options;
        options.worker.file = m_project.context.compiler.executable.string();
        options.worker.args = {options.worker.file, "lsp-worker"};
        options.worker.cwd = m_project.context.workspace_root.string();
        m_workers = m_runtime.create_worker_pool(std::move(options));
    }

    AnalysisSessionId open_session() { return m_views.open_session(); }
    const ProjectSnapshotId& snapshot_id() const {
        return m_project.snapshot_id;
    }

    std::expected<std::vector<Path>, AnalysisError> update_document(
            const AnalysisSessionId& session, const Path& path,
            std::string content, std::int64_t version) {
        cancel_session_requests(session);
        m_index.invalidate(path);
        auto updated = m_views.update(
            session, path, std::move(content), version);
        if (!updated) return std::unexpected(updated.error());
        return impacted_documents(path);
    }

    std::expected<std::vector<Path>, AnalysisError> close_document(
            const AnalysisSessionId& session, const Path& path) {
        cancel_session_requests(session);
        m_index.invalidate(path);
        auto closed = m_views.close_document(session, path);
        if (!closed) return std::unexpected(closed.error());
        return impacted_documents(path);
    }

    void close_session(const AnalysisSessionId& session) {
        cancel_session_requests(session, true);
        m_views.close_session(session);
    }

    const CompilationDescription* compilation_for(const Path& path) const {
        const std::string target = analysis_detail::canonical_path(path);
        for (const auto& compilation : m_project.snapshot->compilations)
            if (analysis_detail::canonical_path(compilation.source) == target)
                return &compilation;
        for (const auto& unit : m_project.snapshot->source_graph.units) {
            const bool header = std::ranges::any_of(
                unit.headers, [&](const InputIdentity& input) {
                    return analysis_detail::canonical_path(input.path) == target;
                });
            if (!header) continue;
            for (const auto& compilation : m_project.snapshot->compilations)
                if (compilation.id == unit.id) return &compilation;
        }
        return nullptr;
    }

    kota::task<std::expected<AnalysisDiagnostics, AnalysisError>> diagnostics(
        const AnalysisSessionId& session, const Path& document);
    kota::task<std::expected<AnalysisCompletions, AnalysisError>> complete(
        const AnalysisSessionId& session, const Path& document,
        unsigned line, unsigned column);
    kota::task<std::expected<AnalysisLocations, AnalysisError>> definitions(
        const AnalysisSessionId& session, const Path& document,
        unsigned line, unsigned column, std::string fallback_name = {});
    kota::task<std::expected<AnalysisLocations, AnalysisError>> references(
        const AnalysisSessionId& session, const Path& document,
        unsigned line, unsigned column, bool include_declaration);
    kota::task<> background_index(const AnalysisSessionId& session);

private:
    struct Prepared {
        const CompilationDescription* compilation = nullptr;
        SourceView view;
        AnalysisRequestStamp stamp;
        std::vector<std::string> command;
        std::map<std::string, std::string> remaps;
        std::string main_file;
        std::vector<Path> dependencies;
    };

    std::expected<Prepared, AnalysisError> prepare(
        const AnalysisSessionId& session, const Path& document);
    kota::task<std::expected<analysis_proto::WireResponse, AnalysisError>> call(
        analysis_proto::WireRequest request, const Prepared& prepared,
        RuntimeLane lane = RuntimeLane::foreground);
    kota::task<std::expected<void, AnalysisError>> prepare_modules(
        Prepared& prepared);
    kota::task<std::expected<void, AnalysisError>> collect(
        const Prepared& prepared, RuntimeLane lane);
    kota::task<> collect_project(const AnalysisSessionId& session,
                                 const CompilationUnitId& foreground);
    std::optional<analysis_proto::WirePchLoad> cached_preamble(
        const Prepared& prepared);
    std::expected<void, AnalysisError> remember_preamble(
        const Prepared& prepared, analysis_proto::WirePchLoad load);
    kota::task<std::expected<analysis_proto::WirePchLoad, AnalysisError>>
        build_preamble(Prepared& prepared,
                       std::optional<Path> failed_preamble);
    void forget_preamble(const Prepared& prepared);
    void cancel_session_requests(const AnalysisSessionId& session,
                                 bool closing = false);
    std::string active_key(const AnalysisSessionId& session,
                           const Path& path) const;
    std::vector<Path> impacted_documents(const Path& changed) const;

    CoreRuntime& m_runtime;
    CoreBuildService& m_builds;
    ProjectHandle m_project;
    SourceViewStore m_views;
    ProjectIndex m_index;
    std::optional<WorkerPoolId> m_workers;
    struct ActiveRequest {
        RuntimeRequest request;
        std::size_t waiters = 0;
    };
    struct AnalysisWork {
        RuntimeRequest request;
        std::size_t waiters = 0;
        std::mutex completion_mutex;
        std::optional<std::expected<void, AnalysisError>> index_result;
    };
    enum class WorkCache { none, index, restore };
    struct WorkWaiter {
        WorkWaiter(CoreAnalysisService* service,
                   std::shared_ptr<AnalysisWork> operation,
                   std::string active, std::string index, WorkCache cache)
            : owner(service), work(std::move(operation)),
              active_key(std::move(active)), index_key(std::move(index)),
              cache(cache) {}
        WorkWaiter(const WorkWaiter&) = delete;
        WorkWaiter& operator=(const WorkWaiter&) = delete;
        ~WorkWaiter();

        CoreAnalysisService* owner;
        std::shared_ptr<AnalysisWork> work;
        std::string active_key;
        std::string index_key;
        WorkCache cache = WorkCache::none;
        bool completed = false;
    };
    WorkWaiter start_work(analysis_proto::WireRequest request,
                          const Prepared& prepared, RuntimeLane lane,
                          std::string index_key);
    WorkWaiter start_restore(const Prepared& prepared, RuntimeLane lane,
                             const std::string& index_key);
    void release_work_waiter(WorkWaiter& waiter);
    std::mutex m_active_mutex;
    std::map<std::string, std::map<std::uint64_t, ActiveRequest>> m_active;
    std::set<std::string> m_closed_sessions;
    std::map<std::string, std::shared_ptr<AnalysisWork>> m_index_work;
    std::map<std::string, std::shared_ptr<AnalysisWork>> m_restore_work;
    std::map<std::string, analysis_detail::PreambleRecord> m_preambles;
    std::set<std::string> m_ignored_preamble_receipts;
};

std::string CoreAnalysisService::active_key(
        const AnalysisSessionId& session, const Path& path) const {
    return session.value + "\n" + analysis_detail::canonical_path(path);
}

void CoreAnalysisService::cancel_session_requests(
        const AnalysisSessionId& session, bool closing) {
    std::lock_guard lock(m_active_mutex);
    if (closing) m_closed_sessions.insert(session.value);
    std::map<std::uint64_t, std::size_t> total_waiters;
    std::map<std::uint64_t, std::size_t> cancelled_waiters;
    std::map<std::uint64_t, RuntimeRequest> requests;
    for (const auto& [key, active_requests] : m_active) {
        const auto separator = key.find('\n');
        const std::string key_session = key.substr(0, separator);
        const bool cancelled_session = key_session == session.value ||
            m_closed_sessions.contains(key_session);
        for (const auto& [id, active] : active_requests) {
            total_waiters[id] += active.waiters;
            if (cancelled_session) cancelled_waiters[id] += active.waiters;
            requests.emplace(id, active.request);
        }
    }
    for (const auto& [id, count] : cancelled_waiters)
        if (count == total_waiters[id]) requests.at(id).cancel();
    const bool session_active = std::ranges::any_of(
        m_active, [&](const auto& entry) {
            return entry.first.starts_with(session.value + "\n");
        });
    if (!session_active) m_closed_sessions.erase(session.value);
}

CoreAnalysisService::WorkWaiter::~WorkWaiter() {
    owner->release_work_waiter(*this);
}

void CoreAnalysisService::release_work_waiter(WorkWaiter& waiter) {
    std::lock_guard lock(m_active_mutex);
    const auto request_id = waiter.work->request.id();
    auto current = m_active.find(waiter.active_key);
    if (current != m_active.end()) {
        auto active = current->second.find(request_id);
        if (active != current->second.end() &&
            --active->second.waiters == 0)
            current->second.erase(active);
        if (current->second.empty()) m_active.erase(current);
    }
    const auto separator = waiter.active_key.find('\n');
    const std::string session = waiter.active_key.substr(0, separator);
    const bool still_active = std::ranges::any_of(
        m_active, [&](const auto& entry) {
            return entry.first.starts_with(session + "\n");
        });
    if (!still_active) m_closed_sessions.erase(session);
    if (--waiter.work->waiters != 0) return;
    if (!waiter.completed) waiter.work->request.cancel();
    auto* cache = waiter.cache == WorkCache::index ? &m_index_work
        : waiter.cache == WorkCache::restore ? &m_restore_work : nullptr;
    if (cache) {
        auto reusable = cache->find(waiter.index_key);
        if (reusable != cache->end() && reusable->second == waiter.work)
            cache->erase(reusable);
    }
}

std::vector<Path> CoreAnalysisService::impacted_documents(
        const Path& changed) const {
    const std::string key = analysis_detail::canonical_path(changed);
    std::vector<Path> result;
    if (compilation_for(changed)) result.push_back(Path(key));
    for (const auto& compilation : m_project.snapshot->compilations) {
        auto dependencies = analysis_detail::dependency_paths(
            *m_project.snapshot, compilation);
        if (std::ranges::any_of(dependencies, [&](const Path& path) {
                return analysis_detail::canonical_path(path) == key;
            }))
            result.push_back(compilation.source);
    }
    std::ranges::sort(result, {}, [](const Path& path) {
        return analysis_detail::canonical_path(path);
    });
    result.erase(std::unique(result.begin(), result.end(),
                             [](const Path& left, const Path& right) {
        return analysis_detail::canonical_path(left) ==
               analysis_detail::canonical_path(right);
    }), result.end());
    return result;
}

std::expected<CoreAnalysisService::Prepared, AnalysisError>
CoreAnalysisService::prepare(const AnalysisSessionId& session,
                             const Path& document) {
    const auto* compilation = compilation_for(document);
    if (!compilation)
        return std::unexpected(AnalysisError{
            "no compilation description for '" + document.string() + "'"});
    auto paths = analysis_detail::dependency_paths(
        *m_project.snapshot, *compilation);
    paths.push_back(document);
    auto dependencies = paths;
    auto view = m_views.capture(session, std::move(paths));
    if (!view) return std::unexpected(view.error());
    const SourceFileView* source = view->find(document);
    if (!source)
        return std::unexpected(AnalysisError{
            "analysis document was not captured: " + document.string()});

    Prepared prepared;
    prepared.compilation = compilation;
    prepared.view = std::move(*view);
    prepared.stamp = AnalysisRequestStamp{
        *context_key(m_project.context), m_project.snapshot_id,
        prepared.view.id, Path(analysis_detail::canonical_path(document)),
        source->document_version.value_or(0)};
    prepared.command = analysis_command(*compilation, m_project.context.compiler);
    prepared.remaps = prepared.view.remapped_files();
    prepared.main_file = analysis_detail::command_file_spelling(*compilation);
    prepared.dependencies = std::move(dependencies);
    if (const SourceFileView* main = prepared.view.find(compilation->source)) {
        prepared.remaps[prepared.main_file] = main->content;
        prepared.remaps[analysis_detail::canonical_path(compilation->source)] =
            main->content;
    }
    return prepared;
}

kota::task<std::expected<void, AnalysisError>>
CoreAnalysisService::prepare_modules(Prepared& prepared) {
    std::vector<Path> required;
    bool transient = false;
    for (const auto& module : prepared.compilation->module_references) {
        bool provider_overlaid = false;
        auto producer = std::ranges::find_if(
            m_project.snapshot->build_graph.actions,
            [&](const BuildAction& action) {
                return std::ranges::any_of(
                    action.outputs, [&](const Path& output) {
                        return analysis_detail::canonical_path(output) ==
                               analysis_detail::canonical_path(module.bmi);
                    });
            });
        auto provider = producer == m_project.snapshot->build_graph.actions.end()
            ? m_project.snapshot->compilations.end()
            : std::ranges::find_if(
                m_project.snapshot->compilations,
                [&](const CompilationDescription& candidate) {
                    return candidate.id.value == producer->id;
                });
        if (provider != m_project.snapshot->compilations.end()) {
            auto dependencies = analysis_detail::dependency_paths(
                *m_project.snapshot, *provider);
            provider_overlaid = std::ranges::any_of(
                dependencies, [&](const Path& dependency) {
                    const auto* file = prepared.view.find(dependency);
                    if (!file || !file->document_version) return false;
                    const auto* input = analysis_detail::snapshot_input(
                        *m_project.snapshot, dependency);
                    return !input || input->digest != file->digest;
                });
        }
        transient = transient || provider_overlaid;
        if (provider_overlaid || !module.bmi.is_regular_file())
            required.push_back(module.bmi);
    }
    if (required.empty()) co_return std::expected<void, AnalysisError>{};

    BuildOptions options;
    options.lane = RuntimeLane::foreground;
    options.required_outputs = required;
    if (transient) {
        options.publication = BuildPublication::transient;
        for (const auto& [path, file] : prepared.view.files)
            if (file.document_version)
                options.input_overrides.push_back(
                    BuildInputOverride{Path(path), file.content});
    }
    auto operation = m_builds.start(m_project, std::move(options));
    if (!operation)
        co_return std::unexpected(AnalysisError{operation.error().message});
    auto result = co_await operation->wait();
    const auto* ready = std::get_if<ReadyBuild>(&result.outcome);
    if (!ready)
        co_return std::unexpected(AnalysisError{
            "required module preparation did not produce ready artifacts"});
    std::println(std::cerr, "bake lsp: core build prepared {} module output(s)",
                 required.size());

    for (const auto& module : prepared.compilation->module_references) {
        const std::string planned_relative = module.bmi.fs()
            .lexically_normal()
            .lexically_relative(m_project.paths.root.fs().lexically_normal())
            .generic_string();
        auto artifact = std::ranges::find_if(
            ready->artifacts, [&](const ArtifactIdentity& candidate) {
                const std::string actual = candidate.path.fs()
                    .lexically_normal().generic_string();
                return candidate.output_name == module.bmi.filename_string() &&
                       (actual == module.bmi.fs().lexically_normal().generic_string() ||
                        (!planned_relative.empty() &&
                         actual.ends_with(planned_relative)));
            });
        if (artifact == ready->artifacts.end()) continue;
        const std::string planned = module.bmi.string();
        const std::string actual = artifact->path.string();
        for (auto& argument : prepared.command) {
            std::size_t offset = 0;
            while ((offset = argument.find(planned, offset)) !=
                   std::string::npos) {
                argument.replace(offset, planned.size(), actual);
                offset += actual.size();
            }
        }
    }
    co_return std::expected<void, AnalysisError>{};
}

CoreAnalysisService::WorkWaiter CoreAnalysisService::start_work(
        analysis_proto::WireRequest request, const Prepared& prepared,
        RuntimeLane lane, std::string index_key) {
    std::shared_ptr<AnalysisWork> work;
    const std::string key = active_key(prepared.view.session,
                                       prepared.stamp.document);
    {
        std::lock_guard lock(m_active_mutex);
        auto reusable = index_key.empty()
            ? m_index_work.end() : m_index_work.find(index_key);
        if (reusable != m_index_work.end()) {
            work = reusable->second;
        } else {
            request.argv = prepared.command;
            request.remap = prepared.remaps;
            if (request.main_file.empty()) request.main_file = prepared.main_file;
            RuntimeSubmitOptions options;
            options.lane = lane;
            work = std::make_shared<AnalysisWork>();
            work->request = m_runtime.submit_worker(
                *m_workers, analysis_proto::encode_request(request),
                std::move(options));
            if (!index_key.empty()) m_index_work[index_key] = work;
        }
        ++work->waiters;
        auto [active, inserted] = m_active[key].try_emplace(
            work->request.id(), ActiveRequest{work->request, 0});
        (void)inserted;
        ++active->second.waiters;
    }
    const auto cache = index_key.empty() ? WorkCache::none : WorkCache::index;
    return WorkWaiter{
        this, std::move(work), key, std::move(index_key), cache};
}

CoreAnalysisService::WorkWaiter CoreAnalysisService::start_restore(
        const Prepared& prepared, RuntimeLane lane,
        const std::string& index_key) {
    std::shared_ptr<AnalysisWork> work;
    const std::string key = active_key(prepared.view.session,
                                       prepared.stamp.document);
    {
        std::lock_guard lock(m_active_mutex);
        auto reusable = m_restore_work.find(index_key);
        if (reusable != m_restore_work.end()) {
            work = reusable->second;
        } else {
            RuntimeSubmitOptions options;
            options.lane = lane;
            work = std::make_shared<AnalysisWork>();
            work->request = m_runtime.submit_blocking(
                [this, index_key](const CancellationToken& token) {
                    if (token.cancellation_requested())
                        return BlockingResult{
                            .ok = false,
                            .error = "index shard restore cancelled"};
                    auto shard = m_index.read_shard(index_key);
                    if (!shard)
                        return BlockingResult{
                            .ok = false, .error = shard.error().message};
                    if (token.cancellation_requested())
                        return BlockingResult{
                            .ok = false,
                            .error = "index shard restore cancelled"};
                    return BlockingResult{.value = std::move(*shard)};
                }, std::move(options));
            m_restore_work[index_key] = work;
        }
        ++work->waiters;
        auto [active, inserted] = m_active[key].try_emplace(
            work->request.id(), ActiveRequest{work->request, 0});
        (void)inserted;
        ++active->second.waiters;
    }
    return WorkWaiter{
        this, std::move(work), key, index_key, WorkCache::restore};
}

kota::task<std::expected<analysis_proto::WireResponse, AnalysisError>>
CoreAnalysisService::call(analysis_proto::WireRequest request,
                          const Prepared& prepared, RuntimeLane lane) {
    auto waiter = start_work(std::move(request), prepared, lane, {});
    auto runtime_result = co_await waiter.work->request.wait();
    waiter.completed = true;
    auto response = analysis_detail::worker_response(runtime_result);
    if (!response) co_return std::unexpected(response.error());
    if (!m_views.is_current(prepared.view))
        co_return std::unexpected(AnalysisError{
            "analysis result is stale for the current source view",
            AnalysisErrorCode::stale});
    co_return std::move(*response);
}

kota::task<std::expected<void, AnalysisError>>
CoreAnalysisService::collect(const Prepared& prepared, RuntimeLane lane) {
    std::map<std::string, std::string> captured;
    for (const auto& path : prepared.dependencies)
        if (const auto* file = prepared.view.find(path))
            captured[analysis_detail::canonical_path(path)] = file->content;
    auto inputs = analysis_detail::identities(
        prepared.view, prepared.dependencies);
    const std::string key = ProjectIndex::shard_key(
        *prepared.compilation, m_project.context.compiler, captured, inputs);
    if (m_index.is_current(prepared.compilation->id, key))
        co_return std::expected<void, AnalysisError>{};
    auto restore_waiter = start_restore(prepared, lane, key);
    auto restore_result = co_await restore_waiter.work->request.wait();
    restore_waiter.completed = true;
    auto restored = analysis_detail::restored_shard(restore_result);
    if (!restored) co_return std::unexpected(restored.error());
    if (!m_views.is_current(prepared.view))
        co_return std::unexpected(AnalysisError{
            "index restore is stale for the current source view",
            AnalysisErrorCode::stale});
    if (*restored) {
        auto installed = m_index.install(
            prepared.compilation->id, std::move(**restored),
            analysis_detail::dependency_paths(
                *m_project.snapshot, *prepared.compilation));
        if (!installed)
            co_return std::unexpected(AnalysisError{
                installed.error().message});
        co_return std::expected<void, AnalysisError>{};
    }
    analysis_proto::WireRequest request;
    request.op = "collect";
    auto waiter = start_work(std::move(request), prepared, lane, key);
    auto runtime_result = co_await waiter.work->request.wait();
    waiter.completed = true;
    std::lock_guard completion_lock(waiter.work->completion_mutex);
    if (!m_views.is_current(prepared.view))
        co_return std::unexpected(AnalysisError{
            "index result is stale for the current source view",
            AnalysisErrorCode::stale});
    auto& result = waiter.work->index_result;
    if (!result) {
        auto response = analysis_detail::worker_response(runtime_result);
        if (!response) {
            result.emplace(std::unexpected(response.error()));
        } else {
            auto adopted = m_index.adopt(
                prepared.compilation->id, key,
                std::move(response->occurrences),
                analysis_detail::dependency_paths(
                    *m_project.snapshot, *prepared.compilation));
            if (adopted) result.emplace();
            else result.emplace(std::unexpected(AnalysisError{
                adopted.error().message}));
        }
    }
    co_return *result;
}

kota::task<> CoreAnalysisService::collect_project(
        const AnalysisSessionId& session,
        const CompilationUnitId& foreground) {
    for (const auto& compilation : m_project.snapshot->compilations) {
        if (compilation.id == foreground) continue;
        auto prepared = prepare(session, compilation.source);
        if (!prepared) continue;
        auto modules = co_await prepare_modules(*prepared);
        if (!modules) continue;
        (void)co_await collect(*prepared, RuntimeLane::background);
    }
    co_return;
}

kota::task<> CoreAnalysisService::background_index(
        const AnalysisSessionId& session) {
    co_await collect_project(session, CompilationUnitId{});
}

std::optional<analysis_proto::WirePchLoad>
CoreAnalysisService::cached_preamble(const Prepared& prepared) {
    const std::string unit = prepared.compilation->id.value;
    if (m_ignored_preamble_receipts.contains(unit)) return std::nullopt;
    auto found = m_preambles.find(unit);
    if (found == m_preambles.end()) {
        Path receipt = m_project.paths.pch / "receipts" /
            (SHA256::hex(unit) + ".json");
        if (auto content = read_file(receipt)) {
            auto record = analysis_detail::parse_preamble_record(*content);
            if (record) found = m_preambles.emplace(unit, std::move(*record)).first;
        }
    }
    if (found == m_preambles.end()) return std::nullopt;
    const SourceFileView* main = prepared.view.find(
        prepared.compilation->source);
    if (!main) return std::nullopt;
    const auto& record = found->second;
    const std::size_t region = analysis_detail::module_region_limit(
        main->content);
    const std::string pch_root = analysis_detail::canonical_path(
        m_project.paths.pch);
    const std::string pch_path = analysis_detail::canonical_path(
        Path(record.load.path));
    const bool valid = record.context == prepared.compilation->context.value &&
        record.dependency_identity == analysis_detail::preamble_dependencies(
            prepared.view, prepared.compilation->source,
            prepared.dependencies) &&
        record.load.region_limit == region &&
        record.load.size <= main->content.size() &&
        record.prefix_digest == SHA256::hex(
            std::string_view(main->content).substr(0, record.load.size)) &&
        pch_path.starts_with(pch_root + "/") &&
        Path(record.load.path).is_regular_file();
    if (!valid) {
        forget_preamble(prepared);
        return std::nullopt;
    }
    return record.load;
}

std::expected<void, AnalysisError> CoreAnalysisService::remember_preamble(
        const Prepared& prepared, analysis_proto::WirePchLoad load) {
    const SourceFileView* main = prepared.view.find(
        prepared.compilation->source);
    if (!main || load.size > main->content.size())
        return std::unexpected(AnalysisError{
            "analysis worker returned invalid preamble bounds"});
    analysis_detail::PreambleRecord record;
    record.context = prepared.compilation->context.value;
    record.dependency_identity = analysis_detail::preamble_dependencies(
        prepared.view, prepared.compilation->source,
        prepared.dependencies);
    record.prefix_digest = SHA256::hex(
        std::string_view(main->content).substr(0, load.size));
    record.load = std::move(load);
    Path receipt = m_project.paths.pch / "receipts" /
        (SHA256::hex(prepared.compilation->id.value) + ".json");
    const std::string document = analysis_detail::preamble_record_json(record);
    ProjectPublicationGuard guard(m_project.paths);
    auto replaced = replace_core_state_file(
        receipt, document, guard,
        [&](std::string_view content) -> std::expected<void, StateError> {
            if (!analysis_detail::parse_preamble_record(content))
                return std::unexpected(StateError{
                    StateErrorCode::malformed, receipt,
                    "invalid analysis preamble receipt"});
            return {};
        });
    if (!replaced)
        return std::unexpected(AnalysisError{replaced.error().message});
    m_preambles[prepared.compilation->id.value] = std::move(record);
    m_ignored_preamble_receipts.erase(prepared.compilation->id.value);
    return {};
}

void CoreAnalysisService::forget_preamble(const Prepared& prepared) {
    const std::string unit = prepared.compilation->id.value;
    m_preambles.erase(unit);
    m_ignored_preamble_receipts.insert(unit);
}

kota::task<std::expected<analysis_proto::WirePchLoad, AnalysisError>>
CoreAnalysisService::build_preamble(
        Prepared& prepared, std::optional<Path> failed_preamble) {
    const SourceFileView* main = prepared.view.find(
        prepared.compilation->source);
    if (!main) co_return analysis_proto::WirePchLoad{};
    analysis_proto::WireRequest request;
    request.op = "preamble";
    request.region_limit = analysis_detail::module_region_limit(main->content);
    static std::atomic<std::uint64_t> sequence = 0;
    const auto nonce = std::format(
        "{}-{}-{}", prepared.view.id.value,
        std::chrono::steady_clock::now().time_since_epoch().count(),
        sequence.fetch_add(1, std::memory_order_relaxed));
    Path temporary = m_project.paths.work / "analysis-preamble" /
        (SHA256::hex(nonce) + ".pch");
    temporary.parent().mkdir_recursive();
    request.pch_out = temporary.string();
    auto built = co_await call(std::move(request), prepared);
    if (!built) co_return std::unexpected(built.error());
    if (!built->built) co_return analysis_proto::WirePchLoad{};

    auto inputs = analysis_detail::identities(
        prepared.view, prepared.dependencies);
    std::string identity = preamble_identity(
        *prepared.compilation, m_project.context.compiler, prepared.view,
        built->preamble_size, inputs);
    Path final = m_project.paths.pch / "o" / identity / "preamble.pch";
    final.parent().mkdir_recursive();
    const bool replacing_failed = failed_preamble &&
        analysis_detail::canonical_path(*failed_preamble) ==
            analysis_detail::canonical_path(final);
    if (replacing_failed || !final.is_regular_file()) {
        auto error = atomic_replace_file(temporary, final);
        if (error)
            co_return std::unexpected(AnalysisError{
                "failed to publish analysis preamble: " + error.message()});
    } else if (temporary.exists()) {
        temporary.remove();
    }
    analysis_proto::WirePchLoad load{
        final.string(), built->preamble_size, built->ends_at_start_of_line,
        analysis_detail::module_region_limit(main->content)};
    auto remembered = remember_preamble(prepared, load);
    if (!remembered) co_return std::unexpected(remembered.error());
    co_return load;
}

kota::task<std::expected<AnalysisDiagnostics, AnalysisError>>
CoreAnalysisService::diagnostics(const AnalysisSessionId& session,
                                 const Path& document) {
    auto prepared = prepare(session, document);
    if (!prepared) co_return std::unexpected(prepared.error());
    auto modules = co_await prepare_modules(*prepared);
    if (!modules) co_return std::unexpected(modules.error());

    auto cached = cached_preamble(*prepared);
    analysis_proto::WirePchLoad pch;
    if (cached) {
        pch = *cached;
    } else {
        auto built = co_await build_preamble(*prepared, std::nullopt);
        if (!built) co_return std::unexpected(built.error());
        pch = std::move(*built);
    }

    analysis_proto::WireRequest request;
    request.op = "syntax";
    request.pch = std::move(pch);
    const bool had_pch = !request.pch.empty();
    const Path attempted_preamble(request.pch.path);
    auto response = co_await call(std::move(request), *prepared);
    if (!response) co_return std::unexpected(response.error());
    if (had_pch && analysis_detail::pch_load_failed(*response)) {
        forget_preamble(*prepared);
        auto rebuilt = co_await build_preamble(*prepared, attempted_preamble);
        if (!rebuilt) co_return std::unexpected(rebuilt.error());
        analysis_proto::WireRequest retry;
        retry.op = "syntax";
        retry.pch = std::move(*rebuilt);
        response = co_await call(std::move(retry), *prepared);
        if (!response) co_return std::unexpected(response.error());
    }
    co_return AnalysisDiagnostics{
        AnalysisResultStamp{prepared->stamp, ResultStatus::ready},
        analysis_detail::diagnostics_from_wire(
            std::move(response->diagnostics))};
}

kota::task<std::expected<AnalysisCompletions, AnalysisError>>
CoreAnalysisService::complete(const AnalysisSessionId& session,
                              const Path& document, unsigned line,
                              unsigned column) {
    auto prepared = prepare(session, document);
    if (!prepared) co_return std::unexpected(prepared.error());
    auto modules = co_await prepare_modules(*prepared);
    if (!modules) co_return std::unexpected(modules.error());
    analysis_proto::WireRequest request;
    request.op = "complete";
    request.line = line;
    request.column = column;
    auto response = co_await call(std::move(request), *prepared);
    if (!response) co_return std::unexpected(response.error());
    co_return AnalysisCompletions{
        AnalysisResultStamp{prepared->stamp, ResultStatus::ready},
        analysis_detail::completions_from_wire(std::move(response->items))};
}

kota::task<std::expected<AnalysisLocations, AnalysisError>>
CoreAnalysisService::definitions(const AnalysisSessionId& session,
                                 const Path& document, unsigned line,
                                 unsigned column, std::string fallback_name) {
    auto prepared = prepare(session, document);
    if (!prepared) co_return std::unexpected(prepared.error());
    auto modules = co_await prepare_modules(*prepared);
    if (!modules) co_return std::unexpected(modules.error());
    auto indexed = co_await collect(*prepared, RuntimeLane::foreground);
    if (!indexed) co_return std::unexpected(indexed.error());
    std::vector<IndexLocation> locations;
    auto occurrence = m_index.occurrence_at(
        document.string(), line, column);
    if (occurrence) {
        std::map<std::string, const CompilationDescription*> providers;
        auto add_provider = [&](const Path& source) {
            const auto* provider = compilation_for(source);
            if (provider && provider->id != prepared->compilation->id)
                providers.emplace(provider->id.value, provider);
        };
        for (const auto& source : m_index.definition_sources(occurrence->usr))
            add_provider(source);
        if (!occurrence->decl_file.empty())
            add_provider(Path(occurrence->decl_file));

        for (const auto& [id, provider] : providers) {
            (void)id;
            bool refreshed = false;
            auto provider_view = prepare(session, provider->source);
            if (provider_view) {
                auto provider_modules = co_await prepare_modules(*provider_view);
                if (provider_modules) {
                    auto collected = co_await collect(
                        *provider_view, RuntimeLane::foreground);
                    refreshed = collected.has_value();
                }
            }
            if (!refreshed) m_index.invalidate(provider->source);
        }
        if (!m_index.has_definition(occurrence->usr))
            co_await collect_project(session, prepared->compilation->id);
        if (!m_views.is_current(prepared->view))
            co_return std::unexpected(AnalysisError{
                "definition result is stale for the current source view",
                AnalysisErrorCode::stale});
        locations = m_index.find_definitions(occurrence->usr);
        if (locations.empty() && !occurrence->decl_file.empty())
            locations.push_back({occurrence->decl_file, occurrence->decl_line,
                                 occurrence->decl_column});
    } else {
        co_await collect_project(session, prepared->compilation->id);
        if (!m_views.is_current(prepared->view))
            co_return std::unexpected(AnalysisError{
                "definition result is stale for the current source view",
                AnalysisErrorCode::stale});
    }
    if (locations.empty() && !fallback_name.empty())
        locations = m_index.find_definitions_by_name(fallback_name);
    co_return AnalysisLocations{
        AnalysisResultStamp{prepared->stamp, ResultStatus::ready},
        std::move(locations)};
}

kota::task<std::expected<AnalysisLocations, AnalysisError>>
CoreAnalysisService::references(const AnalysisSessionId& session,
                                const Path& document, unsigned line,
                                unsigned column, bool include_declaration) {
    auto origin = prepare(session, document);
    if (!origin) co_return std::unexpected(origin.error());
    auto origin_modules = co_await prepare_modules(*origin);
    if (!origin_modules) co_return std::unexpected(origin_modules.error());
    auto origin_index = co_await collect(*origin, RuntimeLane::foreground);
    if (!origin_index) co_return std::unexpected(origin_index.error());
    auto occurrence = m_index.occurrence_at(document.string(), line, column);
    if (!occurrence)
        co_return AnalysisLocations{
            AnalysisResultStamp{origin->stamp, ResultStatus::ready}, {}};

    co_await collect_project(session, origin->compilation->id);
    if (!m_views.is_current(origin->view))
        co_return std::unexpected(AnalysisError{
            "reference result is stale for the current source view",
            AnalysisErrorCode::stale});
    co_return AnalysisLocations{
        AnalysisResultStamp{origin->stamp, ResultStatus::ready},
        m_index.find_references(occurrence->usr, include_declaration)};
}

}  // namespace bake
