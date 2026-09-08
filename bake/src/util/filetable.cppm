export module bake.util.filetable;

import std;
import bake.util;

// ============================================================
// bake.util.filetable — file identity and freshness
//
// One authority for "has this file changed since we last
// digested it": the (size, mtime) stamp is the fast path, the
// content digest is the truth. A recorded stamp that still
// matches vouches for a previously computed digest — the trust
// model shared by the toolchain cache manifests and the LSP
// file table.
// ============================================================

namespace bake {

export struct FileStamp {
    std::uintmax_t size = 0;
    std::int64_t mtime = 0;
    bool valid = false;

    bool operator==(const FileStamp&) const = default;
};

// "size mtime" — the manifest line prefix format (stable across
// cache versions; do not change).
export std::string format_stamp(const FileStamp& s) {
    return s.valid ? std::to_string(s.size) + " " + std::to_string(s.mtime)
                   : std::string("- -");
}

// Parse the "size mtime" prefix of a manifest line; invalid stamp
// when the placeholders are present.
export FileStamp parse_stamp(std::string_view prefix) {
    FileStamp s;
    if (prefix == "- -") return s;
    auto sp = prefix.find(' ');
    if (sp == std::string_view::npos) return s;
    auto mp = prefix.find(' ', sp + 1);
    if (mp != std::string_view::npos) return s;  // extra fields → malformed
    std::string size(prefix.substr(0, sp));
    std::string mtime(prefix.substr(sp + 1));
    if (size.empty() || mtime.empty()) return s;
    for (char c : size)
        if (c < '0' || c > '9') return s;
    for (char c : mtime)
        if (c < '0' || c > '9') return s;
    s.size = std::stoull(size);
    s.mtime = std::stoll(mtime);
    s.valid = true;
    return s;
}

// Current stat of a path; an invalid stamp marks a missing file.
export FileStamp file_stamp(const Path& p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    FileStamp s;
    if (auto st = fs::status(p.fs(), ec); fs::exists(st)) {
        s.size = fs::file_size(p.fs(), ec);
        s.mtime = std::int64_t(
            fs::last_write_time(p.fs(), ec).time_since_epoch().count());
        s.valid = !ec;
    }
    return s;
}

export class FileTable {
public:
    // SHA-256 hex digest of the file's content ("-" when unreadable),
    // memoized per canonical path. A stamp match vouches for the
    // recorded digest; a mismatch re-hashes and re-records.
    std::string digest(const Path& p) {
        std::string key = p.absolute().lexically_normal().string();
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& entry = m_entries[key];
        FileStamp now = file_stamp(p);
        if (entry.stamp == now && entry.digest_known)
            return entry.digest;
        entry.stamp = now;
        entry.digest = now.valid ? SHA256::hex_file(p) : std::string("-");
        entry.digest_known = true;
        return entry.digest;
    }

    // Freshness of a path against a caller-recorded stamp (e.g. parsed
    // from a manifest line). Missing files are never unchanged.
    bool unchanged(const Path& p, const FileStamp& recorded) {
        if (!recorded.valid) return false;
        return file_stamp(p) == recorded;
    }

    // Drop memoized state for a path — call after observing a write.
    void invalidate(const Path& p) {
        std::string key = p.absolute().lexically_normal().string();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.erase(key);
    }

private:
    struct Entry {
        FileStamp stamp;
        std::string digest;
        bool digest_known = false;
    };
    std::map<std::string, Entry> m_entries;
    std::mutex m_mutex;
};

} // namespace bake
