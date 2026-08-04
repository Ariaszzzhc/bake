module;

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <random>

#include <nlohmann/json.hpp>

export module bake.package;

import bake.util;
import bake.project;

// ============================================================
// bake.package — Resolver, Fetcher, Cache
// ============================================================

namespace bake {

// ===== Resolver configuration =====

export struct ResolverConfig {
    bool offline = false;
    bool locked = false;
    bool frozen = false;
};

// ===== Resolver =====

export class Resolver {
public:
    Resolver(Path cache_dir = get_cache_dir()) : cache_dir_(std::move(cache_dir)) {}

    // Resolve all dependencies from manifest.
    // Returns a Lockfile on success.
    std::optional<Lockfile> resolve(const Manifest& manifest, const ResolverConfig& config);

    // Resolve a single dependency: tag → commit → download → hash → extract
    // Returns the lock node
    std::optional<LockNode> resolve_dependency(const Dependency& dep);

private:
    Path cache_dir_;

    // tag → commit via git ls-remote
    std::optional<std::string> resolve_tag(const std::string& url, const std::string& tag);

    // Download archive by commit, returns path to downloaded file
    std::optional<Path> download_archive(const std::string& url, const std::string& commit,
                                          const Path& dest_dir);

    // Compute transport SHA-256 of a file
    std::string file_hash(const Path& file);

    // Safely extract tarball to directory (strips top-level prefix)
    bool extract_archive(const Path& archive, const Path& dest_dir);

    // Compute tree SHA-256 (normalized hash of all files in directory)
    std::string compute_tree_hash(const Path& dir);

    // Generate node ID from name and tag
    std::string make_node_id(const std::string& name, const std::string& tag);
};

// ===== Implementation =====

inline std::string Resolver::make_node_id(const std::string& name, const std::string& tag) {
    return name + "-" + tag;
}

inline std::optional<std::string> Resolver::resolve_tag(const std::string& url, const std::string& tag) {
    // Query both the tag ref and its peeled form (^{}).
    // For annotated tags, ^{} gives the commit SHA; for lightweight tags
    // (which point directly at commits) only the plain ref appears.
    std::vector<std::string> cmd = {
        "git", "ls-remote", url,
        "refs/tags/" + tag,
        "refs/tags/" + tag + "^{}"
    };
    auto result = run_process(cmd, Path(), true);
    if (!result.success()) return std::nullopt;

    // Prefer the peeled (^{}) entry; fall back to the plain ref.
    std::string commit;
    std::string fallback_commit;

    auto& out = result.stdout_output;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t tab = out.find('\t', pos);
        if (tab == std::string::npos) break;
        size_t eol = out.find('\n', tab);
        if (eol == std::string::npos) eol = out.size();

        std::string sha = out.substr(pos, tab - pos);
        std::string ref = out.substr(tab + 1, eol - tab - 1);

        // Trim whitespace
        while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r' || sha.back() == ' '))
            sha.pop_back();

        if (ref.find("^{}") != std::string::npos) {
            commit = sha;  // peeled commit — always prefer
        } else if (fallback_commit.empty()) {
            fallback_commit = sha;
        }

        pos = eol + 1;
    }

    if (!commit.empty()) return commit;
    if (!fallback_commit.empty()) return fallback_commit;
    return std::nullopt;
}

inline std::string Resolver::file_hash(const Path& file) {
    return SHA256::hex_file(file);
}

// Build the archive download URL for a given host.
inline std::string build_archive_url(const std::string& url, const std::string& commit) {
    // Normalize: strip trailing .git
    std::string base = url;
    if (ends_with(base, ".git")) {
        base = base.substr(0, base.size() - 4);
    }

    // Detect host
    if (base.find("gitlab.com") != std::string::npos ||
        base.find("gitlab") != std::string::npos) {
        // GitLab: <base>/-/archive/<commit>/<repo>.tar.gz
        size_t last_slash = base.find_last_of('/');
        std::string repo = (last_slash != std::string::npos)
                           ? base.substr(last_slash + 1) : "repo";
        return base + "/-/archive/" + commit + "/" + repo + ".tar.gz";
    }

    // GitHub and GitHub-compatible hosts (default):
    // <base>/archive/<commit>.tar.gz
    return base + "/archive/" + commit + ".tar.gz";
}

inline std::optional<Path> Resolver::download_archive(const std::string& url,
                                                       const std::string& commit,
                                                       const Path& dest_dir) {
    dest_dir.mkdir_recursive();

    std::string archive_url = build_archive_url(url, commit);
    Path archive_path = dest_dir / (commit + ".tar.gz");

    std::vector<std::string> cmd = {"curl", "-sL", "-o", archive_path.string(), archive_url};
    auto result = run_process(cmd, Path(), true);
    if (!result.success()) {
        std::fprintf(stderr, "bake: failed to download %s\n", archive_url.c_str());
        return std::nullopt;
    }

    if (!archive_path.is_regular_file() || archive_path.string() == "") {
        std::fprintf(stderr, "bake: downloaded file missing\n");
        return std::nullopt;
    }

    return archive_path;
}

// Validate an extracted directory tree for safety.
// Returns true if the tree passes all checks.
inline bool validate_extracted_tree(const std::filesystem::path& tmpdir) {
    namespace fs = std::filesystem;

    const size_t MAX_FILES = 100'000;
    const uintmax_t MAX_TOTAL_SIZE = 1ULL * 1024 * 1024 * 1024;  // 1 GB

    size_t file_count = 0;
    uintmax_t total_size = 0;

    fs::path canonical_tmp = fs::canonical(tmpdir);

    for (auto& entry : fs::recursive_directory_iterator(
            tmpdir, fs::directory_options::follow_directory_symlink)) {
        // Defense in depth: reject any relative path containing ..
        auto rel = fs::relative(entry.path(), tmpdir);
        if (rel.string().find("..") != std::string::npos) {
            std::fprintf(stderr, "bake: rejected path traversal in archive: %s\n",
                         rel.string().c_str());
            return false;
        }

        if (entry.is_symlink()) {
            // Resolve the symlink target and ensure it stays inside tmpdir.
            std::error_code ec;
            fs::path resolved = fs::canonical(entry.path(), ec);
            if (ec) {
                std::fprintf(stderr, "bake: broken symlink in archive: %s\n",
                             entry.path().string().c_str());
                return false;
            }
            // Check that resolved path starts with canonical_tmp.
            std::string r = resolved.string();
            std::string t = canonical_tmp.string();
            if (r.rfind(t, 0) != 0) {
                std::fprintf(stderr, "bake: symlink escapes extraction directory: %s -> %s\n",
                             entry.path().string().c_str(), r.c_str());
                return false;
            }
        }

        if (entry.is_regular_file()) {
            file_count++;
            total_size += entry.file_size();

            if (file_count > MAX_FILES) {
                std::fprintf(stderr, "bake: archive exceeds file count limit (%zu)\n", MAX_FILES);
                return false;
            }
            if (total_size > MAX_TOTAL_SIZE) {
                std::fprintf(stderr, "bake: archive exceeds size limit (1 GB)\n");
                return false;
            }
        }
    }

    return true;
}

inline bool Resolver::extract_archive(const Path& archive, const Path& dest_dir) {
    // Create a uniquely-named temp directory for extraction.
    std::random_device rd;
    std::uniform_int_distribution<unsigned> dist(0, 0xFFFFFF);
    std::string suffix;
    for (int i = 0; i < 6; ++i) {
        char hex[4];
        std::snprintf(hex, sizeof(hex), "%02x", dist(rd) & 0xFF);
        suffix += hex;
    }
    Path tmp_dir = cache_dir_ / (".extract-" + suffix);
    tmp_dir.remove_all();
    tmp_dir.mkdir_recursive();

    // Extract with tar — strip ownership/permissions for safety.
    std::vector<std::string> cmd = {
        "tar", "xf", archive.string(),
        "-C", tmp_dir.string(),
        "--no-same-owner",
        "--no-same-permissions"
    };
    auto result = run_process(cmd, Path(), true);
    if (!result.success()) {
        std::fprintf(stderr, "bake: failed to extract %s\n", archive.string().c_str());
        tmp_dir.remove_all();
        return false;
    }

    // Validate the extracted tree before moving it anywhere.
    if (!validate_extracted_tree(tmp_dir.fs())) {
        tmp_dir.remove_all();
        return false;
    }

    // Find the top-level directory (tarballs usually have one prefix dir).
    // If there's exactly one top-level entry and it's a directory, strip it.
    std::string prefix_dir;
    int top_count = 0;
    for (auto& entry : std::filesystem::directory_iterator(tmp_dir.fs())) {
        top_count++;
        if (top_count == 1 && entry.is_directory()) {
            prefix_dir = entry.path().filename().string();
        }
    }

    if (top_count == 1 && !prefix_dir.empty()) {
        // Move contents of prefix dir to dest_dir
        Path prefix_path = tmp_dir / prefix_dir;
        dest_dir.remove_all();
        dest_dir.parent().mkdir_recursive();
        std::filesystem::rename(prefix_path.fs(), dest_dir.fs());
        tmp_dir.remove_all();
    } else {
        // No single prefix dir, rename tmp to dest
        dest_dir.remove_all();
        dest_dir.parent().mkdir_recursive();
        std::filesystem::rename(tmp_dir.fs(), dest_dir.fs());
    }

    return true;
}

inline std::string Resolver::compute_tree_hash(const Path& dir) {
    // Collect all file paths sorted
    std::vector<std::string> files;
    for (auto& entry : std::filesystem::recursive_directory_iterator(dir.fs())) {
        if (!entry.is_regular_file()) continue;
        auto rel = std::filesystem::relative(entry.path(), dir.fs());
        files.push_back(rel.string());
    }
    std::sort(files.begin(), files.end());

    // Hash: concatenate relative path + file content hash
    std::string combined;
    for (auto& f : files) {
        Path file_path = dir / f;
        auto content = read_file(file_path);
        if (!content) continue;
        combined += f + "\0" + SHA256::hex(*content) + "\0";
    }

    return SHA256::hex(combined);
}

inline std::optional<LockNode> Resolver::resolve_dependency(const Dependency& dep) {
    if (dep.is_path_dep) {
        // Path deps don't need resolution
        LockNode node;
        node.id = make_node_id(dep.name, "path");
        node.url = "";
        node.tag = "";
        node.commit = "";
        node.native = false;
        return node;
    }

    std::fprintf(stderr, "bake: resolving %s (%s)...\n", dep.name.c_str(), dep.tag.c_str());

    // Step 1: tag → commit
    auto commit = resolve_tag(dep.url, dep.tag);
    if (!commit) {
        std::fprintf(stderr, "bake: failed to resolve tag '%s' for %s\n",
                     dep.tag.c_str(), dep.url.c_str());
        return std::nullopt;
    }

    // Step 2: compute transport hash by downloading
    Path download_dir = cache_dir_ / ".downloads";
    auto archive = download_archive(dep.url, *commit, download_dir);
    if (!archive) return std::nullopt;

    std::string transport_hash = file_hash(*archive);

    // Step 3: extract to a unique temp dir and compute tree hash
    Path extracted_dir = cache_dir_ / (".work-" + *commit);
    if (!extract_archive(*archive, extracted_dir)) return std::nullopt;

    std::string tree_hash = compute_tree_hash(extracted_dir);

    // Move to final cache location
    Path final_dir = cache_dir_ / tree_hash;
    if (!final_dir.exists()) {
        final_dir.parent().mkdir_recursive();
        std::filesystem::rename(extracted_dir.fs(), final_dir.fs());
    } else {
        extracted_dir.remove_all();
    }

    // Check if extracted dir has bake.toml (bake-native)
    bool native = (final_dir / "bake.toml").is_regular_file();

    // Clean up download
    archive->remove();

    LockNode node;
    node.id = make_node_id(dep.name, dep.tag);
    node.url = dep.url;
    node.tag = dep.tag;
    node.commit = *commit;
    node.transport_sha256 = transport_hash;
    node.tree_sha256 = tree_hash;
    node.native = native;

    return node;
}

inline std::optional<Lockfile> Resolver::resolve(const Manifest& manifest, const ResolverConfig& config) {
    if (config.offline || config.frozen) {
        std::fprintf(stderr, "bake: cannot resolve dependencies in offline/frozen mode\n");
        return std::nullopt;
    }

    cache_dir_.mkdir_recursive();

    Lockfile lockfile;

    for (auto& [name, dep] : manifest.dependencies) {
        if (dep.is_path_dep) continue;

        auto node = resolve_dependency(dep);
        if (!node) return std::nullopt;

        std::string node_id = node->id;
        lockfile.root_deps[name] = node_id;
        lockfile.nodes[node_id] = *node;

        // TODO: recursive resolution for bake-native deps
    }

    return lockfile;
}

} // namespace bake
