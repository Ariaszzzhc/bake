module;

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/stat.h>
#endif

export module bake.core.model;

import std;
import bake.util;
import bake.toolchain.target;

namespace bake {

export enum class ContextErrorCode {
    missing_workspace,
    missing_member,
    invalid_feature,
    invalid_profile,
    invalid_target,
    invalid_compiler_identity,
};

export struct ContextError {
    ContextErrorCode code;
    std::string message;
};

export struct CompilerIdentity {
    Path executable;
    std::string version;
    std::string content_digest;

    bool operator==(const CompilerIdentity&) const = default;
};

namespace detail {

struct ExecutableStamp {
    std::uintmax_t size = 0;
    std::int64_t mtime = 0;
    std::uint64_t device = 0;
    std::uint64_t file = 0;
    std::int64_t change_time = 0;
    bool valid = false;
};

inline ExecutableStamp executable_stamp(const std::filesystem::path& path) {
    ExecutableStamp stamp;
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.wstring().c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) return stamp;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(handle, &info)) {
        stamp.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
                     info.nFileSizeLow;
        stamp.mtime = (static_cast<std::int64_t>(info.ftLastWriteTime.dwHighDateTime)
                           << 32) |
                      info.ftLastWriteTime.dwLowDateTime;
        stamp.device = info.dwVolumeSerialNumber;
        stamp.file = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) |
                     info.nFileIndexLow;
        stamp.change_time =
            (static_cast<std::int64_t>(info.ftCreationTime.dwHighDateTime) << 32) |
            info.ftCreationTime.dwLowDateTime;
        stamp.valid = true;
    }
    CloseHandle(handle);
#else
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return stamp;
    stamp.size = static_cast<std::uintmax_t>(info.st_size);
#ifdef __APPLE__
    stamp.mtime = static_cast<std::int64_t>(info.st_mtimespec.tv_sec) *
                      1'000'000'000LL +
                  info.st_mtimespec.tv_nsec;
    stamp.change_time = static_cast<std::int64_t>(info.st_ctimespec.tv_sec) *
                            1'000'000'000LL +
                        info.st_ctimespec.tv_nsec;
#else
    stamp.mtime = static_cast<std::int64_t>(info.st_mtim.tv_sec) *
                      1'000'000'000LL +
                  info.st_mtim.tv_nsec;
    stamp.change_time = static_cast<std::int64_t>(info.st_ctim.tv_sec) *
                            1'000'000'000LL +
                        info.st_ctim.tv_nsec;
#endif
    stamp.device = static_cast<std::uint64_t>(info.st_dev);
    stamp.file = static_cast<std::uint64_t>(info.st_ino);
    stamp.valid = true;
#endif
    return stamp;
}

inline std::optional<std::uint64_t> parse_unsigned(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

inline std::optional<std::int64_t> parse_signed(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

inline std::optional<std::string> cached_compiler_digest(
        std::string_view content, const ExecutableStamp& expected) {
    auto lines = split(content, '\n');
    if (lines.size() != 7 || lines[0] != "bake-compiler-identity-v1")
        return std::nullopt;
    auto size = parse_unsigned(lines[1]);
    auto mtime = parse_signed(lines[2]);
    auto device = parse_unsigned(lines[3]);
    auto file = parse_unsigned(lines[4]);
    auto change_time = parse_signed(lines[5]);
    if (!size || !mtime || !device || !file || !change_time ||
        *size != expected.size || *mtime != expected.mtime ||
        *device != expected.device || *file != expected.file ||
        *change_time != expected.change_time || lines[6].size() != 64 ||
        !std::ranges::all_of(lines[6], [](unsigned char c) {
            return std::isxdigit(c);
        }))
        return std::nullopt;
    return lines[6];
}

inline std::string compiler_cache_record(const ExecutableStamp& stamp,
                                         std::string_view digest) {
    return std::format("bake-compiler-identity-v1\n{}\n{}\n{}\n{}\n{}\n{}",
                       stamp.size, stamp.mtime, stamp.device, stamp.file,
                       stamp.change_time, digest);
}

}

export std::expected<CompilerIdentity, ContextError> identify_compiler(
        const Path& executable, std::string version,
        const Path& identity_cache_directory) {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path canonical = fs::canonical(executable.fs(), error);
    if (error || !fs::is_regular_file(canonical, error)) {
        return std::unexpected(ContextError{
            ContextErrorCode::invalid_compiler_identity,
            "Bake compiler executable is not a readable regular file: " +
                executable.string(),
        });
    }
    if (version.empty()) {
        return std::unexpected(ContextError{
            ContextErrorCode::invalid_compiler_identity,
            "Bake compiler version is empty",
        });
    }

    const auto stamp = detail::executable_stamp(canonical);
    if (!stamp.valid) {
        return std::unexpected(ContextError{
            ContextErrorCode::invalid_compiler_identity,
            "failed to read Bake compiler identity: " + canonical.string(),
        });
    }

    static std::mutex mutex;
    std::lock_guard lock(mutex);
    Path cache_directory = identity_cache_directory;
    cache_directory.mkdir_recursive();
    const Path cache_path = identity_cache_directory /
        (SHA256::hex(canonical.string() + "\n" + version) + ".identity");
    std::optional<std::string> digest;
    if (auto cached = read_file(cache_path))
        digest = detail::cached_compiler_digest(*cached, stamp);
    if (!digest) {
        auto content = read_file(Path(canonical));
        if (!content) {
            return std::unexpected(ContextError{
                ContextErrorCode::invalid_compiler_identity,
                "failed to read Bake compiler executable: " + canonical.string(),
            });
        }
        digest = SHA256::hex(*content);
        if (!atomic_write_file(
                cache_path, detail::compiler_cache_record(stamp, *digest))) {
            return std::unexpected(ContextError{
                ContextErrorCode::invalid_compiler_identity,
                "failed to persist Bake compiler identity cache: " +
                    cache_path.string(),
            });
        }
    }
    return CompilerIdentity{Path(canonical), std::move(version),
                            std::move(*digest)};
}

export struct ProjectContextKey {
    std::string value;
    auto operator<=>(const ProjectContextKey&) const = default;
};

export struct ProjectContext {
    Path workspace_root;
    std::optional<Path> member_root;
    std::vector<std::string> features;
    std::string profile;
    TargetSpec target;
    CompilerIdentity compiler;
};

namespace detail {

inline bool is_canonical_absolute(const Path& path) {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path canonical = fs::canonical(path.fs(), error);
    return !error && path.fs().is_absolute() && canonical == path.fs();
}

inline bool valid_path_component(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos &&
           name.find('\0') == std::string_view::npos;
}

inline bool valid_feature(std::string_view name) {
    return !name.empty() && name.find('\0') == std::string_view::npos;
}

inline void append_identity_field(std::string& out, std::string_view value) {
    out += std::to_string(value.size());
    out += ':';
    out += value;
    out += '\n';
}

}

export std::expected<ProjectContext, ContextError> make_project_context(
        const Path& workspace_root, const std::optional<Path>& member_root,
        std::vector<std::string> features, std::string profile,
        TargetSpec target, CompilerIdentity compiler) {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path workspace = fs::canonical(workspace_root.fs(), error);
    if (error || !fs::is_directory(workspace, error)) {
        return std::unexpected(ContextError{
            ContextErrorCode::missing_workspace,
            "workspace root is not an existing directory: " +
                workspace_root.string(),
        });
    }
    std::optional<Path> canonical_member;
    if (member_root) {
        fs::path member = fs::canonical(member_root->fs(), error);
        if (error || !fs::is_directory(member, error)) {
            return std::unexpected(ContextError{
                ContextErrorCode::missing_member,
                "workspace member is not an existing directory: " +
                    member_root->string(),
            });
        }
        canonical_member = Path(member);
    }
    if (!detail::valid_path_component(profile)) {
        return std::unexpected(ContextError{
            ContextErrorCode::invalid_profile,
            "invalid profile name '" + profile + "'",
        });
    }
    for (const auto& feature : features) {
        if (!detail::valid_feature(feature)) {
            return std::unexpected(ContextError{
                ContextErrorCode::invalid_feature,
                "invalid feature name '" + feature + "'",
            });
        }
    }
    std::ranges::sort(features);
    features.erase(std::unique(features.begin(), features.end()), features.end());

    if (target.triple_.empty() || target.triple_.find('-') == std::string::npos ||
        target.triple_ == "unknown-unknown-none") {
        return std::unexpected(ContextError{
            ContextErrorCode::invalid_target,
            "target must contain a complete architecture and ABI triple",
        });
    }
    if (!detail::is_canonical_absolute(compiler.executable) ||
        compiler.version.empty() || compiler.content_digest.size() != 64 ||
        !std::ranges::all_of(compiler.content_digest, [](unsigned char c) {
            return std::isxdigit(c);
        })) {
        return std::unexpected(ContextError{
            ContextErrorCode::invalid_compiler_identity,
            "Bake compiler identity is incomplete",
        });
    }

    return ProjectContext{
        Path(workspace), std::move(canonical_member), std::move(features),
        std::move(profile),
        std::move(target), std::move(compiler),
    };
}

export std::expected<ProjectContextKey, ContextError> context_key(
        const ProjectContext& context) {
    if (!detail::is_canonical_absolute(context.workspace_root)) {
        return std::unexpected(ContextError{
            ContextErrorCode::missing_workspace,
            "workspace root is not canonical",
        });
    }
    if (context.member_root &&
        !detail::is_canonical_absolute(*context.member_root)) {
        return std::unexpected(ContextError{
            ContextErrorCode::missing_member,
            "workspace member is not canonical",
        });
    }
    auto validated = make_project_context(
        context.workspace_root, context.member_root, context.features,
        context.profile, context.target, context.compiler);
    if (!validated) return std::unexpected(validated.error());
    if (validated->features != context.features) {
        return std::unexpected(ContextError{
            ContextErrorCode::invalid_feature,
            "project context features are not canonical",
        });
    }

    std::string identity = "bake-project-context-v1\n";
    detail::append_identity_field(identity, context.workspace_root.string());
    detail::append_identity_field(identity,
                                  context.member_root ? "selected" : "all");
    if (context.member_root)
        detail::append_identity_field(identity, context.member_root->string());
    for (const auto& feature : context.features)
        detail::append_identity_field(identity, feature);
    detail::append_identity_field(identity, context.profile);
    detail::append_identity_field(identity, context.target.triple_with_version());
    detail::append_identity_field(identity, context.compiler.executable.string());
    detail::append_identity_field(identity, context.compiler.version);
    detail::append_identity_field(identity, context.compiler.content_digest);
    return ProjectContextKey{SHA256::hex(identity)};
}

export struct ContextPaths {
    Path root;
    Path bin;
    Path lib;
    Path object;
    Path bmi;
    Path bake;
    Path core;
    Path snapshots;
    Path results;
    Path work;
    Path pch;
    Path index;
};

export std::expected<ContextPaths, ContextError> context_paths(
        const ProjectContext& context) {
    auto key = context_key(context);
    if (!key) return std::unexpected(key.error());
    Path root = context.workspace_root / "out" /
                Path(context.target.triple_with_version()) /
                Path(context.profile);
    Path bake = root / ".bake";
    Path core = bake / "core" / Path(key->value);
    return ContextPaths{
        root, root / "bin", root / "lib", root / ".obj", root / ".bmi",
        bake, core, core / "snapshots", core / "results", core / "work",
        bake / "lsp-preamble", bake / "lsp-index",
    };
}

export struct InputIdentity {
    Path path;
    std::string digest;
    std::uintmax_t size = 0;
    bool operator==(const InputIdentity&) const = default;
};

export struct CompilationUnitId {
    std::string value;
    auto operator<=>(const CompilationUnitId&) const = default;
};

export enum class SourceLanguage { c, cxx, module_interface };

export struct ModuleReference {
    std::string name;
    Path bmi;
};

export struct CompilationDescription {
    CompilationUnitId id;
    ProjectContextKey context;
    std::string owner_moid;
    SourceLanguage language = SourceLanguage::cxx;
    Path working_directory;
    Path source;
    Path planned_output;
    std::vector<std::string> driver_arguments;
    std::vector<ModuleReference> module_references;
};

export std::vector<std::string> materialize_compiler_command(
        const CompilationDescription& description,
        const CompilerIdentity& compiler) {
    std::vector<std::string> command;
    command.reserve(description.driver_arguments.size() + 1);
    command.push_back(compiler.executable.string());
    command.insert(command.end(), description.driver_arguments.begin(),
                   description.driver_arguments.end());
    return command;
}

export struct ProjectSnapshotId {
    std::string value;
    auto operator<=>(const ProjectSnapshotId&) const = default;
};

export struct BuildRequestId {
    std::string value;
    auto operator<=>(const BuildRequestId&) const = default;
};

export struct ArtifactIdentity {
    std::string owner_moid;
    std::string output_name;
    Path path;
    std::string digest;
    std::uintmax_t size = 0;
};

export struct ReadyBuild {
    std::vector<ArtifactIdentity> artifacts;
};
export struct FailedBuild { std::string message; };
export struct CancelledBuild { std::string reason; };
export struct PartialBuild {
    std::vector<ArtifactIdentity> completed_artifacts;
    std::string failed_action;
    std::string message;
};

export using BuildOutcome =
    std::variant<ReadyBuild, FailedBuild, CancelledBuild, PartialBuild>;

export enum class ResultStatus { ready, failed, cancelled, partial };

export inline ResultStatus status_of(const BuildOutcome& outcome) {
    return std::visit([](const auto& value) -> ResultStatus {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ReadyBuild>) return ResultStatus::ready;
        if constexpr (std::is_same_v<T, FailedBuild>) return ResultStatus::failed;
        if constexpr (std::is_same_v<T, CancelledBuild>)
            return ResultStatus::cancelled;
        return ResultStatus::partial;
    }, outcome);
}

export struct BuildResult {
    ProjectContextKey context;
    ProjectSnapshotId snapshot;
    BuildRequestId request;
    std::vector<InputIdentity> inputs;
    BuildOutcome outcome;
};

export struct SourceViewId {
    std::string value;
    auto operator<=>(const SourceViewId&) const = default;
};

export struct AnalysisRequestStamp {
    ProjectContextKey context;
    ProjectSnapshotId snapshot;
    SourceViewId source_view;
    Path document;
    std::int64_t document_version = 0;
};

export struct AnalysisResultStamp {
    AnalysisRequestStamp request;
    ResultStatus status = ResultStatus::ready;
};

}
