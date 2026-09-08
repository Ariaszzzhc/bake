module;

#ifdef _WIN32
  #include <windows.h>
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <sys/file.h>
  #include <unistd.h>
#endif

export module bake.core.state;

import std;
import bake.json;
import bake.util;
import bake.core.model;

namespace bake {

export enum class StateErrorCode {
    not_found,
    io,
    malformed,
    unsupported_schema,
    digest_mismatch,
    context_mismatch,
    validation_mismatch,
    conflict,
};

export struct StateError {
    StateErrorCode code;
    Path path;
    std::string message;
};

export enum class PublishDisposition { published, unchanged };

export struct SnapshotDocument {
    ProjectSnapshotId id;
    ProjectContextKey context;
    std::string validation_id;
    json::Value payload;
    Path path;
};

export struct RequestResultDocument {
    BuildRequestId request;
    ProjectContextKey context;
    ResultStatus status = ResultStatus::ready;
    json::Value payload;
    Path path;
};

export struct InvalidStateRecord {
    Path path;
    StateErrorCode code;
    std::string message;
};

export struct SnapshotScan {
    std::vector<SnapshotDocument> valid;
    std::vector<InvalidStateRecord> invalid;
};

export struct SuccessfulObservation {
    std::uint64_t generation = 0;
    ProjectSnapshotId snapshot;
    std::string validation_id;
};

namespace detail {

using Json = json::Value;

inline std::expected<std::string, StateError> required_string(
        const Json& object, std::string_view name, const Path& path) {
    const Json* value = object.find(name);
    if (!value || !value->is_string()) {
        return std::unexpected(StateError{
            StateErrorCode::malformed, path,
            "state field '" + std::string(name) + "' must be a string",
        });
    }
    return value->get<std::string>();
}

inline bool exact_fields(const Json& object,
                         std::initializer_list<std::string_view> expected) {
    if (!object.is_object() || object.size() != expected.size()) return false;
    for (auto field : expected)
        if (!object.contains(field)) return false;
    return true;
}

inline bool valid_digest(std::string_view value) {
    return value.size() == 64 &&
           std::ranges::all_of(value, [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

inline bool valid_request_id(std::string_view value) {
    return !value.empty() &&
           std::ranges::all_of(value, [](unsigned char c) {
               return std::isalnum(c) || c == '-' || c == '_';
           });
}

inline std::string status_name(ResultStatus status) {
    switch (status) {
        case ResultStatus::ready: return "ready";
        case ResultStatus::failed: return "failed";
        case ResultStatus::cancelled: return "cancelled";
        case ResultStatus::partial: return "partial";
    }
    std::unreachable();
}

inline std::expected<ResultStatus, StateError> parse_status(
        std::string_view value, const Path& path) {
    if (value == "ready") return ResultStatus::ready;
    if (value == "failed") return ResultStatus::failed;
    if (value == "cancelled") return ResultStatus::cancelled;
    if (value == "partial") return ResultStatus::partial;
    return std::unexpected(StateError{
        StateErrorCode::malformed, path,
        "unknown request result status '" + std::string(value) + "'",
    });
}

inline std::string unique_suffix() {
    static std::atomic<std::uint64_t> sequence = 0;
#ifdef _WIN32
    const auto pid = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const auto pid = static_cast<std::uint64_t>(getpid());
#endif
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto thread = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return std::format("{}.{}.{}.{}", pid, ticks, thread,
                       sequence.fetch_add(1, std::memory_order_relaxed));
}

inline bool sync_directory(const Path& directory) {
#ifdef _WIN32
    static_cast<void>(directory);
    return true;
#else
    int fd = open(directory.string().c_str(), O_RDONLY);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
#endif
}

inline std::expected<PublishDisposition, StateError> publish_no_replace(
        const Path& destination, std::string_view content) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent().fs(), error);
    if (error) {
        return std::unexpected(StateError{
            StateErrorCode::io, destination,
            "failed to create state directory: " + error.message(),
        });
    }

    if (auto existing = read_file(destination)) {
        if (*existing == content) return PublishDisposition::unchanged;
        return std::unexpected(StateError{
            StateErrorCode::conflict, destination,
            "immutable state path already contains different content",
        });
    }

    Path temporary = destination.parent() /
        ("." + destination.filename_string() + "." + unique_suffix() + ".tmp");
#ifdef _WIN32
    HANDLE handle = CreateFileW(
        temporary.fs().wstring().c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(StateError{
            StateErrorCode::io, temporary, "failed to create unique state temp file",
        });
    }
    std::size_t written = 0;
    while (written < content.size()) {
        DWORD chunk = 0;
        DWORD remaining = static_cast<DWORD>(std::min<std::size_t>(
            content.size() - written, std::numeric_limits<DWORD>::max()));
        if (!WriteFile(handle, content.data() + written, remaining, &chunk,
                       nullptr)) {
            CloseHandle(handle);
            std::filesystem::remove(temporary.fs());
            return std::unexpected(StateError{
                StateErrorCode::io, temporary, "failed to write state temp file",
            });
        }
        written += chunk;
    }
    if (!FlushFileBuffers(handle)) {
        CloseHandle(handle);
        std::filesystem::remove(temporary.fs());
        return std::unexpected(StateError{
            StateErrorCode::io, temporary, "failed to sync state temp file",
        });
    }
    CloseHandle(handle);
    if (!MoveFileExW(temporary.fs().wstring().c_str(),
                     destination.fs().wstring().c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::filesystem::remove(temporary.fs());
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
            auto existing = read_file(destination);
            if (existing && *existing == content)
                return PublishDisposition::unchanged;
            return std::unexpected(StateError{
                StateErrorCode::conflict, destination,
                "immutable state path won a concurrent conflicting publish",
            });
        }
        return std::unexpected(StateError{
            StateErrorCode::io, destination, "failed to publish state file",
        });
    }
#else
    int fd = open(temporary.string().c_str(),
                  O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return std::unexpected(StateError{
            StateErrorCode::io, temporary, "failed to create unique state temp file",
        });
    }
    std::size_t written = 0;
    while (written < content.size()) {
        ssize_t count = write(fd, content.data() + written,
                              content.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(fd);
            std::filesystem::remove(temporary.fs());
            return std::unexpected(StateError{
                StateErrorCode::io, temporary, "failed to write state temp file",
            });
        }
        written += static_cast<std::size_t>(count);
    }
    if (fsync(fd) != 0) {
        close(fd);
        std::filesystem::remove(temporary.fs());
        return std::unexpected(StateError{
            StateErrorCode::io, temporary, "failed to sync state temp file",
        });
    }
    close(fd);

    if (link(temporary.string().c_str(), destination.string().c_str()) != 0) {
        const int code = errno;
        std::filesystem::remove(temporary.fs());
        if (code == EEXIST) {
            auto existing = read_file(destination);
            if (existing && *existing == content)
                return PublishDisposition::unchanged;
            return std::unexpected(StateError{
                StateErrorCode::conflict, destination,
                "immutable state path won a concurrent conflicting publish",
            });
        }
        return std::unexpected(StateError{
            StateErrorCode::io, destination,
            "failed to publish state file: " +
                std::error_code(code, std::generic_category()).message(),
        });
    }
    std::filesystem::remove(temporary.fs());
#endif
    if (!sync_directory(destination.parent())) {
        return std::unexpected(StateError{
            StateErrorCode::io, destination.parent(),
            "state file was published but its directory could not be synced",
        });
    }
    return PublishDisposition::published;
}

inline std::expected<void, StateError> replace_durable(
        const Path& destination, std::string_view content) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent().fs(), error);
    if (error) {
        return std::unexpected(StateError{
            StateErrorCode::io, destination,
            "failed to create state directory: " + error.message(),
        });
    }
    Path temporary = destination.parent() /
        ("." + destination.filename_string() + "." + unique_suffix() + ".tmp");
#ifdef _WIN32
    HANDLE handle = CreateFileW(
        temporary.fs().wstring().c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(StateError{
            StateErrorCode::io, temporary, "failed to create observation temp file",
        });
    }
    DWORD written = 0;
    const bool wrote = content.size() <= std::numeric_limits<DWORD>::max() &&
        WriteFile(handle, content.data(), static_cast<DWORD>(content.size()),
                  &written, nullptr) && written == content.size();
    const bool synced = wrote && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!synced || !MoveFileExW(temporary.fs().wstring().c_str(),
                                destination.fs().wstring().c_str(),
                                MOVEFILE_REPLACE_EXISTING |
                                    MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary.fs());
        return std::unexpected(StateError{
            StateErrorCode::io, destination,
            "failed to atomically replace successful observation",
        });
    }
#else
    int fd = open(temporary.string().c_str(),
                  O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return std::unexpected(StateError{
            StateErrorCode::io, temporary, "failed to create observation temp file",
        });
    }
    std::size_t written = 0;
    while (written < content.size()) {
        ssize_t count = write(fd, content.data() + written,
                              content.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        written += static_cast<std::size_t>(count);
    }
    const bool synced = written == content.size() && fsync(fd) == 0;
    close(fd);
    if (synced)
        std::filesystem::rename(temporary.fs(), destination.fs(), error);
    if (!synced || error) {
        std::filesystem::remove(temporary.fs());
        return std::unexpected(StateError{
            StateErrorCode::io, destination,
            "failed to atomically replace successful observation: " +
                error.message(),
        });
    }
#endif
    if (!sync_directory(destination.parent())) {
        return std::unexpected(StateError{
            StateErrorCode::io, destination.parent(),
            "successful observation was published but its directory was not synced",
        });
    }
    return {};
}

inline std::expected<Json, StateError> parse_document(
        const Path& path, std::string_view content) {
    try {
        Json parsed = Json::parse(content);
        if (!parsed.is_object()) {
            return std::unexpected(StateError{
                StateErrorCode::malformed, path, "state document must be an object",
            });
        }
        return parsed;
    } catch (const std::exception& exception) {
        return std::unexpected(StateError{
            StateErrorCode::malformed, path,
            "invalid state JSON: " + std::string(exception.what()),
        });
    }
}

}

export class ProjectPublicationGuard {
public:
    explicit ProjectPublicationGuard(const ContextPaths& paths)
        : lock_(std::make_unique<FileLock>(
              paths.bake / "build-publication.lock")) {}

    ProjectPublicationGuard(const ProjectPublicationGuard&) = delete;
    ProjectPublicationGuard& operator=(const ProjectPublicationGuard&) = delete;
    ProjectPublicationGuard(ProjectPublicationGuard&&) noexcept = default;
    ProjectPublicationGuard& operator=(ProjectPublicationGuard&&) noexcept = default;

    bool locked() const { return lock_ && lock_->locked(); }

private:
    std::unique_ptr<FileLock> lock_;
};

export using CoreStateValidator =
    std::function<std::expected<void, StateError>(std::string_view)>;

export std::expected<PublishDisposition, StateError>
publish_immutable_file_recovering_corruption(
        const Path& destination, std::string_view content,
        const ProjectPublicationGuard& guard,
        const CoreStateValidator& validate) {
    if (!guard.locked()) {
        return std::unexpected(StateError{
            StateErrorCode::io, destination,
            "immutable state recovery requires a publication lock",
        });
    }
    if (!validate) {
        return std::unexpected(StateError{
            StateErrorCode::malformed, destination,
            "immutable state recovery requires a content validator",
        });
    }
    auto valid_content = validate(content);
    if (!valid_content) return std::unexpected(valid_content.error());

    auto published = detail::publish_no_replace(destination, content);
    if (published || published.error().code != StateErrorCode::conflict)
        return published;

    auto existing = read_file(destination);
    if (!existing) {
        return std::unexpected(StateError{
            StateErrorCode::io, destination,
            "conflicting immutable state file could not be read",
        });
    }
    if (validate(*existing)) return std::unexpected(published.error());

    auto replaced = detail::replace_durable(destination, content);
    if (!replaced) return std::unexpected(replaced.error());
    auto verified = read_file(destination);
    if (!verified || *verified != content) {
        return std::unexpected(StateError{
            StateErrorCode::io, destination,
            "repaired immutable state file does not match published content",
        });
    }
    auto valid_repair = validate(*verified);
    if (!valid_repair) return std::unexpected(valid_repair.error());
    return PublishDisposition::published;
}

export std::expected<void, StateError> replace_core_state_file(
        const Path& destination, std::string_view content,
        const ProjectPublicationGuard& guard,
        const CoreStateValidator& validate) {
    if (!guard.locked()) {
        return std::unexpected(StateError{
            StateErrorCode::io, destination,
            "Core state replacement requires a publication lock",
        });
    }
    if (!validate) {
        return std::unexpected(StateError{
            StateErrorCode::malformed, destination,
            "Core state replacement requires a content validator",
        });
    }
    auto valid_content = validate(content);
    if (!valid_content) return std::unexpected(valid_content.error());
    auto replaced = detail::replace_durable(destination, content);
    if (!replaced) return std::unexpected(replaced.error());
    auto verified = read_file(destination);
    if (!verified || *verified != content) {
        return std::unexpected(StateError{
            StateErrorCode::io, destination,
            "replaced Core state file does not match published content",
        });
    }
    return validate(*verified);
}

export class ProjectStateStore {
public:
    explicit ProjectStateStore(const ContextPaths& paths,
                               ProjectContextKey context)
        : ProjectStateStore(paths.core, std::move(context)) {}

    ProjectStateStore(Path context_root, ProjectContextKey context)
        : root_(std::move(context_root)), context_(std::move(context)),
          snapshots_(root_ / "snapshots"), results_(root_ / "results") {}

    std::expected<std::pair<ProjectSnapshotId, PublishDisposition>, StateError>
    publish_snapshot(std::string validation_id, const json::Value& payload) const {
        if (validation_id.empty() || !payload.is_object()) {
            return std::unexpected(StateError{
                StateErrorCode::malformed, snapshots_,
                "snapshot validation identity and object payload are required",
            });
        }
        detail::Json document = detail::Json::object();
        document["schema"] = "bake-core-snapshot-v1";
        document["context"] = context_.value;
        document["validation"] = validation_id;
        document["payload"] = payload;
        const std::string content = document.dump();
        ProjectSnapshotId id{SHA256::hex(content)};
        Path path = snapshots_ / (id.value + ".json");
        auto published = detail::publish_no_replace(path, content);
        if (!published) return std::unexpected(published.error());
        return std::pair{id, *published};
    }

    std::expected<std::pair<ProjectSnapshotId, PublishDisposition>, StateError>
    publish_snapshot_recovering_corruption(
            std::string validation_id, const json::Value& payload,
            const ProjectPublicationGuard& guard) const {
        if (!guard.locked()) {
            return std::unexpected(StateError{
                StateErrorCode::io, snapshots_,
                "snapshot corruption recovery requires a publication lock",
            });
        }
        auto published = publish_snapshot(validation_id, payload);
        if (published || published.error().code != StateErrorCode::conflict)
            return published;

        detail::Json document = detail::Json::object();
        document["schema"] = "bake-core-snapshot-v1";
        document["context"] = context_.value;
        document["validation"] = validation_id;
        document["payload"] = payload;
        const std::string content = document.dump();
        ProjectSnapshotId id{SHA256::hex(content)};
        auto existing = load_snapshot(id, validation_id);
        if (existing) {
            return std::unexpected(StateError{
                StateErrorCode::conflict, existing->path,
                "valid immutable snapshot conflicts with recomputed content",
            });
        }
        Path path = snapshots_ / (id.value + ".json");
        auto repaired = detail::replace_durable(path, content);
        if (!repaired) return std::unexpected(repaired.error());
        auto verified = load_snapshot(id, validation_id);
        if (!verified) return std::unexpected(verified.error());
        return std::pair{id, PublishDisposition::published};
    }

    std::expected<SnapshotDocument, StateError> load_snapshot(
            const ProjectSnapshotId& id,
            std::string_view expected_validation) const {
        Path path = snapshots_ / (id.value + ".json");
        if (!detail::valid_digest(id.value)) {
            return std::unexpected(StateError{
                StateErrorCode::malformed, path,
                "snapshot identity must be a lowercase SHA-256 digest",
            });
        }
        auto content = read_file(path);
        if (!content) {
            return std::unexpected(StateError{
                StateErrorCode::not_found, path, "snapshot document was not found",
            });
        }
        if (SHA256::hex(*content) != id.value) {
            return std::unexpected(StateError{
                StateErrorCode::digest_mismatch, path,
                "snapshot content digest does not match its filename",
            });
        }
        auto parsed = detail::parse_document(path, *content);
        if (!parsed) return std::unexpected(parsed.error());
        if (!detail::exact_fields(*parsed,
                                  {"schema", "context", "validation", "payload"})) {
            return std::unexpected(StateError{
                StateErrorCode::malformed, path,
                "snapshot document has missing or unknown fields",
            });
        }
        auto schema = detail::required_string(*parsed, "schema", path);
        if (!schema) return std::unexpected(schema.error());
        if (*schema != "bake-core-snapshot-v1") {
            return std::unexpected(StateError{
                StateErrorCode::unsupported_schema, path,
                "unsupported Core snapshot schema '" + *schema + "'",
            });
        }
        auto context = detail::required_string(*parsed, "context", path);
        if (!context) return std::unexpected(context.error());
        if (*context != context_.value) {
            return std::unexpected(StateError{
                StateErrorCode::context_mismatch, path,
                "snapshot belongs to a different project context",
            });
        }
        auto validation = detail::required_string(*parsed, "validation", path);
        if (!validation) return std::unexpected(validation.error());
        if (*validation != expected_validation) {
            return std::unexpected(StateError{
                StateErrorCode::validation_mismatch, path,
                "snapshot validation identity does not match the request",
            });
        }
        const detail::Json* payload = parsed->find("payload");
        if (!payload || !payload->is_object()) {
            return std::unexpected(StateError{
                StateErrorCode::malformed, path,
                "snapshot payload must be a complete object",
            });
        }
        return SnapshotDocument{id, context_, *validation, *payload, path};
    }

    SnapshotScan scan_snapshot_candidates() const {
        SnapshotScan scan;
        std::error_code error;
        if (!std::filesystem::is_directory(snapshots_.fs(), error)) return scan;
        std::vector<Path> paths;
        for (const auto& entry :
             std::filesystem::directory_iterator(snapshots_.fs(), error)) {
            if (error) break;
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                paths.emplace_back(entry.path());
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            const std::string filename = path.stem_string();
            auto candidate = load_snapshot(ProjectSnapshotId{filename}, "");
            if (!candidate &&
                candidate.error().code == StateErrorCode::validation_mismatch) {
                auto content = read_file(path);
                if (content) {
                    auto parsed = detail::parse_document(path, *content);
                    if (parsed) {
                        auto validation =
                            detail::required_string(*parsed, "validation", path);
                        if (validation)
                            candidate = load_snapshot(
                                ProjectSnapshotId{filename}, *validation);
                    }
                }
            }
            if (candidate) {
                scan.valid.push_back(std::move(*candidate));
            } else {
                scan.invalid.push_back(InvalidStateRecord{
                    path, candidate.error().code, candidate.error().message,
                });
            }
        }
        return scan;
    }

    std::expected<SuccessfulObservation, StateError>
    record_successful_observation(const ProjectSnapshotId& snapshot,
                                  std::string validation_id) const {
        auto stored = load_snapshot(snapshot, validation_id);
        if (!stored) return std::unexpected(stored.error());

        FileLock lock(root_ / "observation.lock");
        if (!lock.locked()) {
            return std::unexpected(StateError{
                StateErrorCode::io, root_ / "observation.lock",
                "failed to lock successful observation",
            });
        }
        std::uint64_t generation = 1;
        auto previous = latest_successful_observation();
        if (previous) generation = previous->generation + 1;
        else if (previous.error().code != StateErrorCode::not_found)
            return std::unexpected(previous.error());

        detail::Json signed_fields = detail::Json::object();
        signed_fields["schema"] = "bake-core-observation-v1";
        signed_fields["context"] = context_.value;
        signed_fields["generation"] = static_cast<std::int64_t>(generation);
        signed_fields["snapshot"] = snapshot.value;
        signed_fields["validation"] = validation_id;
        detail::Json document = signed_fields;
        document["digest"] = SHA256::hex(signed_fields.dump());
        auto replaced = detail::replace_durable(root_ / "latest.json",
                                                 document.dump());
        if (!replaced) return std::unexpected(replaced.error());
        return SuccessfulObservation{generation, snapshot,
                                     std::move(validation_id)};
    }

    std::expected<SuccessfulObservation, StateError>
    latest_successful_observation() const {
        Path path = root_ / "latest.json";
        auto content = read_file(path);
        if (!content) {
            return std::unexpected(StateError{
                StateErrorCode::not_found, path,
                "no successful Core observation has been published",
            });
        }
        auto parsed = detail::parse_document(path, *content);
        if (!parsed) return std::unexpected(parsed.error());
        if (!detail::exact_fields(*parsed, {"schema", "context", "generation",
                                            "snapshot", "validation", "digest"})) {
            return std::unexpected(StateError{
                StateErrorCode::malformed, path,
                "successful observation has missing or unknown fields",
            });
        }
        auto schema = detail::required_string(*parsed, "schema", path);
        auto context = detail::required_string(*parsed, "context", path);
        auto snapshot = detail::required_string(*parsed, "snapshot", path);
        auto validation = detail::required_string(*parsed, "validation", path);
        auto digest = detail::required_string(*parsed, "digest", path);
        if (!schema) return std::unexpected(schema.error());
        if (!context) return std::unexpected(context.error());
        if (!snapshot) return std::unexpected(snapshot.error());
        if (!validation) return std::unexpected(validation.error());
        if (!digest) return std::unexpected(digest.error());
        if (*schema != "bake-core-observation-v1") {
            return std::unexpected(StateError{
                StateErrorCode::unsupported_schema, path,
                "unsupported successful observation schema '" + *schema + "'",
            });
        }
        if (*context != context_.value) {
            return std::unexpected(StateError{
                StateErrorCode::context_mismatch, path,
                "successful observation belongs to a different context",
            });
        }
        const detail::Json* generation_value = parsed->find("generation");
        if (!generation_value || !generation_value->is_integer() ||
            generation_value->get<std::int64_t>() <= 0) {
            return std::unexpected(StateError{
                StateErrorCode::malformed, path,
                "successful observation generation must be positive",
            });
        }
        detail::Json signed_fields = detail::Json::object();
        signed_fields["schema"] = *schema;
        signed_fields["context"] = *context;
        signed_fields["generation"] = generation_value->get<std::int64_t>();
        signed_fields["snapshot"] = *snapshot;
        signed_fields["validation"] = *validation;
        if (SHA256::hex(signed_fields.dump()) != *digest) {
            return std::unexpected(StateError{
                StateErrorCode::digest_mismatch, path,
                "successful observation content digest is invalid",
            });
        }
        auto stored_snapshot = load_snapshot(ProjectSnapshotId{*snapshot},
                                             *validation);
        if (!stored_snapshot) return std::unexpected(stored_snapshot.error());
        return SuccessfulObservation{
            static_cast<std::uint64_t>(generation_value->get<std::int64_t>()),
            ProjectSnapshotId{*snapshot}, *validation,
        };
    }

    std::expected<PublishDisposition, StateError> publish_request_result(
            const BuildRequestId& request, ResultStatus status,
            const json::Value& payload) const {
        if (!detail::valid_request_id(request.value) || !payload.is_object()) {
            return std::unexpected(StateError{
                StateErrorCode::malformed, results_,
                "request identity and object payload are required",
            });
        }
        detail::Json signed_fields = detail::Json::object();
        signed_fields["schema"] = "bake-core-request-result-v1";
        signed_fields["request"] = request.value;
        signed_fields["context"] = context_.value;
        signed_fields["status"] = detail::status_name(status);
        signed_fields["payload"] = payload;
        const std::string digest = SHA256::hex(signed_fields.dump());
        detail::Json document = signed_fields;
        document["digest"] = digest;
        return detail::publish_no_replace(
            results_ / (request.value + ".json"), document.dump());
    }

    std::expected<RequestResultDocument, StateError> load_request_result(
            const BuildRequestId& request) const {
        Path path = results_ / (request.value + ".json");
        if (!detail::valid_request_id(request.value)) {
            return std::unexpected(StateError{
                StateErrorCode::malformed, path,
                "request identity contains unsafe path characters",
            });
        }
        auto content = read_file(path);
        if (!content) {
            return std::unexpected(StateError{
                StateErrorCode::not_found, path,
                "request result document was not found",
            });
        }
        auto parsed = detail::parse_document(path, *content);
        if (!parsed) return std::unexpected(parsed.error());
        if (!detail::exact_fields(*parsed, {"schema", "request", "context",
                                            "status", "payload", "digest"})) {
            return std::unexpected(StateError{
                StateErrorCode::malformed, path,
                "request result has missing or unknown fields",
            });
        }
        auto schema = detail::required_string(*parsed, "schema", path);
        auto stored_request = detail::required_string(*parsed, "request", path);
        auto context = detail::required_string(*parsed, "context", path);
        auto status_text = detail::required_string(*parsed, "status", path);
        auto digest = detail::required_string(*parsed, "digest", path);
        if (!schema) return std::unexpected(schema.error());
        if (!stored_request) return std::unexpected(stored_request.error());
        if (!context) return std::unexpected(context.error());
        if (!status_text) return std::unexpected(status_text.error());
        if (!digest) return std::unexpected(digest.error());
        if (*schema != "bake-core-request-result-v1") {
            return std::unexpected(StateError{
                StateErrorCode::unsupported_schema, path,
                "unsupported Core request result schema '" + *schema + "'",
            });
        }
        if (*stored_request != request.value) {
            return std::unexpected(StateError{
                StateErrorCode::digest_mismatch, path,
                "request result identity does not match its filename",
            });
        }
        if (*context != context_.value) {
            return std::unexpected(StateError{
                StateErrorCode::context_mismatch, path,
                "request result belongs to a different project context",
            });
        }
        const detail::Json* payload = parsed->find("payload");
        if (!payload || !payload->is_object()) {
            return std::unexpected(StateError{
                StateErrorCode::malformed, path,
                "request result payload must be an object",
            });
        }
        detail::Json signed_fields = detail::Json::object();
        signed_fields["schema"] = *schema;
        signed_fields["request"] = *stored_request;
        signed_fields["context"] = *context;
        signed_fields["status"] = *status_text;
        signed_fields["payload"] = *payload;
        if (SHA256::hex(signed_fields.dump()) != *digest) {
            return std::unexpected(StateError{
                StateErrorCode::digest_mismatch, path,
                "request result content digest is invalid",
            });
        }
        auto status = detail::parse_status(*status_text, path);
        if (!status) return std::unexpected(status.error());
        return RequestResultDocument{request, context_, *status, *payload, path};
    }

    std::vector<BuildRequestId> request_result_ids() const {
        std::vector<BuildRequestId> ids;
        std::error_code error;
        if (!std::filesystem::is_directory(results_.fs(), error)) return ids;
        for (const auto& entry :
             std::filesystem::directory_iterator(results_.fs(), error)) {
            if (error) break;
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                ids.push_back(BuildRequestId{entry.path().stem().string()});
        }
        std::ranges::sort(ids, {}, &BuildRequestId::value);
        return ids;
    }

private:
    Path root_;
    ProjectContextKey context_;
    Path snapshots_;
    Path results_;
};

}
