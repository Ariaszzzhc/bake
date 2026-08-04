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
    // Use git ls-remote to resolve tag to commit
    std::vector<std::string> cmd = {"git", "ls-remote", url, "refs/tags/" + tag};
    auto result = run_process(cmd, Path(), true);
    if (!result.success()) return std::nullopt;

    // Output format: <commit>\trefs/tags/<tag>
    auto& out = result.stdout_output;
    size_t tab = out.find('\t');
    if (tab == std::string::npos) return std::nullopt;

    std::string commit = out.substr(0, tab);
    // Trim whitespace
    while (!commit.empty() && (commit.back() == '\n' || commit.back() == '\r' || commit.back() == ' '))
        commit.pop_back();

    if (commit.empty()) return std::nullopt;
    return commit;
}

inline std::string Resolver::file_hash(const Path& file) {
    return SHA256::hex_file(file);
}

inline std::optional<Path> Resolver::download_archive(const std::string& url,
                                                       const std::string& commit,
                                                       const Path& dest_dir) {
    dest_dir.mkdir_recursive();

    // GitHub-style archive URL: <url>/archive/<commit>.tar.gz
    std::string archive_url = url;
    // Normalize: remove trailing .git
    if (ends_with(archive_url, ".git")) {
        archive_url = archive_url.substr(0, archive_url.size() - 4);
    }
    archive_url += "/archive/" + commit + ".tar.gz";

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

inline bool Resolver::extract_archive(const Path& archive, const Path& dest_dir) {
    // Create temp directory for extraction
    Path tmp_dir = dest_dir.parent() / (dest_dir.stem_string() + ".tmp");
    tmp_dir.remove_all();
    tmp_dir.mkdir_recursive();

    // Extract with tar
    std::vector<std::string> cmd = {"tar", "xf", archive.string(), "-C", tmp_dir.string()};
    auto result = run_process(cmd, Path(), true);
    if (!result.success()) {
        std::fprintf(stderr, "bake: failed to extract %s\n", archive.string().c_str());
        tmp_dir.remove_all();
        return false;
    }

    // Find the top-level directory (tarballs usually have one prefix dir)
    std::string prefix_dir;
    for (auto& entry : std::filesystem::directory_iterator(tmp_dir.fs())) {
        if (entry.is_directory()) {
            prefix_dir = entry.path().filename().string();
            break;
        }
    }

    if (!prefix_dir.empty()) {
        // Move contents of prefix dir to dest_dir
        Path prefix_path = tmp_dir / prefix_dir;
        dest_dir.remove_all();
        dest_dir.parent().mkdir_recursive();
        std::filesystem::rename(prefix_path.fs(), dest_dir.fs());
        tmp_dir.remove_all();
    } else {
        // No prefix, just rename tmp to dest
        dest_dir.remove_all();
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

    // Step 3: extract to temp dir and compute tree hash
    Path extracted_dir = cache_dir_ / "pending";
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
