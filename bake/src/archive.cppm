export module bake.archive;

import std;
import bake.util;

// ============================================================
// bake.archive — safe archive extraction for the package fetcher.
//
// The interface owns the security policy (path/link validation,
// resource caps, post-extraction tree verification); the format
// handling is a stage-selected implementation unit of this same
// module:
//
//   stage0/archive_cli.cpp   spawns system tar/unzip (CMake bootstrap)
//   archive.cpp              in-process libarchive (bake-pkgs
//                            "libarchive" package)
//
// Both implementations enforce the policy below — the CLI one by
// pre-scanning `tar -tvf` / `unzip -l` listings, the libarchive one by
// checking structured entry headers while streaming.
// ============================================================

namespace bake::arch {

// ── resource caps ──
inline constexpr std::size_t max_files = 100'000;
inline constexpr std::uintmax_t max_total_size = 1ULL * 1024 * 1024 * 1024;  // 1 GB

// ── entry policy (shared by both implementations) ──

// Normalize separators; zip files produced by some tools store '\'.
inline std::string normalize_entry_path(std::string_view name) {
    std::string out(name);
    for (char& c : out)
        if (c == '\\') c = '/';
    while (!out.empty() && (out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

// True if `name` (already normalized) is absolute or contains a ".."
// component — either would escape the extraction directory.
inline bool entry_path_is_safe(std::string_view name) {
    if (name.empty() || name.front() == '/') return false;
    for (std::size_t dotdot = 0;
         (dotdot = name.find("..", dotdot)) != std::string_view::npos;) {
        bool left_ok = (dotdot == 0 || name[dotdot - 1] == '/');
        bool right_ok = (dotdot + 2 >= name.size() || name[dotdot + 2] == '/');
        if (left_ok && right_ok) return false;
        dotdot += 2;
    }
    return true;
}

// Symlinks additionally may not point through the parent: an absolute
// or ".."-containing target would write outside the tree on use.
inline bool link_target_is_safe(std::string_view target) {
    return entry_path_is_safe(target);
}

// ── post-extraction verification (defense in depth) ──
//
// Walks the extracted tree and rejects relative paths containing "..",
// broken symlinks, symlinks resolving outside the tree, and resource
// cap violations. Both implementations run this after extraction in
// case their view of the archive differs from what materialized.
inline bool validate_extracted_tree(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;

    std::size_t file_count = 0;
    std::uintmax_t total_size = 0;

    fs::path canonical_dir = fs::canonical(dir);

    for (auto& entry : fs::recursive_directory_iterator(
            dir, fs::directory_options::follow_directory_symlink)) {
        auto rel = fs::relative(entry.path(), dir);
        if (rel.string().find("..") != std::string::npos) {
            std::println(std::cerr,
                "bake: rejected path traversal in archive: {}",
                rel.string());
            return false;
        }

        if (entry.is_symlink()) {
            std::error_code ec;
            fs::path resolved = fs::canonical(entry.path(), ec);
            if (ec) {
                std::println(std::cerr, "bake: broken symlink in archive: {}",
                             entry.path().string());
                return false;
            }
            std::string r = resolved.string();
            std::string t = canonical_dir.string();
            if (r.rfind(t, 0) != 0) {
                std::println(std::cerr,
                    "bake: symlink escapes extraction directory: {} -> {}",
                    entry.path().string(), r);
                return false;
            }
        }

        if (entry.is_regular_file()) {
            file_count++;
            total_size += entry.file_size();

            if (file_count > max_files) {
                std::println(std::cerr,
                    "bake: archive exceeds file count limit ({})", max_files);
                return false;
            }
            if (total_size > max_total_size) {
                std::println(std::cerr,
                    "bake: archive exceeds size limit (1 GB)");
                return false;
            }
        }
    }

    return true;
}

// ── the contract ──

export bool extract(const Path& archive, const Path& dest_dir);

}  // namespace bake::arch
