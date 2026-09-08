module;

#include <kota/async/runtime/task.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

export module bake.core.build;

import std;
import bake.json;
import bake.util;
import bake.buildsystem.plan;
import bake.core.model;
import bake.core.project;
import bake.core.runtime;
import bake.core.snapshot;
import bake.core.state;

namespace bake {

export enum class BuildPublication { persistent, transient };

export struct BuildInputOverride {
    Path path;
    std::string content;
};

export enum class BuildProgressKind { moid_started, action_scheduled, action_reused };

export struct BuildProgress {
    BuildProgressKind kind;
    std::string moid_id;
    std::string moid;
    std::string version;
    std::string action_id;
    std::string description;
};

export struct BuildOptions {
    BuildRequestId request;
    RuntimeLane lane = RuntimeLane::background;
    bool verbose = false;
    std::function<void(const BuildProgress&)> progress;
    std::vector<Path> required_outputs;
    std::vector<BuildInputOverride> input_overrides;
    BuildPublication publication = BuildPublication::persistent;
};

export enum class BuildErrorCode {
    invalid_request,
    invalid_graph,
    missing_input,
    io,
};

export struct BuildError {
    BuildErrorCode code;
    Path path;
    std::string message;
};

namespace build_detail {

struct CapturedInput {
    InputIdentity identity;
    Path captured_path;
};

struct ActionWork {
    std::size_t index = 0;
    std::string cache_key;
    bool dirty = true;
    std::vector<Path> staged_outputs;
    RuntimeRequest runtime;
};

struct ReadyResultIndex {
    std::string snapshot;
    std::set<std::string, std::less<>> action_cache_keys;
    std::map<std::string, ArtifactIdentity> artifacts;
};

struct OperationState {
    ProjectHandle handle;
    BuildOptions options;
    BuildRequestId request;
    ProjectContextKey context;
    ProjectStateStore store;
    Path work;
    std::vector<bool> selected;
    std::vector<CapturedInput> inputs;
    std::map<std::string, std::size_t> input_by_path;
    std::vector<ActionWork> actions;
    std::map<std::size_t, std::size_t> work_by_action;
    std::map<std::string, Path> staged_by_output;
    std::map<std::size_t, std::vector<ArtifactIdentity>> reusable_outputs;
    std::map<std::string, std::optional<ReadyResultIndex>> ready_result_cache;
    std::optional<BuildResult> completed;
    std::atomic<bool> cancelled = false;

    OperationState(ProjectHandle project, BuildOptions build_options,
                   BuildRequestId request_id, ProjectContextKey context_key)
        : handle(std::move(project)), options(std::move(build_options)),
          request(std::move(request_id)),
          context(context_key), store(handle.paths, std::move(context_key)),
          work(handle.paths.work / request.value) {}
};

std::string normalized(const Path& path) {
    return path.fs().lexically_normal().generic_string();
}

std::string request_id() {
    static std::atomic<std::uint64_t> sequence = 0;
    const auto ticks = std::chrono::steady_clock::now()
                           .time_since_epoch().count();
    const auto process =
#ifdef _WIN32
        static_cast<std::uint64_t>(GetCurrentProcessId());
#else
        static_cast<std::uint64_t>(getpid());
#endif
    std::random_device random;
    const auto nonce = (static_cast<std::uint64_t>(random()) << 32) ^ random();
    return std::format("build-{}-{}-{}-{}", process, ticks, nonce,
                       sequence.fetch_add(1, std::memory_order_relaxed));
}

bool safe_request_id(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_';
    });
}

std::optional<std::string> override_for(
        const std::map<std::string, std::string>& overrides,
        const Path& path) {
    auto found = overrides.find(normalized(path));
    if (found == overrides.end()) return std::nullopt;
    return found->second;
}

std::optional<Path> staged_path(const ContextPaths& paths, const Path& work,
                                const Path& output) {
    auto relative = output.fs().lexically_normal().lexically_relative(
        paths.root.fs().lexically_normal());
    const std::string relative_name = relative.generic_string();
    if (relative.empty() || relative.is_absolute() ||
        relative_name == ".." || relative_name.starts_with("../")) {
        return std::nullopt;
    }
    return work / "outputs" / Path(relative);
}

std::vector<std::string> remap_command(
        const BuildAction& action,
        const std::map<std::string, Path>& remaps,
        const std::optional<Path>& overlay) {
    std::vector<std::string> command = action.command;
    for (auto& argument : command) {
        if (auto exact = remaps.find(argument); exact != remaps.end()) {
            argument = exact->second.string();
            continue;
        }
        constexpr std::string_view module_output = "-fmodule-output=";
        if (argument.starts_with(module_output)) {
            auto path = remaps.find(argument.substr(module_output.size()));
            if (path != remaps.end())
                argument = std::string(module_output) + path->second.string();
            continue;
        }
        constexpr std::string_view module_file = "-fmodule-file=";
        if (argument.starts_with(module_file)) {
            const auto separator = argument.find('=', module_file.size());
            if (separator != std::string::npos) {
                auto path = remaps.find(argument.substr(separator + 1));
                if (path != remaps.end())
                    argument = argument.substr(0, separator + 1) +
                               path->second.string();
            }
        }
    }
    if (overlay && action.is_compile()) {
        command.push_back("-ivfsoverlay");
        command.push_back(overlay->string());
    }
    return command;
}

json::Value input_json(const InputIdentity& input) {
    json::Value value;
    value["path"] = input.path.string();
    value["digest"] = input.digest;
    value["size"] = static_cast<std::int64_t>(input.size);
    return value;
}

json::Value artifact_json(const ArtifactIdentity& artifact) {
    json::Value value;
    value["owner_moid"] = artifact.owner_moid;
    value["output_name"] = artifact.output_name;
    value["path"] = artifact.path.string();
    value["digest"] = artifact.digest;
    value["size"] = static_cast<std::int64_t>(artifact.size);
    return value;
}

json::Value result_payload(const BuildResult& result,
                           const std::vector<ActionWork>& actions) {
    json::Value payload;
    payload["snapshot"] = result.snapshot.value;
    payload["inputs"] = json::Value::array();
    for (const auto& input : result.inputs)
        payload["inputs"].push_back(input_json(input));
    payload["actions"] = json::Value::array();
    for (const auto& action : actions) {
        json::Value value;
        value["index"] = static_cast<std::int64_t>(action.index);
        value["cache_key"] = action.cache_key;
        value["reused"] = !action.dirty;
        payload["actions"].push_back(std::move(value));
    }
    payload["artifacts"] = json::Value::array();
    if (const auto* ready = std::get_if<ReadyBuild>(&result.outcome)) {
        for (const auto& artifact : ready->artifacts)
            payload["artifacts"].push_back(artifact_json(artifact));
    }
    if (const auto* failed = std::get_if<FailedBuild>(&result.outcome))
        payload["message"] = failed->message;
    if (const auto* cancelled = std::get_if<CancelledBuild>(&result.outcome))
        payload["message"] = cancelled->reason;
    if (const auto* partial = std::get_if<PartialBuild>(&result.outcome)) {
        payload["message"] = partial->message;
        payload["failed_action"] = partial->failed_action;
    }
    return payload;
}

bool matches_file(const InputIdentity& expected) {
    auto content = read_file(expected.path);
    return content && content->size() == expected.size &&
           SHA256::hex(*content) == expected.digest;
}

std::expected<ArtifactIdentity, BuildError> artifact_identity(
        const BuildAction& action, const Path& path) {
    auto content = read_file(path);
    if (!content) {
        return std::unexpected(BuildError{
            BuildErrorCode::io, path,
            "completed build output was not found",
        });
    }
    return ArtifactIdentity{action.moid_id, path.filename_string(), path,
                            SHA256::hex(*content), content->size()};
}

Path receipt_directory(const OperationState& state,
                       std::string_view cache_key) {
    return state.handle.paths.core / "build-actions" / Path(cache_key);
}

std::optional<ReadyResultIndex> index_ready_result(
        const RequestResultDocument& result) {
    if (result.status != ResultStatus::ready) return std::nullopt;
    const auto* snapshot = result.payload.find("snapshot");
    const auto* actions = result.payload.find("actions");
    const auto* artifacts = result.payload.find("artifacts");
    if (!snapshot || !snapshot->is_string() ||
        !actions || !actions->is_array() ||
        !artifacts || !artifacts->is_array())
        return std::nullopt;

    ReadyResultIndex index;
    index.snapshot = snapshot->get<std::string>();
    for (const auto& item : *actions) {
        if (!item.is_object()) return std::nullopt;
        const auto* action_index = item.find("index");
        const auto* cache_key = item.find("cache_key");
        if (!action_index || !action_index->is_integer() ||
            action_index->get<std::int64_t>() < 0 ||
            !cache_key || !cache_key->is_string() ||
            cache_key->get<std::string>().empty())
            return std::nullopt;
        index.action_cache_keys.insert(cache_key->get<std::string>());
    }
    for (const auto& item : *artifacts) {
        if (!item.is_object()) return std::nullopt;
        const auto* owner = item.find("owner_moid");
        const auto* output_name = item.find("output_name");
        const auto* path = item.find("path");
        const auto* digest = item.find("digest");
        const auto* size = item.find("size");
        if (!owner || !owner->is_string() ||
            !output_name || !output_name->is_string() ||
            !path || !path->is_string() ||
            !digest || !digest->is_string() ||
            !size || !size->is_integer() || size->get<std::int64_t>() < 0 ||
            owner->get<std::string>().empty() ||
            output_name->get<std::string>().empty() ||
            path->get<std::string>().empty() ||
            digest->get<std::string>().size() != 64 ||
            !std::ranges::all_of(digest->get<std::string>(),
                [](unsigned char c) {
                    return (c >= '0' && c <= '9') ||
                           (c >= 'a' && c <= 'f');
                }))
            return std::nullopt;
        ArtifactIdentity artifact{
            owner->get<std::string>(), output_name->get<std::string>(),
            Path(path->get<std::string>()), digest->get<std::string>(),
            static_cast<std::uintmax_t>(size->get<std::int64_t>())};
        if (!index.artifacts.emplace(artifact.path.string(),
                                     std::move(artifact)).second)
            return std::nullopt;
    }
    return index;
}

const ReadyResultIndex* ready_result(
        OperationState& state, const BuildRequestId& request) {
    auto [entry, inserted] = state.ready_result_cache.try_emplace(
        request.value, std::nullopt);
    if (inserted) {
        auto result = state.store.load_request_result(request);
        if (result) entry->second = index_ready_result(*result);
    }
    return entry->second ? &*entry->second : nullptr;
}

bool same_artifact(const ArtifactIdentity& left,
                   const ArtifactIdentity& right) {
    return left.owner_moid == right.owner_moid &&
           left.output_name == right.output_name &&
           normalized(left.path) == normalized(right.path) &&
           left.digest == right.digest && left.size == right.size;
}

std::expected<void, BuildError> ready_publication(
        const ProjectHandle& project, const BuildResult& result) {
    auto key = context_key(project.context);
    if (!key || result.context != *key ||
        result.snapshot != project.snapshot_id) {
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_request, {},
            "build result does not belong to the open project snapshot",
        });
    }
    const auto* ready = std::get_if<ReadyBuild>(&result.outcome);
    if (!ready) {
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_request, {},
            "build result is not ready",
        });
    }
    ProjectStateStore store(project.paths, *key);
    auto persisted = store.load_request_result(result.request);
    if (!persisted || persisted->status != ResultStatus::ready) {
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_request, project.paths.results,
            "build result has no matching successful publication",
        });
    }
    auto publication = index_ready_result(*persisted);
    if (!publication || publication->snapshot != result.snapshot.value ||
        publication->artifacts.size() != ready->artifacts.size()) {
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_request, persisted->path,
            "published build result has no artifact identities",
        });
    }
    for (const auto& artifact : ready->artifacts) {
        auto published = publication->artifacts.find(artifact.path.string());
        if (published == publication->artifacts.end() ||
            !same_artifact(published->second, artifact)) {
            return std::unexpected(BuildError{
                BuildErrorCode::invalid_request, persisted->path,
                "published build result artifact identity does not match",
            });
        }
    }
    return {};
}

std::optional<std::vector<ArtifactIdentity>> matching_receipt(
        OperationState& state, const BuildAction& action,
        std::string_view cache_key) {
    Path directory = receipt_directory(state, cache_key);
    std::error_code error;
    if (!std::filesystem::is_directory(directory.fs(), error)) return std::nullopt;
    std::vector<Path> candidates;
    for (const auto& entry :
         std::filesystem::directory_iterator(directory.fs(), error)) {
        if (error) return std::nullopt;
        if (entry.is_regular_file() && entry.path().extension() == ".json")
            candidates.emplace_back(entry.path());
    }
    std::ranges::sort(candidates, [](const Path& left, const Path& right) {
        return normalized(left) < normalized(right);
    });
    for (const auto& path : candidates) {
        auto content = read_file(path);
        if (!content) continue;
        try {
            auto document = json::Value::parse(*content);
            if (!document.is_object() || document.size() != 4 ||
                document.value("schema", "") != "bake-core-action-v1" ||
                document.value("action", "") != action.id ||
                document.value("cache_key", "") != cache_key ||
                !document.contains("request") ||
                !document["request"].is_string()) {
                continue;
            }
            BuildRequestId request{document["request"].get<std::string>()};
            const auto* result = ready_result(state, request);
            if (!result || !result->action_cache_keys.contains(cache_key)) {
                continue;
            }
            std::vector<ArtifactIdentity> outputs;
            for (const auto& output : action.outputs) {
                auto output_content = read_file(output);
                if (!output_content) break;
                ArtifactIdentity artifact{
                    action.moid_id, output.filename_string(), output,
                    SHA256::hex(*output_content), output_content->size()};
                auto published = result->artifacts.find(output.string());
                if (published == result->artifacts.end() ||
                    published->second.owner_moid != artifact.owner_moid ||
                    published->second.output_name != artifact.output_name ||
                    published->second.digest != artifact.digest ||
                    published->second.size != artifact.size) {
                    break;
                }
                outputs.push_back(std::move(artifact));
            }
            if (outputs.size() == action.outputs.size()) return outputs;
        } catch (...) {
        }
    }
    return std::nullopt;
}

std::string action_cache_key(const BuildAction& action,
                             const std::vector<InputIdentity>& inputs,
                             const std::vector<std::string>& dependencies) {
    json::Value document;
    document["schema"] = "bake-core-action-key-v1";
    document["type"] = static_cast<int>(action.type);
    document["id"] = action.id;
    document["command"] = action.command;
    document["inputs"] = json::Value::array();
    for (const auto& input : inputs)
        document["inputs"].push_back(input_json(input));
    document["dependencies"] = dependencies;
    return SHA256::hex(document.dump());
}

std::expected<void, BuildError> publish_receipt(
        const OperationState& state, const ActionWork& work,
        const BuildAction& action) {
    json::Value document;
    document["schema"] = "bake-core-action-v1";
    document["action"] = action.id;
    document["cache_key"] = work.cache_key;
    document["request"] = state.request.value;
    Path path = receipt_directory(state, work.cache_key) /
                (state.request.value + ".json");
    if (!atomic_write_file(path, document.dump())) {
        return std::unexpected(BuildError{
            BuildErrorCode::io, path,
            "failed to publish build action receipt",
        });
    }
    return {};
}

std::expected<void, BuildError> publish_result(
        const OperationState& state, const BuildResult& result) {
    auto published = state.store.publish_request_result(
        result.request, status_of(result.outcome),
        result_payload(result, state.actions));
    if (!published) {
        return std::unexpected(BuildError{
            BuildErrorCode::io, published.error().path,
            published.error().message,
        });
    }
    return {};
}

}

export class CoreBuildOperation {
public:
    CoreBuildOperation() = default;
    BuildRequestId request_id() const;
    void cancel() const;
    kota::task<BuildResult> wait();
    explicit operator bool() const noexcept { return bool(state_); }

private:
    explicit CoreBuildOperation(
        std::shared_ptr<build_detail::OperationState> state)
        : state_(std::move(state)) {}
    std::shared_ptr<build_detail::OperationState> state_;
    friend class CoreBuildService;
};

export class CoreExecutionCapture {
public:
    CoreExecutionCapture() = default;
    ~CoreExecutionCapture();
    CoreExecutionCapture(const CoreExecutionCapture&) = delete;
    CoreExecutionCapture& operator=(const CoreExecutionCapture&) = delete;
    CoreExecutionCapture(CoreExecutionCapture&& other) noexcept;
    CoreExecutionCapture& operator=(CoreExecutionCapture&& other) noexcept;

    const Path& executable() const noexcept { return executable_; }
    explicit operator bool() const noexcept {
        return !executable_.string().empty();
    }

private:
    CoreExecutionCapture(Path root, Path executable)
        : root_(std::move(root)), executable_(std::move(executable)) {}
    void cleanup() noexcept;

    Path root_;
    Path executable_;
    friend class CoreBuildService;
};

export class CoreBuildService {
public:
    explicit CoreBuildService(CoreRuntime& runtime) : runtime_(runtime) {}

    std::expected<CoreBuildOperation, BuildError> start(
        const ProjectHandle& project, BuildOptions options = {});
    std::expected<void, BuildError> validate_ready(
        const ProjectHandle& project, const BuildResult& result) const;
    std::expected<CoreExecutionCapture, BuildError> capture_execution(
        const ProjectHandle& project, const BuildResult& result,
        const ArtifactIdentity& executable) const;

private:
    CoreRuntime& runtime_;
};

void CoreExecutionCapture::cleanup() noexcept {
    if (root_.string().empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(root_.fs(), ignored);
    root_ = Path();
    executable_ = Path();
}

CoreExecutionCapture::~CoreExecutionCapture() { cleanup(); }

CoreExecutionCapture::CoreExecutionCapture(
        CoreExecutionCapture&& other) noexcept
    : root_(std::exchange(other.root_, Path())),
      executable_(std::exchange(other.executable_, Path())) {}

CoreExecutionCapture& CoreExecutionCapture::operator=(
        CoreExecutionCapture&& other) noexcept {
    if (this == &other) return *this;
    cleanup();
    root_ = std::exchange(other.root_, Path());
    executable_ = std::exchange(other.executable_, Path());
    return *this;
}

BuildRequestId CoreBuildOperation::request_id() const {
    return state_ ? state_->request : BuildRequestId{};
}

void CoreBuildOperation::cancel() const {
    if (!state_) return;
    state_->cancelled.store(true, std::memory_order_release);
    for (const auto& action : state_->actions)
        if (action.runtime) action.runtime.cancel();
}

kota::task<BuildResult> CoreBuildOperation::wait() {
    using namespace build_detail;
    if (!state_) {
        co_return BuildResult{
            {}, {}, {}, {}, FailedBuild{"invalid build operation"}};
    }
    if (state_->completed) co_return *state_->completed;

    std::optional<std::size_t> failed_action;
    std::string failure_message;
    bool cancelled = false;
    for (const auto& work : state_->actions) {
        if (!work.dirty) continue;
        auto result = co_await work.runtime.wait();
        const bool request_cancelled =
            result.state == RuntimeRequestState::cancelled_before_start ||
            result.state == RuntimeRequestState::cancelled_after_start ||
            result.state == RuntimeRequestState::running_cancel_requested;
        if (!result.succeeded() && !failed_action) {
            failed_action = work.index;
            cancelled = request_cancelled;
            failure_message = !result.stderr_data.empty()
                                  ? result.stderr_data
                                  : result.error;
        }
        if (failed_action) {
            for (const auto& sibling : state_->actions)
                if (sibling.runtime) sibling.runtime.cancel();
        }
    }

    BuildResult result{state_->context, state_->handle.snapshot_id,
                       state_->request, {}, FailedBuild{}};
    for (const auto& input : state_->inputs)
        result.inputs.push_back(input.identity);

    cancelled = cancelled ||
        state_->cancelled.load(std::memory_order_acquire);

    if (failed_action) {
        const auto& action =
            state_->handle.snapshot->build_graph.actions[*failed_action];
        if (cancelled)
            result.outcome = CancelledBuild{"build request was cancelled"};
        else
            result.outcome = FailedBuild{"build action '" + action.id +
                                         "' failed" +
                                         (failure_message.empty()
                                              ? std::string{}
                                              : ": " + failure_message)};
        if (state_->options.publication == BuildPublication::persistent)
            (void)publish_result(*state_, result);
        state_->completed = result;
        co_return result;
    }

    if (cancelled) {
        result.outcome = CancelledBuild{"build request was cancelled"};
        if (state_->options.publication == BuildPublication::persistent)
            (void)publish_result(*state_, result);
        state_->completed = result;
        co_return result;
    }

    ProjectPublicationGuard publication(state_->handle.paths);
    if (!publication.locked()) {
        result.outcome = FailedBuild{"failed to lock build publication"};
        state_->completed = result;
        co_return result;
    }
    if (state_->cancelled.load(std::memory_order_acquire)) {
        result.outcome = CancelledBuild{"build request was cancelled"};
        if (state_->options.publication == BuildPublication::persistent)
            (void)publish_result(*state_, result);
        state_->completed = result;
        co_return result;
    }

    for (const auto& input : state_->inputs) {
        const bool overridden = std::ranges::any_of(
            state_->options.input_overrides,
            [&](const BuildInputOverride& item) {
                return normalized(item.path) == normalized(input.identity.path);
            });
        if (!overridden && !matches_file(input.identity)) {
            result.outcome = CancelledBuild{
                "build input changed before publication: " +
                input.identity.path.string()};
            if (state_->options.publication == BuildPublication::persistent)
                (void)publish_result(*state_, result);
            state_->completed = result;
            co_return result;
        }
    }
    for (const auto& [_, outputs] : state_->reusable_outputs) {
        for (const auto& output : outputs) {
            if (!matches_file(InputIdentity{
                    output.path, output.digest, output.size})) {
                result.outcome = CancelledBuild{
                    "reused artifact changed before publication: " +
                    output.path.string()};
                if (state_->options.publication == BuildPublication::persistent)
                    (void)publish_result(*state_, result);
                state_->completed = result;
                co_return result;
            }
        }
    }

    ReadyBuild ready;
    for (const auto& work : state_->actions) {
        const auto& action =
            state_->handle.snapshot->build_graph.actions[work.index];
        if (state_->options.publication == BuildPublication::persistent &&
            !work.dirty) {
            const auto& outputs = state_->reusable_outputs.at(work.index);
            for (const auto& output : outputs)
                ready.artifacts.push_back(output);
            continue;
        }
        for (std::size_t i = 0; i < action.outputs.size(); ++i) {
            Path path = state_->options.publication == BuildPublication::persistent
                            ? (work.dirty ? work.staged_outputs[i]
                                          : action.outputs[i])
                            : work.staged_outputs[i];
            auto artifact = artifact_identity(action, path);
            if (!artifact) {
                result.outcome = FailedBuild{artifact.error().message};
                state_->completed = result;
                co_return result;
            }
            if (state_->options.publication == BuildPublication::persistent)
                artifact->path = action.outputs[i];
            ready.artifacts.push_back(std::move(*artifact));
        }
    }

    if (state_->options.publication == BuildPublication::persistent) {
        struct Promotion {
            Path staged;
            Path destination;
            std::optional<Path> backup;
            bool promoted = false;
        };
        std::vector<Promotion> promotions;
        for (const auto& work : state_->actions) {
            if (!work.dirty) continue;
            const auto& action =
                state_->handle.snapshot->build_graph.actions[work.index];
            for (std::size_t i = 0; i < action.outputs.size(); ++i) {
                const Path& staged = work.staged_outputs[i];
                const Path& destination = action.outputs[i];
                if (!staged.is_regular_file()) {
                    result.outcome = FailedBuild{
                        "build action did not produce '" + staged.string() + "'"};
                    state_->completed = result;
                    co_return result;
                }
                std::error_code error;
                std::filesystem::create_directories(
                    destination.parent().fs(), error);
                if (error) {
                    result.outcome = FailedBuild{
                        "failed to prepare artifact destination: " +
                        error.message()};
                    state_->completed = result;
                    co_return result;
                }
                Promotion promotion{staged, destination};
                const bool destination_exists =
                    std::filesystem::exists(destination.fs(), error);
                if (error || (destination_exists &&
                              !std::filesystem::is_regular_file(
                                  destination.fs(), error))) {
                    result.outcome = FailedBuild{
                        "artifact destination is not a regular file: " +
                        destination.string()};
                    state_->completed = result;
                    co_return result;
                }
                if (destination_exists) {
                    Path backup = state_->work / "backups" /
                        Path(SHA256::hex(normalized(destination)));
                    std::filesystem::create_directories(
                        backup.parent().fs(), error);
                    if (!error)
                        std::filesystem::copy_file(
                            destination.fs(), backup.fs(),
                            std::filesystem::copy_options::overwrite_existing,
                            error);
                    if (error) {
                        result.outcome = FailedBuild{
                            "failed to preserve prior artifact: " +
                            error.message()};
                        state_->completed = result;
                        co_return result;
                    }
                    promotion.backup = std::move(backup);
                }
                promotions.push_back(std::move(promotion));
            }
        }

        std::vector<Path> published_receipts;
        auto rollback = [&]() {
            std::string failure;
            for (auto receipt = published_receipts.rbegin();
                 receipt != published_receipts.rend(); ++receipt) {
                std::error_code error;
                std::filesystem::remove(receipt->fs(), error);
                if (error && failure.empty()) failure = error.message();
            }
            for (auto promotion = promotions.rbegin();
                 promotion != promotions.rend(); ++promotion) {
                if (!promotion->promoted) continue;
                if (promotion->backup) {
                    auto error = atomic_replace_file(
                        *promotion->backup, promotion->destination);
                    if (error && failure.empty()) failure = error.message();
                } else {
                    std::error_code error;
                    std::filesystem::remove(
                        promotion->destination.fs(), error);
                    if (error && failure.empty()) failure = error.message();
                }
            }
            return failure;
        };
        auto fail_publication = [&](std::string message) {
            if (auto failure = rollback(); !failure.empty())
                message += "; rollback failed: " + failure;
            result.outcome = FailedBuild{std::move(message)};
            state_->completed = result;
            return result;
        };

        for (auto& promotion : promotions) {
            auto error = atomic_replace_file(
                promotion.staged, promotion.destination);
            if (error) {
                co_return fail_publication(
                    "failed to publish build artifact: " + error.message());
            }
            promotion.promoted = true;
        }
        for (const auto& artifact : ready.artifacts) {
            if (!matches_file(InputIdentity{
                    artifact.path, artifact.digest, artifact.size})) {
                co_return fail_publication(
                    "published artifact does not match staged content: " +
                    artifact.path.string());
            }
        }

        result.outcome = ready;
        for (const auto& work : state_->actions) {
            if (!work.dirty) continue;
            Path receipt_path =
                receipt_directory(*state_, work.cache_key) /
                Path(state_->request.value + ".json");
            auto published = publish_receipt(
                *state_, work,
                state_->handle.snapshot->build_graph.actions[work.index]);
            if (!published) {
                if (auto content = read_file(receipt_path)) {
                    try {
                        auto document = json::Value::parse(*content);
                        if (document.is_object() &&
                            document.value("schema", "") ==
                                "bake-core-action-v1" &&
                            document.value("action", "") ==
                                state_->handle.snapshot->build_graph
                                    .actions[work.index].id &&
                            document.value("cache_key", "") == work.cache_key &&
                            document.value("request", "") ==
                                state_->request.value)
                            published_receipts.push_back(receipt_path);
                    } catch (...) {
                    }
                }
                co_return fail_publication(published.error().message);
            }
            published_receipts.push_back(std::move(receipt_path));
        }
        auto published = publish_result(*state_, result);
        if (!published) {
            auto stored = state_->store.load_request_result(state_->request);
            if (stored && stored->status == ResultStatus::ready &&
                stored->payload.dump() ==
                    result_payload(result, state_->actions).dump()) {
                published_receipts.push_back(
                    state_->handle.paths.results /
                    Path(state_->request.value + ".json"));
            }
            co_return fail_publication(published.error().message);
        }
        std::error_code cleanup_error;
        std::filesystem::remove_all(state_->work.fs(), cleanup_error);
    } else {
        result.outcome = std::move(ready);
    }
    state_->completed = result;
    co_return result;
}

std::expected<CoreBuildOperation, BuildError> CoreBuildService::start(
        const ProjectHandle& project, BuildOptions options) {
    using namespace build_detail;
    if (!project.snapshot) {
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_request, {},
            "project handle has no snapshot",
        });
    }
    if (options.request.value.empty())
        options.request.value = request_id();
    if (!safe_request_id(options.request.value)) {
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_request, {},
            "build request identity contains unsafe characters",
        });
    }
    if (!options.input_overrides.empty() &&
        options.publication != BuildPublication::transient) {
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_request, {},
            "overlaid inputs require transient build publication",
        });
    }

    auto key = context_key(project.context);
    if (!key) {
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_request, {}, key.error().message,
        });
    }
    BuildRequestId operation_request = options.request;
    auto state = std::make_shared<OperationState>(
        project, std::move(options), std::move(operation_request), *key);
    auto previous_result = state->store.load_request_result(state->request);
    if (previous_result ||
        previous_result.error().code != StateErrorCode::not_found ||
        state->work.exists()) {
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_request, state->work,
            "build request identity has already been used",
        });
    }
    std::error_code reservation_error;
    std::filesystem::create_directories(state->handle.paths.work.fs(),
                                        reservation_error);
    if (reservation_error ||
        !std::filesystem::create_directory(state->work.fs(),
                                           reservation_error)) {
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_request, state->work,
            "build request work directory could not be reserved exclusively",
        });
    }
    const auto& graph = project.snapshot->build_graph;
    state->selected.assign(graph.actions.size(),
                           state->options.required_outputs.empty());
    if (!state->options.required_outputs.empty()) {
        for (const auto& required : state->options.required_outputs) {
            bool found = false;
            for (std::size_t i = 0; i < graph.actions.size(); ++i) {
                if (std::ranges::any_of(graph.actions[i].outputs,
                    [&](const Path& output) {
                        return normalized(output) == normalized(required);
                    })) {
                    state->selected[i] = true;
                    found = true;
                }
            }
            if (!found) {
                return std::unexpected(BuildError{
                    BuildErrorCode::invalid_request, required,
                    "required output has no producer in the project graph",
                });
            }
        }
        for (std::size_t cursor = graph.actions.size(); cursor-- > 0;) {
            if (!state->selected[cursor]) continue;
            for (auto dependency : graph.actions[cursor].depends_on) {
                if (dependency >= cursor) {
                    return std::unexpected(BuildError{
                        BuildErrorCode::invalid_graph, {},
                        "build graph is not in dependency order",
                    });
                }
                state->selected[dependency] = true;
            }
        }
    }

    std::map<std::string, std::size_t> producer;
    for (std::size_t i = 0; i < graph.actions.size(); ++i) {
        for (const auto& output : graph.actions[i].outputs) {
            if (!producer.emplace(normalized(output), i).second) {
                return std::unexpected(BuildError{
                    BuildErrorCode::invalid_graph, output,
                    "multiple build actions produce the same path",
                });
            }
        }
    }

    std::map<std::string, std::string> overrides;
    for (const auto& item : state->options.input_overrides) {
        if (!overrides.emplace(normalized(item.path), item.content).second) {
            return std::unexpected(BuildError{
                BuildErrorCode::invalid_request, item.path,
                "input is overlaid more than once",
            });
        }
    }

    for (std::size_t i = 0; i < graph.actions.size(); ++i) {
        if (!state->selected[i]) continue;
        const auto& action = graph.actions[i];
        for (const auto& input : action.inputs) {
            auto generated = producer.find(normalized(input));
            if (generated != producer.end() && state->selected[generated->second])
                continue;
            const std::string key = normalized(input);
            if (state->input_by_path.contains(key)) continue;
            auto override_content = override_for(overrides, input);
            auto content = override_content ? override_content : read_file(input);
            if (!content) {
                return std::unexpected(BuildError{
                    BuildErrorCode::missing_input, input,
                    "build input was not found",
                });
            }
            InputIdentity identity{input, SHA256::hex(*content),
                                   content->size()};
            Path captured = state->work / "inputs" /
                            Path(SHA256::hex(key) + input.extension().string());
            if (!write_file(captured, *content)) {
                return std::unexpected(BuildError{
                    BuildErrorCode::io, captured,
                    "failed to capture build input",
                });
            }
            state->input_by_path.emplace(key, state->inputs.size());
            state->inputs.push_back(
                CapturedInput{std::move(identity), captured});
        }
    }

    std::vector<std::string> keys(graph.actions.size());
    std::vector<bool> dirty(graph.actions.size(), false);
    for (std::size_t i = 0; i < graph.actions.size(); ++i) {
        if (!state->selected[i]) continue;
        const auto& action = graph.actions[i];
        std::vector<InputIdentity> inputs;
        for (const auto& input : action.inputs) {
            auto generated = producer.find(normalized(input));
            if (generated != producer.end() && state->selected[generated->second])
                continue;
            inputs.push_back(
                state->inputs[state->input_by_path.at(normalized(input))].identity);
        }
        std::vector<std::string> dependencies;
        for (auto dependency : action.depends_on) {
            if (dependency >= i || !state->selected[dependency]) {
                return std::unexpected(BuildError{
                    BuildErrorCode::invalid_graph, {},
                    "build action has an invalid dependency",
                });
            }
            dependencies.push_back(keys[dependency]);
        }
        keys[i] = action_cache_key(action, inputs, dependencies);
        const bool dependency_dirty = std::ranges::any_of(
            action.depends_on,
            [&](std::size_t dependency) { return dirty[dependency]; });
        std::optional<std::vector<ArtifactIdentity>> reusable;
        if (state->options.publication == BuildPublication::persistent &&
            !dependency_dirty) {
            reusable = matching_receipt(*state, action, keys[i]);
        }
        dirty[i] = state->options.publication == BuildPublication::transient ||
                   dependency_dirty || !reusable;
        if (reusable)
            state->reusable_outputs.emplace(i, std::move(*reusable));
    }

    for (std::size_t i = 0; i < graph.actions.size(); ++i) {
        if (!state->selected[i] || !dirty[i]) continue;
        for (const auto& input : graph.actions[i].inputs) {
            auto generated = producer.find(normalized(input));
            if (generated == producer.end() || dirty[generated->second]) continue;
            const std::string key = normalized(input);
            if (state->input_by_path.contains(key)) continue;
            const auto& outputs = state->reusable_outputs.at(generated->second);
            auto captured = std::ranges::find_if(
                outputs, [&](const ArtifactIdentity& output) {
                    return normalized(output.path) == key;
                });
            if (captured == outputs.end()) {
                return std::unexpected(BuildError{
                    BuildErrorCode::invalid_graph, input,
                    "reused producer does not contain its declared output",
                });
            }
            Path captured_path = state->work / "inputs" /
                Path(SHA256::hex(key) + input.extension().string());
            auto content = read_file(input);
            if (!content || content->size() != captured->size ||
                SHA256::hex(*content) != captured->digest) {
                return std::unexpected(BuildError{
                    BuildErrorCode::missing_input, input,
                    "reused build artifact changed before capture",
                });
            }
            if (!write_file(captured_path, *content)) {
                return std::unexpected(BuildError{
                    BuildErrorCode::io, captured_path,
                    "failed to capture reused build artifact",
                });
            }
            state->input_by_path.emplace(key, state->inputs.size());
            state->inputs.push_back(CapturedInput{
                InputIdentity{input, captured->digest, captured->size},
                captured_path});
        }
    }

    if (state->options.progress) {
        std::set<std::string> announced;
        for (std::size_t i = 0; i < graph.actions.size(); ++i) {
            if (!state->selected[i]) continue;
            const auto& action = graph.actions[i];
            if (dirty[i] && announced.insert(action.moid_id).second) {
                state->options.progress(BuildProgress{
                    BuildProgressKind::moid_started, action.moid_id,
                    action.moid, action.moid_version, action.id,
                    action.description});
            }
            if (state->options.verbose || !dirty[i]) {
                state->options.progress(BuildProgress{
                    dirty[i] ? BuildProgressKind::action_scheduled
                             : BuildProgressKind::action_reused,
                    action.moid_id, action.moid, action.moid_version,
                    action.id, action.description});
            }
        }
    }

    for (std::size_t i = 0; i < graph.actions.size(); ++i) {
        if (!state->selected[i] || !dirty[i]) continue;
        for (const auto& output : graph.actions[i].outputs) {
            auto staged = staged_path(project.paths, state->work, output);
            if (!staged) {
                return std::unexpected(BuildError{
                    BuildErrorCode::invalid_graph, output,
                    "build output escapes the context output root",
                });
            }
            state->staged_by_output.emplace(normalized(output), *staged);
        }
    }

    std::optional<Path> overlay;
    if (!state->inputs.empty() || !state->staged_by_output.empty()) {
        json::Value vfs;
        vfs["version"] = 0;
        vfs["case-sensitive"] = "true";
        vfs["use-external-names"] = "false";
        vfs["roots"] = json::Value::array();
        auto add_file = [&](const Path& name, const Path& external) {
            json::Value file;
            file["type"] = "file";
            file["name"] = name.string();
            file["external-contents"] = external.string();
            vfs["roots"].push_back(std::move(file));
        };
        for (const auto& input : state->inputs)
            add_file(input.identity.path, input.captured_path);
        for (const auto& [output, staged] : state->staged_by_output)
            add_file(Path(output), staged);
        overlay = state->work / "inputs.vfs.json";
        if (!write_file(*overlay, vfs.dump())) {
            return std::unexpected(BuildError{
                BuildErrorCode::io, *overlay,
                "failed to write captured-input overlay",
            });
        }
    }

    for (std::size_t i = 0; i < graph.actions.size(); ++i) {
        if (!state->selected[i]) continue;
        const auto& action = graph.actions[i];
        ActionWork work;
        work.index = i;
        work.cache_key = keys[i];
        work.dirty = dirty[i];
        state->work_by_action.emplace(i, state->actions.size());
        if (!work.dirty) {
            state->actions.push_back(std::move(work));
            continue;
        }

        std::map<std::string, Path> remaps;
        if (action.is_compile()) {
            for (const auto& output : action.outputs)
                remaps.emplace(normalized(output),
                               state->staged_by_output.at(normalized(output)));
        } else {
            remaps.insert(state->staged_by_output.begin(),
                          state->staged_by_output.end());
        }
        for (const auto& output : action.outputs) {
            Path staged = state->staged_by_output.at(normalized(output));
            staged.parent().mkdir_recursive();
            work.staged_outputs.push_back(staged);
        }
        if (!action.is_compile()) {
            for (const auto& input : state->inputs)
                remaps.emplace(normalized(input.identity.path),
                               input.captured_path);
        }
        auto command = remap_command(action, remaps, overlay);
        if (command.empty()) {
            return std::unexpected(BuildError{
                BuildErrorCode::invalid_graph, {},
                "build action has no command",
            });
        }
        ProcessSpec process;
        process.file = command.front();
        process.args = std::move(command);
        process.cwd = graph.project_root.string();
        RuntimeSubmitOptions submit;
        submit.lane = state->options.lane;
        for (auto dependency : action.depends_on) {
            auto dependency_work = state->work_by_action.find(dependency);
            if (dependency_work != state->work_by_action.end()) {
                const auto& request =
                    state->actions[dependency_work->second].runtime;
                if (request) submit.dependencies.push_back(request);
            }
        }
        work.runtime = runtime_.submit_process(std::move(process),
                                               std::move(submit));
        state->actions.push_back(std::move(work));
    }

    return CoreBuildOperation(std::move(state));
}

std::expected<void, BuildError> CoreBuildService::validate_ready(
        const ProjectHandle& project, const BuildResult& result) const {
    auto publication = build_detail::ready_publication(project, result);
    if (!publication) return std::unexpected(publication.error());
    const auto& ready = std::get<ReadyBuild>(result.outcome);
    for (const auto& artifact : ready.artifacts) {
        auto content = read_file(artifact.path);
        if (!content || content->size() != artifact.size ||
            SHA256::hex(*content) != artifact.digest) {
            return std::unexpected(BuildError{
                BuildErrorCode::missing_input, artifact.path,
                "ready artifact no longer matches its build result",
            });
        }
    }
    return {};
}

std::expected<CoreExecutionCapture, BuildError>
CoreBuildService::capture_execution(
        const ProjectHandle& project, const BuildResult& result,
        const ArtifactIdentity& executable) const {
    auto publication = build_detail::ready_publication(project, result);
    if (!publication) return std::unexpected(publication.error());
    const auto& ready = std::get<ReadyBuild>(result.outcome);
    auto selected = std::ranges::find_if(
        ready.artifacts, [&](const ArtifactIdentity& artifact) {
            return build_detail::same_artifact(artifact, executable);
        });
    if (selected == ready.artifacts.end()) {
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_request, executable.path,
            "execution artifact is not part of this build result",
        });
    }
    auto below = [](const Path& path, const Path& directory) {
        auto relative = path.fs().lexically_normal().lexically_relative(
            directory.fs().lexically_normal());
        const std::string name = relative.generic_string();
        return !relative.empty() && !relative.is_absolute() && name != ".." &&
               !name.starts_with("../");
    };
    if (!below(selected->path, project.paths.bin)) {
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_request, selected->path,
            "execution artifact is not a project executable",
        });
    }

    const auto& graph = project.snapshot->build_graph;
    std::optional<std::size_t> selected_action;
    for (std::size_t index = 0; index < graph.actions.size(); ++index) {
        if (std::ranges::any_of(
                graph.actions[index].outputs, [&](const Path& output) {
                    return build_detail::normalized(output) ==
                           build_detail::normalized(selected->path);
                })) {
            selected_action = index;
            break;
        }
    }
    if (!selected_action) {
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_graph, selected->path,
            "execution artifact has no producer in the project graph",
        });
    }
    std::vector<bool> closure(graph.actions.size(), false);
    std::vector<std::size_t> pending{*selected_action};
    while (!pending.empty()) {
        const std::size_t index = pending.back();
        pending.pop_back();
        if (index >= graph.actions.size()) {
            return std::unexpected(BuildError{
                BuildErrorCode::invalid_graph, {},
                "execution dependency is outside the project graph",
            });
        }
        if (closure[index]) continue;
        closure[index] = true;
        for (const auto dependency : graph.actions[index].depends_on)
            pending.push_back(dependency);
    }

    std::vector<const ArtifactIdentity*> capture{&*selected};
    for (std::size_t index = 0; index < graph.actions.size(); ++index) {
        if (!closure[index] || graph.actions[index].type !=
                                   BuildAction::Type::Link)
            continue;
        for (const auto& output : graph.actions[index].outputs) {
            if (!below(output, project.paths.lib)) continue;
            auto artifact = std::ranges::find_if(
                ready.artifacts, [&](const ArtifactIdentity& candidate) {
                    return build_detail::normalized(candidate.path) ==
                           build_detail::normalized(output);
                });
            if (artifact == ready.artifacts.end()) {
                return std::unexpected(BuildError{
                    BuildErrorCode::invalid_request, output,
                    "shared-library dependency is absent from the build result",
                });
            }
            capture.push_back(&*artifact);
        }
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(project.paths.work.fs(),
                                        filesystem_error);
    if (filesystem_error) {
        return std::unexpected(BuildError{
            BuildErrorCode::io, project.paths.work,
            "execution capture directory could not be created",
        });
    }
    Path root;
    for (int attempt = 0; attempt < 8; ++attempt) {
        filesystem_error.clear();
        Path candidate = project.paths.work /
            Path("execution-" + build_detail::request_id());
        if (std::filesystem::create_directory(candidate.fs(),
                                              filesystem_error)) {
            root = std::move(candidate);
            break;
        }
        if (filesystem_error) break;
    }
    if (root.string().empty()) {
        return std::unexpected(BuildError{
            BuildErrorCode::io, project.paths.work,
            "execution capture could not be reserved exclusively",
        });
    }
    CoreExecutionCapture result_capture(root, Path());
    Path captured_executable;
    std::set<std::string> destinations;
    for (const auto* artifact : capture) {
        const bool is_executable = artifact == &*selected;
        Path destination = root /
            Path(is_executable || project.context.target.is_windows()
                     ? "bin" : "lib") /
            Path(artifact->path.filename_string());
        if (!destinations.insert(
                build_detail::normalized(destination)).second)
            continue;
        auto content = read_file(artifact->path);
        if (!content || content->size() != artifact->size ||
            SHA256::hex(*content) != artifact->digest) {
            return std::unexpected(BuildError{
                BuildErrorCode::missing_input, artifact->path,
                "execution input no longer matches its build result",
            });
        }
        if (!write_file(destination, *content)) {
            return std::unexpected(BuildError{
                BuildErrorCode::io, destination,
                "failed to write stable execution input",
            });
        }
        if (is_executable) {
            filesystem_error.clear();
            auto permissions = std::filesystem::status(
                artifact->path.fs(), filesystem_error).permissions();
            if (!filesystem_error)
                std::filesystem::permissions(
                    destination.fs(), permissions,
                    std::filesystem::perm_options::replace,
                    filesystem_error);
            if (filesystem_error) {
                return std::unexpected(BuildError{
                    BuildErrorCode::io, destination,
                    "failed to preserve executable permissions",
                });
            }
            captured_executable = destination;
        }
    }
    result_capture.executable_ = std::move(captured_executable);
    return std::move(result_capture);
}

}
