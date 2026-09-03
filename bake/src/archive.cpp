// stage1 implementation — in-process extraction via libarchive
// (bake-pkgs "libarchive" package: zlib/xz/bzip2/zstd backends). Every
// entry is policy-checked from its structured header while streaming;
// unsafe archives never touch the filesystem. Replaces the stage0
// system-binary transport (src/stage0/archive_cli.cpp).

module;

#include <archive.h>
#include <archive_entry.h>

module bake.archive;

import std;
import bake.util;

namespace bake::arch {

namespace {

struct ArchiveReadCloser {
    void operator()(struct archive* a) const { archive_read_free(a); }
};

// Log a policy rejection and report failure to the caller.
bool reject(const Path& archive, std::string_view what,
            std::string_view detail) {
    std::println(std::cerr, "bake: rejecting {}: {} ({})", what, detail,
                 archive.string());
    return false;
}

}  // namespace

bool extract(const Path& archive, const Path& dest_dir) {
    std::unique_ptr<struct archive, ArchiveReadCloser> a(archive_read_new());
    if (!a) return false;
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());

    if (archive_read_open_filename(a.get(), archive.native().c_str(), 65536) !=
        ARCHIVE_OK) {
        std::println(std::cerr, "bake: failed to open archive {} ({})",
                     archive.string(), archive_error_string(a.get()));
        return false;
    }

    dest_dir.mkdir_recursive();

    std::size_t file_count = 0;
    std::uintmax_t total_size = 0;
    struct archive_entry* entry = nullptr;

    while (true) {
        int rc = archive_read_next_header(a.get(), &entry);
        if (rc == ARCHIVE_EOF) break;
        if (rc != ARCHIVE_OK) {
            std::println(std::cerr, "bake: failed to read archive {} ({})",
                         archive.string(), archive_error_string(a.get()));
            return false;
        }

        const char* raw_path = archive_entry_pathname(entry);
        std::string name = normalize_entry_path(raw_path ? raw_path : "");
        if (!name.empty() && name.back() == '/') name.pop_back();
        if (name.empty() || name == ".") continue;

        if (!entry_path_is_safe(name))
            return reject(archive, "unsafe path in archive", name);

        Path out = dest_dir / name.c_str();

        // Hardlinks to earlier entries only alias content, which a
        // source package never needs.
        if (archive_entry_hardlink(entry) != nullptr)
            return reject(archive, "hardlink entry in archive", name);

        mode_t filetype = archive_entry_filetype(entry);

        if (filetype == AE_IFDIR) {
            std::filesystem::create_directories(out.fs());
            continue;
        }

        if (filetype == AE_IFLNK) {
            const char* raw_target = archive_entry_symlink(entry);
            std::string target =
                normalize_entry_path(raw_target ? raw_target : "");
            if (!link_target_is_safe(target))
                return reject(archive, "symlink escaping archive",
                              name + " -> " + target);
            std::error_code error;
            std::filesystem::create_symlink(target, out.fs(), error);
            if (error)
                return reject(archive, "failed to create symlink",
                              name + " -> " + target);
            continue;
        }

        // Fifos, devices, sockets, whiteouts are not content.
        if (filetype != AE_IFREG) continue;

        // Regular file: stream the payload, enforcing caps as it lands.
        std::error_code parent_error;
        std::filesystem::create_directories(out.parent().fs(), parent_error);
        std::ofstream f(out.fs(), std::ios::binary | std::ios::trunc);
        if (!f) return reject(archive, "failed to create file", name);

        char buffer[65536];
        la_ssize_t n = 0;
        std::uintmax_t size = 0;
        bool oversize = false;
        while ((n = archive_read_data(a.get(), buffer, sizeof(buffer))) > 0) {
            f.write(buffer, static_cast<std::streamsize>(n));
            if (!f) return reject(archive, "failed to write file", name);
            size += static_cast<std::uintmax_t>(n);
            if (size > max_total_size) {
                oversize = true;
                break;
            }
        }
        if (n < 0) {
            std::println(std::cerr, "bake: failed to read {} in {} ({})",
                         name, archive.string(), archive_error_string(a.get()));
            return false;
        }
        if (oversize)
            return reject(archive, "archive exceeds size limit (1 GB)", name);
        f.close();

        // Preserve the execute bits only — ownership and fine-grained
        // permissions are transport detail.
        if (archive_entry_perm(entry) & 0111) {
            std::filesystem::permissions(
                out.fs(),
                std::filesystem::perms::owner_exec |
                    std::filesystem::perms::group_exec |
                    std::filesystem::perms::others_exec,
                std::filesystem::perm_options::add);
        }

        file_count++;
        total_size += size;
        if (file_count > max_files)
            return reject(archive, "archive exceeds file count limit",
                          "more than 100000 entries");
        if (total_size > max_total_size)
            return reject(archive, "archive exceeds size limit (1 GB)", name);
    }

    return validate_extracted_tree(dest_dir.fs());
}

}  // namespace bake::arch
