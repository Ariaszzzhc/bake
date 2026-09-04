export module bake.util.cachestore;

import std;
import bake.util;

// ============================================================
// bake.util.cachestore — quota management for content-addressed
// product stores
//
// Every on-disk build product cache in bake shares one layout:
//   <namespace>/h/<key>.txt     manifest (final digest + per-input
//                               size/mtime/digest lines)
//   <namespace>/o/<digest>/     products
// Products are content-addressed, so eviction is always safe: a
// removed directory turns its manifest into a clean miss that
// rebuilds on demand. The toolchain runtime cache enforces its
// quota through this module.
// ============================================================

namespace bake {

export struct CacheQuota {
    std::uintmax_t limit_bytes = 0;  // 0 → unlimited

    // Per-namespace limit from BAKE_CACHE_LIMIT_MB; 8 GiB default.
    static CacheQuota from_env() {
        CacheQuota q;
        if (const char* env = std::getenv("BAKE_CACHE_LIMIT_MB");
            env && *env) {
            long long mb = std::atoll(env);
            if (mb > 0)
                q.limit_bytes = static_cast<std::uintmax_t>(mb) * 1024 * 1024;
        } else {
            q.limit_bytes = std::uintmax_t{8} * 1024 * 1024 * 1024;
        }
        return q;
    }
};

namespace {

std::uintmax_t directory_size(const Path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::uintmax_t total = 0;
    for (auto it = fs::recursive_directory_iterator(dir.fs(), ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code sec;
        if (it->is_regular_file(sec))
            total += it->file_size(sec);
    }
    return total;
}

} // namespace

// Enforce the quota on a namespace directory (the one containing
// h/ and o/). Evicts least-recently-modified product directories
// under o/ until the total fits. `protect` (a product directory
// just written by this process) is never evicted. Manifests under
// h/ are left alone: a stale manifest misses cleanly on lookup.
export void cache_enforce_quota(const Path& namespace_dir,
                                const CacheQuota& quota,
                                const Path& protect = {}) {
    if (quota.limit_bytes == 0) return;

    Path o_dir = namespace_dir / "o";
    if (!o_dir.is_directory()) return;

    namespace fs = std::filesystem;
    std::error_code ec;

    // Collect product directories with their latest modification time.
    struct Candidate {
        Path dir;
        std::int64_t mtime;
        std::uintmax_t size;
    };
    std::vector<Candidate> candidates;
    std::uintmax_t total = 0;
    for (auto& entry : fs::directory_iterator(o_dir.fs(), ec)) {
        if (ec || !entry.is_directory()) continue;
        Candidate c;
        c.dir = Path(entry.path());
        c.size = directory_size(c.dir);
        std::error_code tec;
        c.mtime = std::int64_t(
            fs::last_write_time(entry.path(), tec).time_since_epoch().count());
        if (tec) c.mtime = 0;
        total += c.size;
        candidates.push_back(std::move(c));
    }

    if (total <= quota.limit_bytes) return;

    // Oldest first.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.mtime < b.mtime;
              });

    for (const auto& c : candidates) {
        if (total <= quota.limit_bytes) break;
        if (!protect.string().empty() && c.dir == protect) continue;
        std::uintmax_t before = c.size;
        std::error_code rmec;
        fs::remove_all(c.dir.fs(), rmec);
        if (!rmec) {
            total -= before;
            std::println(std::cerr, "bake: cache quota: evicted '{}'",
                         c.dir.string());
        }
    }
}

} // namespace bake
