export module bake.toolchain.target;

import std;
import bake.util;

// ============================================================
// bake.toolchain.target — target vocabulary
//
// TargetSpec is the single source of truth for what bake compiles
// for: the triple string plus glibc/macos version suffixes. All
// property queries derive from it. This type is the interface
// between the build system and the toolchain.
// ============================================================

namespace bake {

// ===== Cross-compilation target =====
//
// TargetSpec stores the full triple string (arch-vendor-os-libc).
// When passed to Clang via -target, LLVM normalizes internally.
// The triple string is the single source of truth. All property
// queries (is_darwin, is_linux_musl, arch, …) derive from it.

export struct TargetSpec {
    std::string triple_;  // "arch-os[-abi]" (empty when native)
    bool native_ = true;  // true = compiling for host (no -target flag)

    // glibc target version ("x86_64-linux-gnu.2.17" → {2,17}). Only
    // meaningful for the gnu libc family. Defaults to bake's baseline.
    int glibc_major_ = 0;
    int glibc_minor_ = 0;

    // macOS deployment minimum ("aarch64-macos.12" / "aarch64-macos.12.0"
    // → {12,0}). The deployment target is a property of the target query,
    // not a free-form flag: an explicit -mmacosx-version-min on the
    // command line is forwarded to clang but overridden by the injected
    // value. Defaults to default_macos_min.
    int macos_min_major_ = 0;
    int macos_min_minor_ = 0;

    static constexpr int default_glibc_major = 2;
    static constexpr int default_glibc_minor = 28;
    static constexpr int default_macos_min_major = 14;
    static constexpr int default_macos_min_minor = 0;

    bool is_native() const { return native_; }

    bool is_darwin() const {
        return triple_.contains("macos") || triple_.contains("darwin")
            || triple_.contains("apple");
    }

    bool is_linux() const { return triple_.contains("linux"); }

    bool is_linux_musl() const {
        return triple_.contains("linux") && triple_.contains("musl");
    }

    bool is_android() const {
        return triple_.contains("linux") && triple_.contains("android");
    }

    // glibc ("*-linux-gnu", optionally ".2.17"-versioned). The default
    // Linux ELF ABI — a bare "linux" triple with no libc segment is gnu.
    bool is_linux_gnu() const {
        return is_linux() && !is_linux_musl() && !is_android();
    }

    bool is_windows() const {
        return triple_.contains("windows");
    }

    bool is_windows_gnu() const {
        return triple_.contains("windows") && triple_.contains("gnu");
    }

    // Architecture component — first segment of the triple.
    std::string arch() const {
        auto pos = triple_.find('-');
        return pos == std::string::npos ? triple_ : triple_.substr(0, pos);
    }

    // Returns the triple string for the -target flag, or "" for native.
    // Never carries the glibc/darwin version suffix — LLVM triples can't
    // reliably encode it.
    std::string triple() const { return native_ ? "" : triple_; }

    // Triple WITH the target version suffix ("x86_64-linux-gnu.2.36",
    // "aarch64-macos.12"), for argv consumed by bake's own driver shim:
    // its preprocessing strips the suffix back off before LLVM parses the
    // triple and records the version for header pinning / deployment-min
    // injection / link interception. Only meaningful when a version was
    // explicitly requested.
    std::string triple_with_version() const {
        if (native_) return triple_;
        if (glibc_major_ || glibc_minor_)
            return triple_ + "." + std::to_string(glibc_major()) + "." +
                   std::to_string(glibc_minor());
        if (macos_min_major_)
            return triple_ + "." + std::to_string(macos_min_major_) +
                   (macos_min_minor_
                        ? "." + std::to_string(macos_min_minor_) : "");
        return triple_;
    }

    // Deployment minimum for darwin targets, as "MAJOR.MINOR". Explicit
    // -target suffix wins; otherwise the built-in default.
    std::string macos_deployment_min() const {
        if (macos_min_major_)
            return std::to_string(macos_min_major_) + "." +
                   std::to_string(macos_min_minor_);
        return std::to_string(default_macos_min_major) + "." +
               std::to_string(default_macos_min_minor);
    }

    int glibc_major() const {
        return glibc_major_ ? glibc_major_ : default_glibc_major;
    }
    int glibc_minor() const {
        return glibc_minor_ || glibc_major_ ? glibc_minor_
                                            : default_glibc_minor;
    }
};

// Host macOS deployment minimum, from SystemVersion.plist
// ("ProductVersion" → "26.5"). The single detection point for native
// darwin deployment targets — the target spec carries it so that compile
// injection, link platform_version and cache keys all read one value.
// Falls back to the built-in default when detection fails.
static void detect_host_macos_min(TargetSpec& t) {
#if defined(__APPLE__)
    if (auto content = read_file(
            Path("/System/Library/CoreServices/SystemVersion.plist"))) {
        auto key_pos = content->find("ProductVersion");
        if (key_pos == std::string::npos) return;
        auto open = content->find("<string>", key_pos);
        if (open == std::string::npos) return;
        auto start = open + 8;
        auto end = content->find("</string>", start);
        if (end == std::string::npos) return;
        std::string ver = content->substr(start, end - start);
        int major = 0, minor = 0;
        if (std::sscanf(ver.c_str(), "%d.%d", &major, &minor) == 2 &&
            major > 0) {
            t.macos_min_major_ = major;
            t.macos_min_minor_ = minor;
        }
    }
#endif
}

// Detect host platform. This is the ONLY place with #ifdef for host detection.
export TargetSpec detect_host_target() {
    TargetSpec t;
    t.native_ = true;
#if defined(__APPLE__) && defined(__aarch64__)
    t.triple_ = "aarch64-apple-darwin";
    detect_host_macos_min(t);
#elif defined(__APPLE__) && defined(__x86_64__)
    t.triple_ = "x86_64-apple-darwin";
    detect_host_macos_min(t);
#elif defined(__linux__) && defined(__GLIBC__) && defined(__aarch64__)
    // Linked against the host's glibc (stage-0 build on a glibc distro):
    // native gnu — may use the system libc directly (SystemGnu layout).
    t.triple_ = "aarch64-linux-gnu";
    t.glibc_major_ = __GLIBC__;
    t.glibc_minor_ = __GLIBC_MINOR__;
#elif defined(__linux__) && defined(__GLIBC__) && defined(__x86_64__)
    t.triple_ = "x86_64-linux-gnu";
    t.glibc_major_ = __GLIBC__;
    t.glibc_minor_ = __GLIBC_MINOR__;
#elif defined(__linux__) && defined(__aarch64__)
    // Self-hosted bake is a static musl binary — hermetic musl everywhere.
    t.triple_ = "aarch64-linux-musl";
#elif defined(__linux__) && defined(__x86_64__)
    t.triple_ = "x86_64-linux-musl";
#elif defined(_WIN32) && defined(_M_X64)
    t.triple_ = "x86_64-windows-gnu";
#elif defined(_WIN32) && defined(_M_ARM64)
    t.triple_ = "aarch64-windows-gnu";
#else
    t.triple_ = "unknown-unknown-none";
#endif
    return t;
}

// Parse a user-supplied target spec into a canonical triple.
// Preserves all segments (including vendor). Normalizes arch aliases
// (arm64→aarch64, amd64→x86_64) and handles MinGW legacy triples.
export TargetSpec parse_target(std::string_view spec) {
    TargetSpec t;
    if (spec.empty() || spec == "native" || spec == "host") return t;
    t.native_ = false;

    std::vector<std::string> segments;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= spec.size(); ++i) {
        if (i == spec.size() || spec[i] == '-') {
            if (i > start)
                segments.emplace_back(spec.substr(start, i - start));
            start = i + 1;
        }
    }

    if (segments.empty()) return t;

    // Normalize architecture aliases to LLVM canonical names.
    if (segments[0] == "arm64")  segments[0] = "aarch64";
    if (segments[0] == "amd64")  segments[0] = "x86_64";

    // Normalize MinGW triples: x86_64-w64-mingw32 → x86_64-windows-gnu.
    for (std::size_t i = 1; i < segments.size(); ++i) {
        if (segments[i].starts_with("mingw32")) {
            segments.erase(segments.begin() + 1, segments.begin() + i + 1);
            segments.push_back("windows-gnu");
            break;
        }
    }

    // Strip "unknown" vendor: LLVM normalizes x86_64-linux-musl →
    // x86_64-unknown-linux-musl internally, but bake uses 3-segment
    // triples (arch-os-libc) consistently.
    if (segments.size() == 4 && segments[1] == "unknown")
        segments.erase(segments.begin() + 1);

    // glibc version suffix: last segment "gnu.2.17" → triple keeps "gnu",
    // {2,17} stored on the spec (LLVM triples cannot encode it).
    auto& last = segments.back();
    if (last.starts_with("gnu.")) {
        std::string_view suffix(last.data() + 4, last.size() - 4);
        int major = 0, minor = 0;
        std::size_t i = 0;
        bool ok = suffix.size() > 2;  // at least "N.M"
        for (; ok && i < suffix.size() && suffix[i] != '.'; ++i) {
            if (suffix[i] < '0' || suffix[i] > '9') { ok = false; break; }
            major = major * 10 + (suffix[i] - '0');
        }
        if (ok && (i >= suffix.size() || i == 0)) ok = false;
        for (++i; ok && i < suffix.size(); ++i) {
            if (suffix[i] < '0' || suffix[i] > '9') { ok = false; break; }
            minor = minor * 10 + (suffix[i] - '0');
        }
        if (ok && i != suffix.size()) ok = false;
        if (ok && major == 2 && minor > 0) {
            last = "gnu";
            t.glibc_major_ = major;
            t.glibc_minor_ = minor;
        }
    }

    // darwin deployment-version suffix. Two shapes:
    //   query style: "macos.12" / "darwin.14.1"   (bake target queries)
    //   LLVM style:  "darwin24" / "macosx14.0.0"  (clang -target form)
    // → triple keeps "macos"/"darwin", {major,minor} stored on the spec.
    // One to three numeric components; the patch level is ignored.
    for (auto& seg : segments) {
        std::string os;
        std::string_view suffix;
        if (seg.starts_with("macos.")) {
            os = "macos";
            suffix = std::string_view(seg.data() + 6, seg.size() - 6);
        } else if (seg.starts_with("darwin.")) {
            os = "darwin";
            suffix = std::string_view(seg.data() + 7, seg.size() - 7);
        } else if (seg.starts_with("macosx") && seg.size() > 6 &&
                   seg[6] >= '0' && seg[6] <= '9') {
            os = "macos";
            suffix = std::string_view(seg.data() + 6, seg.size() - 6);
        } else if (seg.starts_with("darwin") && seg.size() > 6 &&
                   seg[6] >= '0' && seg[6] <= '9') {
            os = "darwin";
            suffix = std::string_view(seg.data() + 6, seg.size() - 6);
        } else {
            continue;
        }
        int comps[3] = {0, 0, 0};
        int ci = 0;
        bool ok = !suffix.empty();
        for (std::size_t k = 0; ok && k < suffix.size(); ++k) {
            char c = suffix[k];
            if (c == '.') {
                if (++ci >= 3) ok = false;
            } else if (c >= '0' && c <= '9') {
                comps[ci] = comps[ci] * 10 + (c - '0');
            } else {
                ok = false;
            }
        }
        if (ok && comps[0] > 0) {
            seg = os;
            t.macos_min_major_ = comps[0];
            t.macos_min_minor_ = comps[1];
        }
        break;
    }

    // Bare "arch-linux" defaults to gnu (the default Linux ELF ABI).
    if (segments.size() == 2 && segments[1] == "linux")
        segments.push_back("gnu");

    t.triple_ = segments[0];
    for (std::size_t i = 1; i < segments.size(); ++i)
        t.triple_ += "-" + segments[i];

    return t;
}

// ===== Triple pattern matching =====
//
// Match a target triple against a pattern. Both are split on '-'.
// '*' matches an entire segment. Segment counts must be equal.

export bool triple_matches(std::string_view triple, std::string_view pattern) {
    auto split = [](std::string_view s) {
        std::vector<std::string> segs;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '-') {
                if (i > start) segs.emplace_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        return segs;
    };

    auto triple_segs = split(triple);
    auto pattern_segs = split(pattern);

    if (triple_segs.size() != pattern_segs.size()) return false;
    for (std::size_t i = 0; i < triple_segs.size(); ++i) {
        if (pattern_segs[i] == "*") continue;
        if (pattern_segs[i] != triple_segs[i]) return false;
    }
    return true;
}

} // namespace bake
