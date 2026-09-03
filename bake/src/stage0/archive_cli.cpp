// stage0 implementation — pre-scans listings from the system tar/unzip
// binaries, then extracts with them and verifies the tree. Compiled
// only by the CMake bootstrap; the self-hosted bake extracts in
// process via libarchive instead (src/archive.cpp).

module bake.archive;

import std;
import bake.util;

namespace bake::arch {

namespace {

// ── listing pre-scans ──
//
// Entries are listed and policy-checked BEFORE anything touches the
// filesystem. Limits and path/link rejection come from the interface's
// shared policy; the listings themselves are parsed from the tools'
// text output.

bool prescan_tar(const Path& archive) {
    auto result = run_process({"tar", "-tvf", archive.string()}, Path(), true);
    if (!result.success()) {
        std::println(std::cerr, "bake: failed to list archive entries");
        return false;
    }

    std::size_t file_count = 0;
    std::uintmax_t total_size = 0;

    // Verbose listing lines (BSD and GNU tar):
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

        // Find path: after the timestamp pattern "HH:MM " in the line.
        std::size_t time_end = std::string::npos;
        for (std::size_t i = 10; i + 1 < line.size(); ++i) {
            if (line[i] == ':' &&
                std::isdigit(static_cast<unsigned char>(line[i - 1]))) {
                if (i + 2 < line.size() && line[i + 1] == ' ') {
                    time_end = i + 2;
                    break;
                }
                if (i + 3 < line.size() &&
                    std::isdigit(static_cast<unsigned char>(line[i + 1])) &&
                    line[i + 2] == ' ') {
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

        entry = normalize_entry_path(entry);
        link_target = normalize_entry_path(link_target);
        if (entry.empty()) continue;

        if (!entry_path_is_safe(entry)) {
            std::println(std::cerr,
                "bake: rejecting unsafe path in archive: {}", entry);
            return false;
        }
        if (type_char == 'l' && !link_target.empty() &&
            !link_target_is_safe(link_target)) {
            std::println(std::cerr,
                "bake: rejecting symlink escaping archive: {} -> {}",
                entry, link_target);
            return false;
        }
        if (type_char == 'h' ||
            (type_char == 'l' && line.find(" link to ") != std::string::npos)) {
            std::println(std::cerr,
                "bake: rejecting hardlink entry in archive: {}", entry);
            return false;
        }

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

        if (file_count > max_files) {
            std::println(std::cerr,
                "bake: archive exceeds file count limit ({} entries)",
                max_files);
            return false;
        }
        if (total_size > max_total_size) {
            std::println(std::cerr,
                "bake: archive exceeds size limit (1 GB)");
            return false;
        }
    }

    return true;
}

bool prescan_zip(const Path& archive) {
    auto result = run_process({"unzip", "-l", archive.string()}, Path(), true);
    if (!result.success()) {
        std::println(std::cerr, "bake: failed to list zip entries");
        return false;
    }

    std::size_t file_count = 0;
    std::uintmax_t total_size = 0;

    // Entry lines:
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
        name = normalize_entry_path(name.substr(first));
        if (name.empty()) continue;

        if (!entry_path_is_safe(name)) {
            std::println(std::cerr,
                "bake: rejecting unsafe path in zip: {}", name);
            return false;
        }

        file_count++;
        total_size += length;

        if (file_count > max_files) {
            std::println(std::cerr,
                "bake: zip exceeds file count limit ({} entries)", max_files);
            return false;
        }
        if (total_size > max_total_size) {
            std::println(std::cerr,
                "bake: zip exceeds size limit (1 GB)");
            return false;
        }
    }

    return true;
}

}  // namespace

bool extract(const Path& archive, const Path& dest_dir) {
    const bool is_zip = ends_with(archive.string(), ".zip");
    if (is_zip ? !prescan_zip(archive) : !prescan_tar(archive)) return false;

    dest_dir.mkdir_recursive();

    // tar: POSIX strip ownership/permissions with flags supported by
    // both GNU tar and BSD tar. Extraction always targets a new, empty
    // directory, so GNU tar's non-portable --no-overwrite-dir is
    // unnecessary. Windows: plain extraction (Windows tar.exe doesn't
    // support those flags).
    std::vector<std::string> cmd;
    if (is_zip) {
        cmd = {"unzip", "-q", archive.string(), "-d", dest_dir.string()};
    } else {
        cmd = {"tar", "xf", archive.string(), "-C", dest_dir.string()};
#ifndef _WIN32
        cmd.push_back("--no-same-owner");
        cmd.push_back("--no-same-permissions");
#endif
    }
    if (!run_process(cmd, Path(), true).success()) {
        std::println(std::cerr, "bake: failed to extract {}", archive.string());
        return false;
    }

    return validate_extracted_tree(dest_dir.fs());
}

}  // namespace bake::arch
