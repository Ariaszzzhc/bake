module;

#include <nlohmann/json.hpp>

export module bake.package;

import std;
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

    // Resolve a single dependency AND its full transitive closure.
    // Returns a Lockfile with the dep as the only root dep.
    std::optional<Lockfile> resolve_subtree(const Dependency& root_dep,
                                             const std::string& root_name);

    // Resolve a single dependency: tag → commit → download → hash → extract
    // Returns the lock node
    std::optional<LockNode> resolve_dependency(const Dependency& dep);

    // Re-download cached sources for all lock nodes using existing locked commits.
    // Does NOT re-resolve tags. Used when cache is missing/corrupted but lock is valid.
    bool redownload(const Lockfile& lockfile, const ResolverConfig& config);

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

public:
    // Generate node ID from name and tag
    std::string make_node_id(const std::string& name, const std::string& tag);
};

// compute_tree_sha256 is exported below (as a free function) for use by
// the CLI layer's cache verification. It lives here because bake.engine
// does NOT import bake.package, so new exports here don't trigger the
// clang 22 module deserialization crash in self-bootstrap.

export inline std::string compute_tree_sha256(const Path& dir) {
    std::vector<std::pair<std::string, std::string>> entries;

    for (auto& entry : std::filesystem::recursive_directory_iterator(dir.fs())) {
        // Use lexically_relative instead of filesystem::relative.
        // filesystem::relative() resolves symlinks, so a symlink and its
        // target would hash identically. lexically_relative() does a pure
        // lexical computation that preserves the symlink's own path identity.
        auto rel = entry.path().lexically_relative(dir.fs());
        std::string path = rel.string();

        for (auto& c : path) {
            if (c == '\\') c = '/';
        }

        auto status = entry.symlink_status();
        unsigned mode = static_cast<unsigned>(status.permissions());

        std::string contribution = path;
        contribution += '|';
        contribution += std::to_string(mode);
        contribution += '|';

        if (std::filesystem::is_symlink(status)) {
            std::error_code ec;
            contribution += "S:";
            contribution += std::filesystem::read_symlink(entry.path(), ec).string();
        } else if (std::filesystem::is_regular_file(status)) {
            contribution += "F:";
            contribution += SHA256::hex_file(Path(entry.path()));
        } else if (std::filesystem::is_directory(status)) {
            contribution += "D";
        } else {
            continue;
        }

        entries.push_back({path, contribution});
    }

    std::sort(entries.begin(), entries.end());

    std::string combined;
    for (auto& [path, data] : entries) {
        combined += data;
        combined += '\n';
    }

    return SHA256::hex(combined);
}

// Verify that cached source for every lock node matches its tree_sha256.
// Detects tampering with cached dependencies. Called by the CLI layer.
export inline bool verify_lock_cache(const Lockfile& lockfile, const Path& cache_dir) {
    for (auto& [id, node] : lockfile.nodes) {
        if (node.tree_sha256.empty()) continue;

        Path cache_path = cache_dir / node.tree_sha256;
        if (!cache_path.is_directory()) {
            std::println(std::cerr,
                "bake: cache for '{}' missing at {}",
                id, cache_path.string());
            return false;
        }

        std::string actual_hash = compute_tree_sha256(cache_path);
        if (actual_hash != node.tree_sha256) {
            std::println(std::cerr,
                "bake: cache tampered for '{}' (expected {}, got {})",
                id, node.tree_sha256.substr(0, 12),
                actual_hash.substr(0, 12));
            return false;
        }
    }
    return true;
}

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
        std::println(std::cerr, "bake: failed to download {}", archive_url);
        return std::nullopt;
    }

    if (!archive_path.is_regular_file() || archive_path.string() == "") {
        std::println(std::cerr, "bake: downloaded file missing");
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
            std::println(std::cerr, "bake: rejected path traversal in archive: {}",
                         rel.string());
            return false;
        }

        if (entry.is_symlink()) {
            // Resolve the symlink target and ensure it stays inside tmpdir.
            std::error_code ec;
            fs::path resolved = fs::canonical(entry.path(), ec);
            if (ec) {
                std::println(std::cerr, "bake: broken symlink in archive: {}",
                             entry.path().string());
                return false;
            }
            // Check that resolved path starts with canonical_tmp.
            std::string r = resolved.string();
            std::string t = canonical_tmp.string();
            if (r.rfind(t, 0) != 0) {
                std::println(std::cerr, "bake: symlink escapes extraction directory: {} -> {}",
                             entry.path().string(), r);
                return false;
            }
        }

        if (entry.is_regular_file()) {
            file_count++;
            total_size += entry.file_size();

            if (file_count > MAX_FILES) {
                std::println(std::cerr, "bake: archive exceeds file count limit ({})", MAX_FILES);
                return false;
            }
            if (total_size > MAX_TOTAL_SIZE) {
                std::println(std::cerr, "bake: archive exceeds size limit (1 GB)");
                return false;
            }
        }
    }

    return true;
}

// Pre-scan a tarball's entry list to detect malicious content BEFORE
// extracting. This prevents path traversal, symlink escapes, and zip bombs
// from ever touching the filesystem.
// Returns true if the archive passes all safety checks.
inline bool prescan_archive(const Path& archive, size_t& out_file_count,
                            uintmax_t& out_total_size) {
    const size_t MAX_FILES = 100'000;
    const uintmax_t MAX_TOTAL_SIZE = 1ULL * 1024 * 1024 * 1024;  // 1 GB

    // List all entries with metadata using `tar -tvf` (verbose).
    // This gives us type, size, and symlink target for each entry.
    auto result = run_process(
        {"tar", "-tvf", archive.string()}, Path(), true);
    if (!result.success()) {
        std::println(std::cerr, "bake: failed to list archive entries");
        return false;
    }

    size_t file_count = 0;
    uintmax_t total_size = 0;

    // Parse the verbose listing line by line.
    // Format example (BSD tar):
    //   -rw-r--r--  0 user group     1234 Jan  1 12:00 prefix/file.txt
    //   lrwxrwxrwx  0 user group        8 Jan  1 12:00 prefix/link -> target
    const auto& listing = result.stdout_output;
    size_t pos = 0;
    while (pos < listing.size()) {
        size_t eol = listing.find('\n', pos);
        if (eol == std::string::npos) eol = listing.size();

        std::string line = listing.substr(pos, eol - pos);
        pos = eol + 1;

        if (line.empty()) continue;

        char type_char = line[0];

        // Find path: after timestamp pattern "HH:MM " in the line
        size_t time_end = std::string::npos;
        for (size_t i = 10; i + 1 < line.size(); ++i) {
            if (line[i] == ':' && std::isdigit(static_cast<unsigned char>(line[i - 1]))) {
                if (i + 2 < line.size() && line[i + 1] == ' ') {
                    time_end = i + 2;
                    break;
                }
                if (i + 3 < line.size() && std::isdigit(static_cast<unsigned char>(line[i + 1]))
                    && line[i + 2] == ' ') {
                    time_end = i + 3;
                    break;
                }
            }
        }
        if (time_end == std::string::npos) continue;
        while (time_end < line.size() && line[time_end] == ' ') time_end++;

        std::string entry;
        std::string link_target;
        if (type_char == 'l') {
            size_t arrow = line.find(" -> ", time_end);
            if (arrow != std::string::npos) {
                entry = line.substr(time_end, arrow - time_end);
                link_target = line.substr(arrow + 4);
            } else {
                entry = line.substr(time_end);
            }
        } else {
            entry = line.substr(time_end);
        }

        for (auto& c : entry) { if (c == '\\') c = '/'; }
        for (auto& c : link_target) { if (c == '\\') c = '/'; }
        while (!entry.empty() && (entry.back() == '\r' || entry.back() == ' '))
            entry.pop_back();
        while (!link_target.empty() && (link_target.back() == '\r' || link_target.back() == ' '))
            link_target.pop_back();

        if (entry.empty()) continue;

        // Reject absolute paths
        if (!entry.empty() && entry[0] == '/') {
            std::println(std::cerr,
                "bake: rejecting archive entry with absolute path: {}",
                entry);
            return false;
        }

        // Reject path traversal (any component that is "..")
        {
            size_t dotdot = 0;
            while ((dotdot = entry.find("..", dotdot)) != std::string::npos) {
                bool left_ok = (dotdot == 0 || entry[dotdot - 1] == '/');
                bool right_ok = (dotdot + 2 >= entry.size() ||
                                entry[dotdot + 2] == '/');
                if (left_ok && right_ok) {
                    std::println(std::cerr,
                        "bake: rejecting path traversal in archive: {}",
                        entry);
                    return false;
                }
                dotdot += 2;
            }
        }

        // Reject symlinks that escape the archive root
        if (type_char == 'l' && !link_target.empty()) {
            if (link_target[0] == '/') {
                std::println(std::cerr,
                    "bake: rejecting symlink with absolute target: {} -> {}",
                    entry, link_target);
                return false;
            }
            size_t dd = 0;
            while ((dd = link_target.find("..", dd)) != std::string::npos) {
                bool left_ok = (dd == 0 || link_target[dd - 1] == '/');
                bool right_ok = (dd + 2 >= link_target.size() ||
                                link_target[dd + 2] == '/');
                if (left_ok && right_ok) {
                    std::println(std::cerr,
                        "bake: rejecting symlink escaping archive: {} -> {}",
                        entry, link_target);
                    return false;
                }
                dd += 2;
            }
        }

        // Reject hardlinks entirely
        if (type_char == 'h' || (type_char == 'l' && line.find(" link to ") != std::string::npos)) {
            std::println(std::cerr,
                "bake: rejecting hardlink entry in archive: {}",
                entry);
            return false;
        }

        // Accumulate size for regular files
        if (type_char == '-') {
            std::string meta = line.substr(0, time_end);
            std::istringstream iss(meta);
            std::string field;
            std::vector<std::string> fields;
            while (iss >> field) fields.push_back(field);
            if (fields.size() >= 5) {
                uintmax_t fsize = 0;
                std::istringstream sz(fields[4]);
                sz >> fsize;
                total_size += fsize;
            }
            file_count++;
        }

        if (file_count > MAX_FILES) {
            std::println(std::cerr,
                "bake: archive exceeds file count limit ({} entries)",
                MAX_FILES);
            return false;
        }
        if (total_size > MAX_TOTAL_SIZE) {
            std::println(std::cerr,
                "bake: archive exceeds size limit (1 GB)");
            return false;
        }
    }

    out_file_count = file_count;
    out_total_size = total_size;
    return true;
}

inline bool Resolver::extract_archive(const Path& archive, const Path& dest_dir) {
    // Phase 1: Pre-scan archive entries BEFORE extraction.
    // This prevents malicious archives from writing to the filesystem
    // before safety checks can run.
    size_t file_count = 0;
    uintmax_t total_size = 0;
    if (!prescan_archive(archive, file_count, total_size)) {
        return false;
    }

    // Phase 2: Create a uniquely-named temp directory for extraction.
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

    // Phase 3: Extract with tar.
    // POSIX: strip ownership/permissions with flags supported by both GNU tar
    // and BSD tar. Extraction always targets a new, empty temp directory, so
    // GNU tar's non-portable --no-overwrite-dir is unnecessary.
    // Windows: plain extraction (Windows tar.exe doesn't support those flags).
    std::vector<std::string> cmd = {
        "tar", "xf", archive.string(),
        "-C", tmp_dir.string()
    };
#ifndef _WIN32
    cmd.push_back("--no-same-owner");
    cmd.push_back("--no-same-permissions");
#endif
    auto result = run_process(cmd, Path(), true);
    if (!result.success()) {
        std::println(std::cerr, "bake: failed to extract {}", archive.string());
        tmp_dir.remove_all();
        return false;
    }

    // Phase 4: Post-extraction validation (defense in depth).
    // The pre-scan should have caught everything, but verify the actual
    // extracted tree as well — in case tar's behavior differs from -tf.
    if (!validate_extracted_tree(tmp_dir.fs())) {
        tmp_dir.remove_all();
        return false;
    }

    // Find the top-level directory (tarballs usually have one prefix dir).
    std::string prefix_dir;
    int top_count = 0;
    for (auto& entry : std::filesystem::directory_iterator(tmp_dir.fs())) {
        top_count++;
        if (top_count == 1 && entry.is_directory()) {
            prefix_dir = entry.path().filename().string();
        }
    }

    if (top_count == 1 && !prefix_dir.empty()) {
        Path prefix_path = tmp_dir / prefix_dir;
        dest_dir.remove_all();
        dest_dir.parent().mkdir_recursive();
        std::filesystem::rename(prefix_path.fs(), dest_dir.fs());
        tmp_dir.remove_all();
    } else {
        dest_dir.remove_all();
        dest_dir.parent().mkdir_recursive();
        std::filesystem::rename(tmp_dir.fs(), dest_dir.fs());
    }

    return true;
}

inline std::string Resolver::compute_tree_hash(const Path& dir) {
    return compute_tree_sha256(dir);
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

    std::println(std::cerr, "bake: resolving {} ({})...", dep.name, dep.tag);

    // Step 1: tag → commit
    auto commit = resolve_tag(dep.url, dep.tag);
    if (!commit) {
        std::println(std::cerr, "bake: failed to resolve tag '{}' for {}",
                     dep.tag, dep.url);
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

inline bool Resolver::redownload(const Lockfile& lockfile, const ResolverConfig& config) {
    if (config.offline || config.frozen) {
        std::println(std::cerr, "bake: cannot re-download in offline/frozen mode");
        return false;
    }

    cache_dir_.mkdir_recursive();

    for (auto& [id, node] : lockfile.nodes) {
        if (node.commit.empty() || node.tree_sha256.empty()) continue;

        Path cached = cache_dir_ / node.tree_sha256;
        if (cached.is_directory()) continue;  // already cached

        std::println(std::cerr, "bake: re-downloading '{}' (commit {})...",
                     id, node.commit.substr(0, 12));

        // Download by locked commit (no tag re-resolution)
        Path download_dir = cache_dir_ / ".downloads";
        auto archive = download_archive(node.url, node.commit, download_dir);
        if (!archive) {
            std::println(std::cerr, "bake: download failed for '{}'", id);
            return false;
        }

        // Verify transport hash
        std::string transport_hash = file_hash(*archive);
        if (transport_hash != node.transport_sha256) {
            std::println(std::cerr, "bake: transport hash mismatch for '{}'", id);
            archive->remove();
            return false;
        }

        // Extract and verify tree hash
        Path extracted_dir = cache_dir_ / (".work-" + node.commit);
        if (!extract_archive(*archive, extracted_dir)) {
            archive->remove();
            return false;
        }

        std::string tree_hash = compute_tree_hash(extracted_dir);
        if (tree_hash != node.tree_sha256) {
            std::println(std::cerr, "bake: tree hash mismatch for '{}'", id);
            extracted_dir.remove_all();
            archive->remove();
            return false;
        }

        // Move to final cache location
        Path final_dir = cache_dir_ / tree_hash;
        final_dir.parent().mkdir_recursive();
        std::filesystem::rename(extracted_dir.fs(), final_dir.fs());
        archive->remove();
    }

    return true;
}

inline std::optional<Lockfile> Resolver::resolve(const Manifest& manifest, const ResolverConfig& config) {
    if (config.offline || config.frozen) {
        std::println(std::cerr, "bake: cannot resolve dependencies in offline/frozen mode");
        return std::nullopt;
    }

    cache_dir_.mkdir_recursive();

    Lockfile lockfile;

    // BFS queue: (dep_name, dep, is_root)
    struct QueueEntry {
        std::string name;
        Dependency dep;
        bool is_root;
    };
    std::vector<QueueEntry> queue;

    // Seed with root manifest dependencies
    for (auto& [name, dep] : manifest.dependencies) {
        if (dep.is_path_dep) continue;
        queue.push_back({name, dep, true});
    }

    // Conflict tracking
    std::map<std::string, std::string> url_to_commit;      // url → commit
    std::map<std::string, std::string> name_to_url;        // name → url
    std::map<std::string, std::string> url_commit_to_node; // "url\0commit" → node_id
    std::map<std::string, std::string> name_to_node_id;    // dep_name → actual node_id

    const size_t MAX_NODES = 256;
    size_t node_count = 0;

    while (!queue.empty()) {
        if (node_count >= MAX_NODES) {
            std::println(std::cerr, "bake: dependency graph exceeds limit ({} nodes)", MAX_NODES);
            return std::nullopt;
        }

        auto [dep_name, dep, is_root] = queue.front();
        queue.erase(queue.begin());

        // Check if this exact URL was already resolved (before downloading).
        // This prevents redundant downloads and handles aliases correctly.
        auto url_it = url_to_commit.find(dep.url);
        if (url_it != url_to_commit.end()) {
            const std::string& existing_commit = url_it->second;

            // We need to resolve the tag to know the commit.
            auto commit = resolve_tag(dep.url, dep.tag);
            if (!commit) {
                std::println(std::cerr, "bake: failed to resolve tag '{}' for {}",
                             dep.tag, dep.url);
                return std::nullopt;
            }

            if (*commit != existing_commit) {
                std::println(std::cerr,
                    "bake: conflict — {} resolved to commit {}, but same URL was already at {}",
                    dep.url, *commit, existing_commit);
                return std::nullopt;
            }

            // Same URL + same commit — alias the existing node.
            std::string key = dep.url + std::string(1, '\0') + existing_commit;
            auto alias_it = url_commit_to_node.find(key);
            if (alias_it != url_commit_to_node.end()) {
                if (is_root) {
                    lockfile.root_deps[dep_name] = alias_it->second;
                }
                // Register the name mapping
                name_to_url[dep_name] = dep.url;
                name_to_node_id[dep_name] = alias_it->second;
            }
            continue;
        }

        // Not a duplicate URL — do the full resolve.
        auto node = resolve_dependency(dep);
        if (!node) return std::nullopt;

        url_to_commit[dep.url] = node->commit;
        std::string uc_key = dep.url + std::string(1, '\0') + node->commit;
        url_commit_to_node[uc_key] = node->id;

        // Conflict check 2: same name from a different URL
        auto name_it = name_to_url.find(dep_name);
        if (name_it != name_to_url.end()) {
            if (name_it->second != dep.url) {
                std::println(std::cerr,
                    "bake: conflict — dependency '{}' from {}, but already resolved from {}",
                    dep_name, dep.url, name_it->second);
                return std::nullopt;
            }
        } else {
            name_to_url[dep_name] = dep.url;
        }

        // Add to lockfile
        lockfile.nodes[node->id] = *node;
        name_to_node_id[dep_name] = node->id;
        if (is_root) {
            lockfile.root_deps[dep_name] = node->id;
        }
        node_count++;

        // Recursive: if bake-native, load its bake.toml and enqueue its deps
        if (node->native && !node->tree_sha256.empty()) {
            Path dep_cache = cache_dir_ / node->tree_sha256;
            Path dep_toml = dep_cache / "bake.toml";
            if (dep_toml.is_regular_file()) {
                auto sub_manifest = Manifest::load(dep_cache);
                if (sub_manifest) {
                    for (auto& [sub_name, sub_dep] : sub_manifest->dependencies) {
                        if (sub_dep.is_path_dep) continue;
                        // Push the sub_name as a placeholder; it will be
                        // resolved to the actual node_id in post-processing.
                        node->dependencies.push_back(sub_name);
                        queue.push_back({sub_name, sub_dep, false});
                    }
                    // Update the node in lockfile with its dependency edges
                    lockfile.nodes[node->id].dependencies = node->dependencies;
                }
            }
        }
    }

    // Post-processing: fix up dependency edges.
    // Parent nodes store sub_name as their dependency edge, but the lockfile
    // needs actual node IDs. Replace each placeholder with the real node ID.
    for (auto& [id, node] : lockfile.nodes) {
        for (auto& child_id : node.dependencies) {
            if (lockfile.nodes.count(child_id)) continue;
            auto nit = name_to_node_id.find(child_id);
            if (nit != name_to_node_id.end()) {
                child_id = nit->second;
            }
        }
    }

    return lockfile;
}

// Resolve a single dep + its transitive closure into a Lockfile.
inline std::optional<Lockfile> Resolver::resolve_subtree(const Dependency& root_dep,
                                                          const std::string& root_name) {
    cache_dir_.mkdir_recursive();

    Lockfile lockfile;

    struct QueueEntry {
        std::string name;
        Dependency dep;
        bool is_root;
    };
    std::vector<QueueEntry> queue;
    queue.push_back({root_name, root_dep, true});

    std::map<std::string, std::string> url_to_commit;
    std::map<std::string, std::string> url_commit_to_node;
    std::map<std::string, std::string> name_to_node_id;

    const size_t MAX_NODES = 256;
    size_t node_count = 0;

    while (!queue.empty()) {
        if (node_count >= MAX_NODES) {
            std::println(std::cerr, "bake: dependency graph exceeds limit ({} nodes)", MAX_NODES);
            return std::nullopt;
        }

        auto [dep_name, dep, is_root] = queue.front();
        queue.erase(queue.begin());

        auto url_it = url_to_commit.find(dep.url);
        if (url_it != url_to_commit.end()) {
            auto commit = resolve_tag(dep.url, dep.tag);
            if (!commit || *commit != url_it->second) {
                if (commit && *commit != url_it->second) {
                    std::println(std::cerr, "bake: conflict — same URL resolved to different commits");
                    return std::nullopt;
                }
                return std::nullopt;
            }
            std::string key = dep.url + std::string(1, '\0') + url_it->second;
            auto alias_it = url_commit_to_node.find(key);
            if (alias_it != url_commit_to_node.end()) {
                name_to_node_id[dep_name] = alias_it->second;
                if (is_root) {
                    lockfile.root_deps[dep_name] = alias_it->second;
                }
            }
            continue;
        }

        auto node = resolve_dependency(dep);
        if (!node) return std::nullopt;

        url_to_commit[dep.url] = node->commit;
        std::string uc_key = dep.url + std::string(1, '\0') + node->commit;
        url_commit_to_node[uc_key] = node->id;

        lockfile.nodes[node->id] = *node;
        name_to_node_id[dep_name] = node->id;
        if (is_root) {
            lockfile.root_deps[dep_name] = node->id;
        }
        node_count++;

        if (node->native && !node->tree_sha256.empty()) {
            Path dep_cache = cache_dir_ / node->tree_sha256;
            Path dep_toml = dep_cache / "bake.toml";
            if (dep_toml.is_regular_file()) {
                auto sub_manifest = Manifest::load(dep_cache);
                if (sub_manifest) {
                    for (auto& [sub_name, sub_dep] : sub_manifest->dependencies) {
                        if (sub_dep.is_path_dep) continue;
                        node->dependencies.push_back(sub_name);
                        queue.push_back({sub_name, sub_dep, false});
                    }
                    lockfile.nodes[node->id].dependencies = node->dependencies;
                }
            }
        }
    }

    // Post-processing: fix up dependency edges in resolve_subtree
    for (auto& [id, node] : lockfile.nodes) {
        for (auto& child_id : node.dependencies) {
            if (lockfile.nodes.count(child_id)) continue;
            auto nit = name_to_node_id.find(child_id);
            if (nit != name_to_node_id.end()) {
                child_id = nit->second;
            }
        }
    }

    return lockfile;
}

} // namespace bake
