export module bake.buildsystem.package;

import std;
import bake.util;
import bake.buildsystem.project;
import bake.json;
namespace json = bake::json;

// ============================================================
// bake.buildsystem.package — Resolver, Fetcher, Cache
// ============================================================

namespace bake {

// ===== Lockfile (JSON, flat identity-keyed entries) =====
//
// Format:
// {
//   "deps": {
//     "git:<url>@<commit>":     { "url": "...", "ref": "...", "ref_type": "tag",
//                                 "commit": "...", "integrity": "sha256-...",
//                                 "name": "fmt" },
//     "archive:<url>":          { "url": "...", "ref_type": "archive",
//                                 "integrity": "sha256-..." }
//   }
// }
//
// Identity rules (lock key): uniform locator identity —
//   git dep:     "git:<normalized-url>@<commit>"
//   archive dep: "archive:<normalized-url>"
// "name" is a display annotation ([package].name of native sources), not
// part of identity. Path dependencies are manifest-only and never locked.

export struct LockDep {
    std::string key;          // identity key
    std::string name;         // display annotation ([package].name, native only)
    std::string url;          // as declared
    std::string ref;          // ref value (tag/branch/commit; "" = default branch)
    std::string ref_type;     // "tag" | "branch" | "rev" | "head" | "archive"
    std::string commit;       // resolved commit (git deps only)
    std::string integrity;    // "sha256-<tree-hash>"

    bool is_remote() const { return !url.empty(); }
    bool is_archive() const { return ref_type == "archive"; }

    // Extract the bare hex hash from "sha256-<hex>".
    std::string cache_hash() const {
        if (integrity.empty()) return {};
        auto pos = integrity.find('-');
        return pos == std::string::npos ? integrity : integrity.substr(pos + 1);
    }
};

export struct Lockfile {
    std::map<std::string, LockDep> deps;   // identity key → dep

    static std::optional<Lockfile> load(const Path& path) {
        if (!path.is_regular_file()) return std::nullopt;

        auto content = read_file(path);
        if (!content) return std::nullopt;

        Lockfile lf;

        json::Value doc;
        try {
            doc = json::Value::parse(*content);
        } catch (...) {
            return std::nullopt;
        }

        if (!doc.is_object()) return std::nullopt;
        const json::Value* deps_it = doc.find("deps");
        if (!deps_it || !deps_it->is_object()) return std::nullopt;

        for (const auto& item : deps_it->items()) {
            const auto& val = item.value();
            if (!val.is_object()) continue;
            LockDep dep;
            dep.key = item.key();
            if (auto v = val.value("url", ""); !v.empty()) dep.url = v;
            if (auto v = val.value("ref", ""); !v.empty()) dep.ref = v;
            if (auto v = val.value("ref_type", ""); !v.empty()) dep.ref_type = v;
            if (auto v = val.value("commit", ""); !v.empty()) dep.commit = v;
            if (auto v = val.value("integrity", ""); !v.empty()) dep.integrity = v;
            if (auto v = val.value("name", ""); !v.empty()) dep.name = v;
            // Entries without a URL (pre-rename path entries) are dropped;
            // the next resolve rebuilds the lock without them.
            if (!dep.is_remote()) continue;
            lf.deps[dep.key] = std::move(dep);
        }

        return lf;
    }

    bool save(const Path& path) const {
        json::Value doc = json::Value::object();
        json::Value deps_obj = json::Value::object();

        for (auto& [key, dep] : deps) {
            json::Value entry = json::Value::object();
            entry["url"] = dep.url;
            if (dep.is_archive()) {
                entry["ref_type"] = "archive";
                entry["integrity"] = dep.integrity;
            } else {
                entry["ref"] = dep.ref;
                entry["ref_type"] = dep.ref_type;
                entry["commit"] = dep.commit;
                entry["integrity"] = dep.integrity;
            }
            if (!dep.name.empty()) entry["name"] = dep.name;
            deps_obj[key] = std::move(entry);
        }

        doc["deps"] = std::move(deps_obj);

        std::string content = doc.dump(2) + "\n";
        return atomic_write_file(path, content);
    }

    // Find a locked remote dep by normalized URL + ref identity
    // (ref_type + ref value; archives match on URL alone).
    const LockDep* find_remote(const std::string& dep_url,
                               const std::string& ref_type,
                               const std::string& ref) const {
        std::string norm = normalize_dependency_url(dep_url);
        for (auto& [key, dep] : deps) {
            if (!dep.is_remote()) continue;
            if (normalize_dependency_url(dep.url) == norm &&
                dep.ref_type == ref_type && dep.ref == ref) {
                return &dep;
            }
        }
        return nullptr;
    }

    // Recursively check staleness across every declared scope (global and
    // all [target.*] tables): every remote dep in the closure must have a
    // matching lock entry with a resolved commit (git) and integrity.
    // Orphan entries are not staleness — the next resolve rebuilds the lock
    // from the closure and prunes them (remove also prunes explicitly).
    bool is_consistent(const Manifest& manifest, const Path& root) const {
        std::set<Path> visited;
        return check_consistency(manifest, root, visited);
    }

  private:
    bool check_consistency(const Manifest& manifest, const Path& root,
                           std::set<Path>& visited) const {
        std::vector<const std::map<std::string, Dependency>*> scopes;
        scopes.push_back(&manifest.dependencies);
        for (const auto& condition : manifest.targets)
            scopes.push_back(&condition.dependencies);

        for (auto* scope : scopes) {
            for (auto& [alias, dep] : *scope) {
                (void)alias;
                if (dep.is_path_dep) {
                    // Recurse into path dep manifests
                    Path dep_dir = (manifest.project_dir / dep.path.c_str())
                                       .lexically_normal();
                    Path dep_toml = dep_dir / "bake.toml";
                    if (!dep_toml.is_regular_file()) continue;
                    auto canonical = dep_dir.absolute();
                    if (visited.insert(canonical).second) {
                        auto sub = Manifest::load(dep_dir);
                        if (sub && sub->has_moid()) {
                            sub->project_dir = dep_dir;
                            if (!check_consistency(*sub, root, visited))
                                return false;
                        }
                    }
                    continue;
                }

                // Remote dep: must have a lock entry with full identity.
                std::string ref_type = dep.is_archive() ? "archive" : "";
                std::string ref;
                if (ref_type.empty()) {
                    auto pair = dep.git_ref();
                    ref_type = pair.first;
                    ref = pair.second;
                }
                const LockDep* locked = find_remote(dep.url, ref_type, ref);
                if (!locked) return false;
                if (locked->integrity.empty()) return false;
                if (!dep.is_archive() && locked->commit.empty()) return false;
            }
        }
        return true;
    }


};

// Get the global source cache directory
export Path get_cache_dir() {
#ifdef _WIN32
    const char* home = std::getenv("LOCALAPPDATA");
    if (!home) home = "C:\\";
    return Path(home) / "bake" / "src";
#else
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return Path(home) / ".cache" / "bake" / "src";
#endif
}

// ===== Resolver configuration =====

export struct ResolverConfig {
    bool offline = false;
    bool frozen = false;
};

// ===== Resolver =====

export class Resolver {
public:
    Resolver(Path cache_dir = get_cache_dir()) : cache_dir_(std::move(cache_dir)) {}

    // Resolve the remote dependency closure from every declared scope of
    // the manifest (global + all [target.*] tables). Existing lock entries
    // matching a declared url+ref are carried over verbatim — the network
    // is only touched for new or changed dependencies. Returns a fresh
    // lockfile covering exactly the closure (orphan entries pruned).
    std::optional<Lockfile> resolve(const Manifest& manifest,
                                    const ResolverConfig& config,
                                    const Lockfile* hints = nullptr,
                                    const std::set<std::string>&
                                        root_features = {});

    // Resolve a single remote dependency (network): ref → commit →
    // download → extract → tree hash. Returns the lock entry.
    std::optional<LockDep> resolve_dependency(const Dependency& dep);

    // Re-download cached sources for all lock entries using existing locked
    // commits/URLs. Does NOT re-resolve refs. Used when cache is
    // missing/corrupted but lock is valid.
    bool redownload(const Lockfile& lockfile, const ResolverConfig& config);

private:
    Path cache_dir_;

    // ref → commit via git ls-remote. "rev" passes through verbatim,
    // "head" resolves the remote's HEAD.
    std::optional<std::string> resolve_ref(const std::string& url,
                                           const std::string& ref_type,
                                           const std::string& ref);

    // Download an archive URL verbatim to dest_dir/<file_name>.
    std::optional<Path> download_archive(const std::string& url,
                                         const std::string& file_name,
                                         const Path& dest_dir);

    // Compute SHA-256 of a file
    std::string file_hash(const Path& file);

    // Safely extract a tarball/zip to a directory (strips top-level prefix)
    bool extract_archive(const Path& archive, const Path& dest_dir);

    // Compute tree SHA-256 (normalized hash of all files in directory)
    std::string compute_tree_hash(const Path& dir);

    // Obtain a remote dependency's source tree in a fresh work directory
    // under the cache. Archives download verbatim; git deps try the host's
    // tarball endpoint (GitHub/GitLab style) first and fall back to a full
    // `git clone` + checkout when the host has none. Returns the extracted
    // directory; the caller moves it into the content cache.
    std::optional<Path> fetch_sources(const std::string& url, bool is_archive,
                                      const std::string& commit);

    // Clone url and check out an exact commit into dest_dir. .git is
    // transport detail — the cache holds bare sources, and dropping it
    // keeps the tree identical to the tarball transport.
    bool fetch_via_clone(const std::string& url, const std::string& commit,
                         const Path& dest_dir);
};

// Tree hash of a source directory, used by the CLI layer's cache
// verification. (engine intentionally never imports this module — the
// build side consumes remote sources through the SourceIndex injected by
// the composition root, never through the package domain.)
export inline std::string compute_tree_sha256(const Path& dir) {
    std::vector<std::pair<std::string, std::string>> entries;

    for (auto& entry : std::filesystem::recursive_directory_iterator(dir.fs())) {
        // Skip bake-internal sentinel files — they are not part of the source.
        if (entry.path().filename() == ".bake-verified") continue;

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

// Extract a short display name from an identity key.
//   "brotli"                               → "brotli"
//   "git:https://github.com/Mbed-TLS/mbedtls@abc123" → "mbedtls"
inline std::string display_name_for_key(const std::string& key) {
    if (!key.starts_with("git:")) return key;
    // Strip "git:" prefix, take URL up to '@', extract last path segment.
    std::string url = key.substr(4);
    auto at = url.find('@');
    if (at != std::string::npos) url = url.substr(0, at);
    // Remove trailing slash.
    while (!url.empty() && url.back() == '/') url.pop_back();
    auto slash = url.find_last_of('/');
    if (slash != std::string::npos) return url.substr(slash + 1);
    return url;
}

// Extract repo name from a URL (last path segment, no extension).
//   "https://github.com/curl/curl" → "curl"
inline std::string repo_name_from_url(const std::string& url) {
    std::string u = url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    auto slash = u.find_last_of('/');
    std::string name = (slash != std::string::npos) ? u.substr(slash + 1) : u;
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

// Verify that cached source for every remote lock entry matches its integrity.
// Detects tampering with cached dependencies. Called by the CLI layer.
// Silent on cache miss (caller handles messaging); prints only on tamper.
//
// A `.bake-verified` sentinel is written inside each cache dir after a
// successful full-tree verification. Subsequent calls compare the sentinel
// to the expected hash and skip the expensive tree walk when they match.
export inline bool verify_lock_cache(const Lockfile& lockfile, const Path& cache_dir) {
    for (auto& [key, dep] : lockfile.deps) {
        if (!dep.is_remote()) continue;
        std::string hash = dep.cache_hash();
        if (hash.empty()) continue;

        Path cache_path = cache_dir / hash;
        if (!cache_path.is_directory()) return false;

        // Fast path: sentinel exists and matches expected hash.
        Path sentinel = cache_path / ".bake-verified";
        if (sentinel.is_regular_file()) {
            if (auto content = read_file(sentinel)) {
                if (*content == hash) continue;
            }
        }

        // Slow path: full tree SHA-256.
        std::string actual_hash = compute_tree_sha256(cache_path);
        if (actual_hash != hash) {
            std::println(std::cerr,
                "bake: cache tampered for '{}' (expected {}, got {})",
                display_name_for_key(key), hash.substr(0, 12),
                actual_hash.substr(0, 12));
            return false;
        }

        write_file(sentinel, hash);
    }
    return true;
}

// ===== Implementation =====

inline std::optional<std::string> Resolver::resolve_ref(
        const std::string& url, const std::string& ref_type,
        const std::string& ref) {
    // A pinned commit resolves to itself.
    if (ref_type == "rev") return ref;

    // Build the ls-remote refspecs:
    //   tag    → the tag ref and its peeled form (^{} gives the commit for
    //            annotated tags; lightweight tags only have the plain ref)
    //   branch → the branch ref
    //   head   → the remote's default branch
    std::vector<std::string> cmd = {"git", "ls-remote", url};
    if (ref_type == "tag") {
        cmd.push_back("refs/tags/" + ref);
        cmd.push_back("refs/tags/" + ref + "^{}");
    } else if (ref_type == "branch") {
        cmd.push_back("refs/heads/" + ref);
    } else {
        cmd.push_back("HEAD");
    }

    auto result = run_process(cmd, Path(), true);
    if (!result.success()) return std::nullopt;

    // Prefer the peeled (^{}) entry; fall back to the first plain ref.
    std::string commit;
    std::string fallback_commit;

    auto& out = result.stdout_output;
    std::size_t pos = 0;
    while (pos < out.size()) {
        std::size_t tab = out.find('\t', pos);
        if (tab == std::string::npos) break;
        std::size_t eol = out.find('\n', tab);
        if (eol == std::string::npos) eol = out.size();

        std::string sha = out.substr(pos, tab - pos);
        std::string found_ref = out.substr(tab + 1, eol - tab - 1);

        // Trim whitespace
        while (!sha.empty() &&
               (sha.back() == '\n' || sha.back() == '\r' || sha.back() == ' '))
            sha.pop_back();

        if (found_ref.find("^{}") != std::string::npos) {
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
        std::size_t last_slash = base.find_last_of('/');
        std::string repo = (last_slash != std::string::npos)
                           ? base.substr(last_slash + 1) : "repo";
        return base + "/-/archive/" + commit + "/" + repo + ".tar.gz";
    }

    // GitHub and GitHub-compatible hosts (default):
    // <base>/archive/<commit>.tar.gz
    return base + "/archive/" + commit + ".tar.gz";
}

inline std::optional<Path> Resolver::download_archive(const std::string& url,
                                                       const std::string& file_name,
                                                       const Path& dest_dir) {
    dest_dir.mkdir_recursive();

    Path archive_path = dest_dir / file_name;

    std::vector<std::string> cmd = {"curl", "-sL", "--fail", "-o",
                                    archive_path.string(), url};
    auto result = run_process(cmd, Path(), true);
    if (!result.success()) {
        std::println(std::cerr, "bake: failed to download {}", url);
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

    const std::size_t MAX_FILES = 100'000;
    const std::uintmax_t MAX_TOTAL_SIZE = 1ULL * 1024 * 1024 * 1024;  // 1 GB

    std::size_t file_count = 0;
    std::uintmax_t total_size = 0;

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
inline bool prescan_archive(const Path& archive, std::size_t& out_file_count,
                            std::uintmax_t& out_total_size) {
    const std::size_t MAX_FILES = 100'000;
    const std::uintmax_t MAX_TOTAL_SIZE = 1ULL * 1024 * 1024 * 1024;  // 1 GB

    // List all entries with metadata using `tar -tvf` (verbose).
    // This gives us type, size, and symlink target for each entry.
    auto result = run_process(
        {"tar", "-tvf", archive.string()}, Path(), true);
    if (!result.success()) {
        std::println(std::cerr, "bake: failed to list archive entries");
        return false;
    }

    std::size_t file_count = 0;
    std::uintmax_t total_size = 0;

    // Parse the verbose listing line by line.
    // Format example (BSD tar):
    //   -rw-r--r--  0 user group     1234 Jan  1 12:00 prefix/file.txt
    //   lrwxrwxrwx  0 user group        8 Jan  1 12:00 prefix/link -> target
    const auto& listing = result.stdout_output;
    std::size_t pos = 0;
    while (pos < listing.size()) {
        std::size_t eol = listing.find('\n', pos);
        if (eol == std::string::npos) eol = listing.size();

        std::string line = listing.substr(pos, eol - pos);
        pos = eol + 1;

        if (line.empty()) continue;

        char type_char = line[0];

        // Find path: after timestamp pattern "HH:MM " in the line
        std::size_t time_end = std::string::npos;
        for (std::size_t i = 10; i + 1 < line.size(); ++i) {
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
            std::size_t arrow = line.find(" -> ", time_end);
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
            std::size_t dotdot = 0;
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
            std::size_t dd = 0;
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
                std::uintmax_t fsize = 0;
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

// Pre-scan a zip archive's entry list via `unzip -l` BEFORE extracting.
// Same protections as the tar pre-scan: absolute paths, path traversal,
// and count/size limits (zip bombs). Zip listings do not expose symlink
// targets or modes — symlink escapes are caught by the post-extraction
// tree validation.
inline bool prescan_zip(const Path& archive, std::size_t& out_file_count,
                        std::uintmax_t& out_total_size) {
    const std::size_t MAX_FILES = 100'000;
    const std::uintmax_t MAX_TOTAL_SIZE = 1ULL * 1024 * 1024 * 1024;  // 1 GB

    auto result = run_process(
        {"unzip", "-l", archive.string()}, Path(), true);
    if (!result.success()) {
        std::println(std::cerr, "bake: failed to list zip entries");
        return false;
    }

    std::size_t file_count = 0;
    std::uintmax_t total_size = 0;

    // Entry lines look like:
    //       123  2024-01-01 12:00   path/name
    // Header/dashed/summary lines fail the numeric-length parse and are
    // skipped ("Length ..." header, "----" separators, "N files" summary).
    const auto& listing = result.stdout_output;
    std::size_t pos = 0;
    while (pos < listing.size()) {
        std::size_t eol = listing.find('\n', pos);
        if (eol == std::string::npos) eol = listing.size();
        std::string line = listing.substr(pos, eol - pos);
        pos = eol + 1;

        std::istringstream iss(line);
        std::uintmax_t length = 0;
        std::string date, time, name;
        if (!(iss >> length >> date >> time)) continue;
        std::getline(iss, name);
        std::size_t first = name.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        name = name.substr(first);
        for (auto& c : name) {
            if (c == '\\') c = '/';
        }
        while (!name.empty() && (name.back() == '\r' || name.back() == ' '))
            name.pop_back();
        if (name.empty()) continue;

        if (name.front() == '/') {
            std::println(std::cerr,
                "bake: rejecting zip entry with absolute path: {}", name);
            return false;
        }
        std::size_t dotdot = 0;
        while ((dotdot = name.find("..", dotdot)) != std::string::npos) {
            bool left_ok = (dotdot == 0 || name[dotdot - 1] == '/');
            bool right_ok = (dotdot + 2 >= name.size() || name[dotdot + 2] == '/');
            if (left_ok && right_ok) {
                std::println(std::cerr,
                    "bake: rejecting path traversal in zip: {}", name);
                return false;
            }
            dotdot += 2;
        }

        file_count++;
        total_size += length;

        if (file_count > MAX_FILES) {
            std::println(std::cerr,
                "bake: zip exceeds file count limit ({} entries)", MAX_FILES);
            return false;
        }
        if (total_size > MAX_TOTAL_SIZE) {
            std::println(std::cerr, "bake: zip exceeds size limit (1 GB)");
            return false;
        }
    }

    out_file_count = file_count;
    out_total_size = total_size;
    return true;
}

inline bool Resolver::extract_archive(const Path& archive, const Path& dest_dir) {
    // Pre-scan archive entries BEFORE extraction.
    // This prevents malicious archives from writing to the filesystem
    // before safety checks can run.
    const bool is_zip = ends_with(archive.string(), ".zip");
    std::size_t file_count = 0;
    std::uintmax_t total_size = 0;
    if (is_zip ? !prescan_zip(archive, file_count, total_size)
               : !prescan_archive(archive, file_count, total_size)) {
        return false;
    }

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

    // Extract. Zip archives go through unzip; everything else through tar.
    //
    // tar: POSIX strip ownership/permissions with flags supported by both
    // GNU tar and BSD tar. Extraction always targets a new, empty temp
    // directory, so GNU tar's non-portable --no-overwrite-dir is
    // unnecessary. Windows: plain extraction (Windows tar.exe doesn't
    // support those flags).
    std::vector<std::string> cmd;
    if (is_zip) {
        cmd = {"unzip", "-q", archive.string(), "-d", tmp_dir.string()};
    } else {
        cmd = {"tar", "xf", archive.string(), "-C", tmp_dir.string()};
#ifndef _WIN32
        cmd.push_back("--no-same-owner");
        cmd.push_back("--no-same-permissions");
#endif
    }
    auto result = run_process(cmd, Path(), true);
    if (!result.success()) {
        std::println(std::cerr, "bake: failed to extract {}", archive.string());
        tmp_dir.remove_all();
        return false;
    }

    // Post-extraction validation (defense in depth).
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

inline bool Resolver::fetch_via_clone(const std::string& url,
                                      const std::string& commit,
                                      const Path& dest_dir) {
    dest_dir.remove_all();

    std::vector<std::string> clone = {"git", "clone", "--quiet", "--no-checkout",
                                      url, dest_dir.string()};
    if (!run_process(clone, Path(), true).success()) {
        std::println(std::cerr, "bake: failed to clone {}", url);
        dest_dir.remove_all();
        return false;
    }

    auto checkout = [&]() {
        return run_process({"git", "-C", dest_dir.string(), "checkout",
                            "--quiet", "--detach", commit},
                           Path(), true)
            .success();
    };

    // The commit is normally reachable from a branch or tag in a standard
    // clone. If it is not (deleted branch, uploaded-only object), ask the
    // server for the SHA directly before giving up.
    if (!checkout()) {
        run_process({"git", "-C", dest_dir.string(), "fetch", "--quiet",
                     "origin", commit},
                    Path(), true);
        if (!checkout()) {
            std::println(std::cerr, "bake: commit {} not found in {}",
                         commit, url);
            dest_dir.remove_all();
            return false;
        }
    }

    (dest_dir / ".git").remove_all();
    return true;
}

inline std::optional<Path> Resolver::fetch_sources(const std::string& url,
                                                   bool is_archive,
                                                   const std::string& commit) {
    std::println("  Downloading {}",
                 is_archive ? url.substr(url.find_last_of('/') + 1)
                            : repo_name_from_url(url));

    Path download_dir = cache_dir_ / ".downloads";
    std::string download_url = url;
    std::string file_name = SHA256::hex(url).substr(0, 24) +
                            archive_extension(url);
    if (!is_archive) {
        download_url = build_archive_url(url, commit);
        file_name = commit + ".tar.gz";
    }

    if (auto archive = download_archive(download_url, file_name, download_dir)) {
        std::string transport_hash = file_hash(*archive);
        Path extracted = cache_dir_ / (".work-" + transport_hash.substr(0, 12));
        bool ok = extract_archive(*archive, extracted);
        archive->remove();
        if (ok) return extracted;
    }

    // Archives have no clone transport. Git deps fall back to a full clone
    // when the host exposes no tarball endpoint.
    if (is_archive) return std::nullopt;

    std::println("  No tarball endpoint for {}; cloning", repo_name_from_url(url));
    Path clone_dir = cache_dir_ / (".clone-" + commit.substr(0, 12));
    if (!fetch_via_clone(url, commit, clone_dir)) return std::nullopt;
    return clone_dir;
}


inline std::optional<LockDep> Resolver::resolve_dependency(const Dependency& dep) {
    if (!dep.is_remote()) return std::nullopt;  // path deps are manifest-only

    const bool is_archive = dep.is_archive();
    std::string ref_type = is_archive ? "archive" : "";
    std::string ref;
    std::string commit;

    if (!is_archive) {
        auto pair = dep.git_ref();
        ref_type = pair.first;
        ref = pair.second;

        if (ref_type == "rev") {
            commit = ref;  // pinned commit resolves to itself
        } else {
            std::println("  Resolving {} of {}",
                         ref_type == "head" ? "default branch" : ref_type + " '" + ref + "'",
                         repo_name_from_url(dep.url));
            auto resolved = resolve_ref(dep.url, ref_type, ref);
            if (!resolved) {
                std::println(std::cerr,
                    "bake: failed to resolve {} '{}' for {}",
                    ref_type, ref, dep.url);
                return std::nullopt;
            }
            commit = *resolved;
        }
    }

    // Download, extract, tree-hash, move into the content cache.
    auto extracted = fetch_sources(dep.url, is_archive, commit);
    if (!extracted) return std::nullopt;

    std::string tree_hash = compute_tree_hash(*extracted);

    Path final_dir = cache_dir_ / tree_hash;
    if (!final_dir.exists()) {
        final_dir.parent().mkdir_recursive();
        std::filesystem::rename(extracted->fs(), final_dir.fs());
    } else {
        extracted->remove_all();
    }

    // Native sources carry their moid name as the lock annotation.
    std::string name;
    if (auto native = Manifest::load_moid(final_dir))
        name = native->moid->name;

    LockDep node;
    node.url = dep.url;
    node.ref = is_archive ? "" : ref;
    node.ref_type = ref_type;
    node.commit = is_archive ? "" : commit;
    node.integrity = "sha256-" + tree_hash;
    node.name = std::move(name);
    node.key = is_archive
        ? "archive:" + normalize_dependency_url(dep.url)
        : "git:" + normalize_dependency_url(dep.url) + "@" + commit;

    return node;
}

inline bool Resolver::redownload(const Lockfile& lockfile, const ResolverConfig& config) {
    if (config.offline || config.frozen) {
        std::println(std::cerr, "bake: cannot re-download in offline/frozen mode");
        return false;
    }

    cache_dir_.mkdir_recursive();

    for (auto& [key, dep] : lockfile.deps) {
        if (!dep.is_remote()) continue;

        std::string hash = dep.cache_hash();
        if (hash.empty()) continue;

        Path cached = cache_dir_ / hash;
        if (cached.is_directory()) continue;  // already cached

        // A git entry without a resolved commit cannot be fetched by
        // identity; the next resolve re-locks it.
        if (!dep.is_archive() && dep.commit.empty()) continue;

        // Download by locked identity (no ref re-resolution)
        auto extracted = fetch_sources(dep.url, dep.is_archive(), dep.commit);
        if (!extracted) {
            std::println(std::cerr, "bake: download failed for '{}'", key);
            return false;
        }

        // Verify the tree hash against the lock
        std::string tree_hash = compute_tree_hash(*extracted);
        if (tree_hash != hash) {
            std::println(std::cerr, "bake: tree hash mismatch for '{}'", key);
            extracted->remove_all();
            return false;
        }

        // Move to final cache location
        Path final_dir = cache_dir_ / tree_hash;
        final_dir.parent().mkdir_recursive();
        std::filesystem::rename(extracted->fs(), final_dir.fs());
    }

    return true;
}
inline std::optional<Lockfile> Resolver::resolve(const Manifest& manifest,
                                                 const ResolverConfig& config,
                                                 const Lockfile* hints,
                                                 const std::set<std::string>&
                                                     root_features) {
    if (config.offline || config.frozen) {
        std::println(std::cerr, "bake: cannot resolve dependencies in offline/frozen mode");
        return std::nullopt;
    }

    cache_dir_.mkdir_recursive();

    Lockfile lockfile;

    // BFS queue: (dep, parent manifest dir, features the consumer
    // activated on this dependency).
    //
    // The lock carries the union over every target and every reachable
    // feature activation: default features always, explicit activations
    // arriving on dependency edges. Conflicts are not checked here —
    // they are a per-build-target concern validated during graph
    // resolution.
    struct QueueEntry {
        Dependency dep;
        Path manifest_dir;
        std::set<std::string> features;
    };
    std::vector<QueueEntry> queue;

    // Accumulated activation per package identity (dedup key / path).
    std::map<std::string, std::set<std::string>> activated_by_key;
    std::map<Path, std::set<std::string>> activated_by_path;

    auto active_features_of = [&](const Manifest& m,
                                  const std::set<std::string>& extra)
            -> std::optional<std::set<std::string>> {
        for (const auto& name : extra) {
            if (!m.features.count(name)) {
                std::println(std::cerr,
                             "bake: feature '{}' is not declared by "
                             "package '{}'",
                             name,
                             m.moid ? m.moid->name : std::string("<unknown>"));
                return std::nullopt;
            }
        }
        // Explicit activation demotes conflicting defaults so their
        // dependencies are neither resolved nor locked (mirrors graph
        // resolution — see demote_conflicting_defaults).
        return demote_conflicting_defaults(m, extra);
    };

    auto enqueue_scopes = [&](const Manifest& m,
                              const std::set<std::string>& active) {
        std::vector<const std::map<std::string, Dependency>*> scopes;
        scopes.push_back(&m.dependencies);
        for (const auto& condition : m.targets)
            scopes.push_back(&condition.dependencies);
        for (const auto& name : active)
            scopes.push_back(&m.features.at(name).dependencies);
        for (auto* scope : scopes)
            for (auto& [name, dep] : *scope)
                queue.push_back({dep, m.project_dir,
                                 {dep.features.begin(), dep.features.end()}});
    };
    {
        // CLI root features participate in lock resolution: their
        // conditional dependencies must be resolved and locked.
        auto root_active =
            active_features_of(manifest, root_features);
        if (!root_active) return std::nullopt;
        enqueue_scopes(manifest, *root_active);

        // Workspace members are first-class dependency scopes: their
        // declarations resolve and lock exactly like the root's.
        if (manifest.is_workspace()) {
            for (const auto& member :
                 manifest.workspace->members) {
                Path member_dir =
                    (manifest.project_dir / member.c_str())
                        .lexically_normal();
                if (!(member_dir / "bake.toml").is_regular_file())
                    continue;
                auto sub = Manifest::load_moid(member_dir);
                if (!sub) continue;
                sub->project_dir = member_dir;
                auto member_active = active_features_of(*sub, {});
                if (!member_active) return std::nullopt;
                enqueue_scopes(*sub, *member_active);
            }
        }
    }

    // Dedupe by normalized url + ref identity within this run.
    std::set<std::string> seen_refs;
    auto dedup_key = [](const Dependency& dep) {
        if (dep.is_archive())
            return normalize_dependency_url(dep.url) + "\narchive\n";
        auto pair = dep.git_ref();
        return normalize_dependency_url(dep.url) + "\n" + pair.first + "\n" +
               pair.second;
    };

    const std::size_t MAX_ENTRIES = 256;
    std::size_t entry_count = 0;

    while (!queue.empty()) {
        if (entry_count >= MAX_ENTRIES) {
            std::println(std::cerr, "bake: dependency graph exceeds limit ({} entries)",
                         MAX_ENTRIES);
            return std::nullopt;
        }

        auto entry = queue.front();
        queue.erase(queue.begin());
        const Dependency& dep = entry.dep;

        if (dep.is_path_dep) {
            // Path deps are manifest-only (never locked); recurse into
            // native ones to reach their remote dependencies.
            Path dep_dir = (entry.manifest_dir / dep.path.c_str())
                               .lexically_normal();
            Path dep_toml = dep_dir / "bake.toml";
            if (dep_toml.is_regular_file()) {
                auto sub_manifest = Manifest::load_moid(dep_dir);
                if (sub_manifest) {
                    sub_manifest->project_dir = dep_dir;
                    auto& seen = activated_by_path[dep_dir.absolute()];
                    bool grew = false;
                    for (const auto& name : entry.features)
                        if (seen.insert(name).second) grew = true;
                    if (grew || seen.empty()) {
                        auto active =
                            active_features_of(*sub_manifest, seen);
                        if (!active) return std::nullopt;
                        enqueue_scopes(*sub_manifest, *active);
                    }
                }
            }
            continue;
        }

        // A first visit resolves over the network; a later visit only
        // matters when it activated additional features, in which case the
        // newly reachable feature dependencies are enqueued below.
        auto& accumulated = activated_by_key[dedup_key(dep)];
        bool grew = false;
        for (const auto& name : entry.features)
            if (accumulated.insert(name).second) grew = true;
        if (!seen_refs.insert(dedup_key(dep)).second && !grew) continue;

        std::string ref_type = dep.is_archive() ? "archive" : "";
        std::string ref;
        if (ref_type.empty()) {
            auto pair = dep.git_ref();
            ref_type = pair.first;
            ref = pair.second;
        }

        // Locked-as-hints: url+ref unchanged → carry the entry verbatim.
        // No ls-remote, no download. The closure through the carried
        // entry is still re-enqueued: the hint lock may have been pruned
        // since it was written, and missing transitives then resolve
        // normally instead of failing the graph later.
        if (hints && !grew) {
            const LockDep* hint = hints->find_remote(dep.url, ref_type, ref);
            if (hint && !hint->integrity.empty() &&
                (dep.is_archive() || !hint->commit.empty())) {
                if (lockfile.deps.emplace(hint->key, *hint).second)
                    entry_count++;
                std::string hash = hint->cache_hash();
                if (!hash.empty()) {
                    Path dep_cache = cache_dir_ / hash;
                    if (auto sub_manifest = Manifest::load_moid(dep_cache)) {
                        sub_manifest->project_dir = dep_cache;
                        auto active = active_features_of(*sub_manifest,
                                                         accumulated);
                        if (!active) return std::nullopt;
                        enqueue_scopes(*sub_manifest, *active);
                    }
                }
                continue;
            }
        }

        // Resolve the dependency (network).
        auto lock_dep = resolve_dependency(dep);
        if (!lock_dep) return std::nullopt;

        lockfile.deps[lock_dep->key] = *lock_dep;
        entry_count++;

        // Recurse into native cached sources for transitive deps.
        std::string hash = lock_dep->cache_hash();
        if (!hash.empty()) {
            Path dep_cache = cache_dir_ / hash;
            if (auto sub_manifest = Manifest::load_moid(dep_cache)) {
                sub_manifest->project_dir = dep_cache;
                auto active = active_features_of(*sub_manifest, accumulated);
                if (!active) return std::nullopt;
                enqueue_scopes(*sub_manifest, *active);
            }
        }
    }

    return lockfile;
}

// Rebuild a lockfile restricted to the entries reachable from the
// manifest's declared closure (every scope, defaults plus the feature
// activations requested on dependency edges, with conflicting defaults
// demoted). Zero network: reachability walks path-dep manifests and
// cached native sources through lock entries.
// Used by `bake remove` to drop entries the manifest no longer references.
export Lockfile prune_lock(const Manifest& manifest, const Lockfile& lock) {
    Lockfile pruned;

    std::map<Path, std::set<std::string>> visited;
    std::function<void(const Manifest&, const std::set<std::string>&)> walk =
        [&](const Manifest& m, const std::set<std::string>& activated) {
            const std::set<std::string> effective =
                demote_conflicting_defaults(m, activated);
            std::vector<const std::map<std::string, Dependency>*> scopes;
            scopes.push_back(&m.dependencies);
            for (const auto& condition : m.targets)
                scopes.push_back(&condition.dependencies);
            for (const auto& name : effective)
                if (m.features.count(name))
                    scopes.push_back(&m.features.at(name).dependencies);
            for (auto* scope : scopes) {
                for (auto& [alias, dep] : *scope) {
                    (void)alias;
                    if (dep.is_path_dep) {
                        Path dep_dir = (m.project_dir / dep.path.c_str())
                                           .lexically_normal();
                        if ((dep_dir / "bake.toml").is_regular_file()) {
                            auto& seen = visited[dep_dir.absolute()];
                            bool grew = false;
                            for (const auto& name : dep.features)
                                if (seen.insert(name).second) grew = true;
                            if (grew || seen.size() == dep.features.size()) {
                                if (auto sub = Manifest::load_moid(dep_dir)) {
                                    sub->project_dir = dep_dir;
                                    walk(*sub, seen);
                                }
                            }
                        }
                        continue;
                    }

                    std::string ref_type = dep.is_archive() ? "archive" : "";
                    std::string ref;
                    if (ref_type.empty()) {
                        auto pair = dep.git_ref();
                        ref_type = pair.first;
                        ref = pair.second;
                    }
                    const LockDep* entry =
                        lock.find_remote(dep.url, ref_type, ref);
                    if (!entry) continue;  // lock was already stale here
                    pruned.deps.emplace(entry->key, *entry);

                    // Recurse into cached native sources for transitive
                    // reach, tracking feature activations per source.
                    Path cache_dir =
                        get_cache_dir() / entry->cache_hash();
                    if (cache_dir.is_directory()) {
                        auto& seen = visited[cache_dir.absolute()];
                        bool grew = false;
                        for (const auto& name : dep.features)
                            if (seen.insert(name).second) grew = true;
                        if (grew || seen.size() == dep.features.size()) {
                            if (auto sub = Manifest::load_moid(cache_dir)) {
                                sub->project_dir = cache_dir;
                                walk(*sub, seen);
                            }
                        }
                    }
                }
            }
        };
    walk(manifest, {});
    return pruned;
}


} // namespace bake
