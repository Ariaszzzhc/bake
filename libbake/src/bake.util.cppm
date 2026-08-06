module;

#ifdef _WIN32
  #include <windows.h>
  #include <process.h>
  #include <io.h>
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
#else
  #include <sys/wait.h>
  #include <unistd.h>
  #include <fcntl.h>
  #ifdef __APPLE__
    #include <mach-o/dyld.h>
  #endif
#endif
#include <errno.h>

export module bake.util;

import std;

// ============================================================
// bake.util — foundational utilities
//   Path, SHA-256, glob, string utils, process spawning
// ============================================================

namespace bake {

// ===== String utilities =====

export inline std::string trim(std::string_view s) {
    std::size_t start = 0, end = s.size();
    while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r'))
        ++start;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n' || s[end - 1] == '\r'))
        --end;
    return std::string(s.substr(start, end - start));
}

export inline std::vector<std::string> split(std::string_view s, char delim) {
    std::vector<std::string> result;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == delim) {
            result.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return result;
}

export inline bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

export inline bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

export inline bool contains(std::string_view s, std::string_view sub) {
    return s.find(sub) != std::string_view::npos;
}

export inline std::string join(const std::vector<std::string>& parts, std::string_view delim) {
    if (parts.empty()) return {};
    std::string result = parts[0];
    for (std::size_t i = 1; i < parts.size(); ++i) {
        result += delim;
        result += parts[i];
    }
    return result;
}

export inline std::string to_lower(std::string_view s) {
    std::string result(s);
    for (auto& c : result) {
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
    }
    return result;
}

// ===== Path =====

export class Path {
public:
    Path() = default;
    Path(const char* p) : m_path(p) {}
    Path(std::string_view p) : m_path(p) {}
    Path(const std::string& p) : m_path(p) {}
    Path(const std::filesystem::path& p) : m_path(p) {}

    const std::filesystem::path& fs() const { return m_path; }
    std::string string() const { return m_path.string(); }
    // On Windows, path::native() returns wstring. Use string() for portable narrow string.
    std::string native() const {
#ifdef _WIN32
        return m_path.string();
#else
        return m_path.native();
#endif
    }

    Path operator/(const Path& rhs) const { return Path(m_path / rhs.m_path); }
    Path operator/(const char* rhs) const { return Path(m_path / rhs); }
    Path& operator/=(const Path& rhs) { m_path /= rhs.m_path; return *this; }
    Path& operator/=(const char* rhs) { m_path /= rhs; return *this; }

    bool operator==(const Path& rhs) const { return m_path == rhs.m_path; }
    bool operator<(const Path& rhs) const { return m_path < rhs.m_path; }

    bool exists() const { return std::filesystem::exists(m_path); }
    bool is_directory() const { return std::filesystem::is_directory(m_path); }
    bool is_regular_file() const { return std::filesystem::is_regular_file(m_path); }

    Path parent() const { return Path(m_path.parent_path()); }
    Path filename() const { return Path(m_path.filename()); }
    Path stem() const { return Path(m_path.stem()); }
    Path extension() const { return Path(m_path.extension()); }
    Path absolute() const { return Path(std::filesystem::absolute(m_path)); }
    Path lexically_normal() const { return Path(m_path.lexically_normal()); }

    bool has_extension(std::string_view ext) const {
        return m_path.extension().string() == ext;
    }

    bool is_cpp() const { return has_extension(".cpp") || has_extension(".cc") || has_extension(".cxx"); }
    bool is_c() const { return has_extension(".c"); }
    bool is_module_interface() const { return has_extension(".cppm") || has_extension(".ixx"); }

    std::string stem_string() const { return m_path.stem().string(); }
    std::string filename_string() const { return m_path.filename().string(); }

    void mkdir_recursive() const { std::filesystem::create_directories(m_path); }
    void remove() const { std::filesystem::remove(m_path); }
    void remove_all() const { std::filesystem::remove_all(m_path); }

    static Path current() { return Path(std::filesystem::current_path()); }
    static void current(const Path& p) { std::filesystem::current_path(p.fs()); }

private:
    std::filesystem::path m_path;
};

export inline std::string to_string(const Path& p) { return p.string(); }

// ===== Cross-platform PATH search =====

// Search PATH for an executable. On Windows, also tries .exe/.cmd/.bat extensions.
export inline std::optional<Path> find_in_path(const std::string& name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return std::nullopt;

#ifdef _WIN32
    const char path_sep = ';';
    std::vector<std::string> exts = {".exe", ".cmd", ".bat"};
    // If name already has an extension, don't append others
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos) exts.clear();
#else
    const char path_sep = ':';
#endif

    std::string path_str(path_env);
    std::size_t start = 0;
    while (start <= path_str.size()) {
        std::size_t end = path_str.find(path_sep, start);
        if (end == std::string::npos) end = path_str.size();
        std::string dir = path_str.substr(start, end - start);
        start = end + 1;

        if (dir.empty()) continue;

#ifdef _WIN32
        if (exts.empty()) {
            Path candidate = Path(dir) / name;
            if (candidate.is_regular_file()) return candidate;
        } else {
            for (auto& ext : exts) {
                Path candidate = Path(dir) / (name + ext);
                if (candidate.is_regular_file()) return candidate;
            }
        }
#else
        Path candidate = Path(dir) / name;
        if (candidate.is_regular_file()) return candidate;
#endif
    }
    return std::nullopt;
}

// ===== File utilities =====

export inline std::optional<std::string> read_file(const Path& path) {
    std::ifstream f(path.fs(), std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

export inline bool write_file(const Path& path, std::string_view content) {
    if (!path.parent().exists()) { path.parent().mkdir_recursive(); }
    std::ofstream f(path.fs(), std::ios::binary);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return f.good();
}

// Atomic write: temp file → fsync → rename.
// Prevents partial writes from corrupting the destination on crash.
export inline bool atomic_write_file(const Path& path, std::string_view content) {
    if (!path.parent().exists()) { path.parent().mkdir_recursive(); }

#ifdef _WIN32
    // Windows: write to temp file, then MoveFileEx with REPLACE_EXISTING.
    std::string tmp = path.string() + "." + std::to_string(GetCurrentProcessId()) + ".tmp";

    // Write content using ofstream
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { std::filesystem::remove(tmp); return false; }
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f.good()) { f.close(); std::filesystem::remove(tmp); return false; }
    f.close();

    // Atomic replace via MoveFileEx
    std::wstring wtmp = std::filesystem::path(tmp).wstring();
    std::wstring wdest = path.fs().wstring();
    if (!MoveFileExW(wtmp.c_str(), wdest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::filesystem::remove(tmp);
        return false;
    }
    return true;
#else
    // POSIX: open + write + fsync + rename + dir fsync
    std::string tmp = path.string() + "." + std::to_string(getpid()) + ".tmp";

    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::filesystem::remove(tmp);
        return false;
    }

    std::size_t written = 0;
    const char* data = content.data();
    while (written < content.size()) {
        ssize_t n = write(fd, data + written, content.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            std::filesystem::remove(tmp);
            return false;
        }
        written += static_cast<std::size_t>(n);
    }

    if (fsync(fd) != 0) {
        close(fd);
        std::filesystem::remove(tmp);
        return false;
    }
    close(fd);

    std::error_code ec;
    std::filesystem::rename(tmp, path.string(), ec);
    if (ec) {
        std::filesystem::remove(tmp);
        return false;
    }

    // fsync parent directory for durability
    int dir_fd = open(path.parent().string().c_str(), O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    return true;
#endif
}

export inline bool file_exists(const Path& p) { return p.is_regular_file(); }
export inline bool dir_exists(const Path& p) { return p.is_directory(); }

// ===== SHA-256 =====

export class SHA256 {
public:
    static std::array<std::uint8_t, 32> compute(const std::uint8_t* data, std::size_t len) {
        std::uint32_t state[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };

        std::vector<std::uint8_t> msg(data, data + len);
        std::uint64_t bit_len = static_cast<std::uint64_t>(len) * 8;
        msg.push_back(0x80);
        while (msg.size() % 64 != 56) msg.push_back(0x00);
        for (int i = 7; i >= 0; --i)
            msg.push_back(static_cast<std::uint8_t>(bit_len >> (i * 8)));

        for (std::size_t offset = 0; offset < msg.size(); offset += 64)
            process_block(msg.data() + offset, state);

        std::array<std::uint8_t, 32> result{};
        for (int i = 0; i < 8; ++i) {
            result[i * 4]     = static_cast<std::uint8_t>(state[i] >> 24);
            result[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
            result[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
            result[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
        }
        return result;
    }

    static std::array<std::uint8_t, 32> compute(std::string_view data) {
        return compute(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    }

    static std::array<std::uint8_t, 32> compute_file(const Path& path) {
        auto content = read_file(path);
        if (!content) return {};
        return compute(*content);
    }

    static std::string to_hex(const std::array<std::uint8_t, 32>& hash) {
        static const char hex[] = "0123456789abcdef";
        std::string result(64, '0');
        for (std::size_t i = 0; i < 32; ++i) {
            result[i * 2]     = hex[hash[i] >> 4];
            result[i * 2 + 1] = hex[hash[i] & 0x0f];
        }
        return result;
    }

    static std::string hex(std::string_view data) { return to_hex(compute(data)); }
    static std::string hex_file(const Path& path) { return to_hex(compute_file(path)); }

private:
    static constexpr std::uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    static std::uint32_t rotr(std::uint32_t x, std::uint32_t n) { return (x >> n) | (x << (32 - n)); }

    static void process_block(const std::uint8_t* block, std::uint32_t state[8]) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ ((~e) & g);
            std::uint32_t temp1 = h + S1 + ch + K[i] + w[i];
            std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t temp2 = S0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
};

// ===== Self executable path =====

// Returns the canonical absolute path to the running executable.
// Returns an empty string if the path cannot be determined.
export inline std::string get_self_exe_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    std::wstring wide(buf, len);
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(needed - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                        result.data(), needed, nullptr, nullptr);
    return result;
#elif defined(__APPLE__)
    std::uint32_t bufsize = 0;
    _NSGetExecutablePath(nullptr, &bufsize);
    if (bufsize == 0) return {};
    std::string raw(bufsize, '\0');
    if (_NSGetExecutablePath(raw.data(), &bufsize) != 0) return {};
    raw.resize(std::strlen(raw.c_str()));
    std::error_code ec;
    auto canonical = std::filesystem::canonical(raw, ec);
    if (ec) return raw;
    return canonical.string();
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len < 0) return {};
    buf[len] = '\0';
    return std::string(buf);
#endif
}

// ===== Runtime resource path resolution =====
//
// All paths are resolved at runtime relative to the executable, eliminating
// compile-time defines that break when the binary is relocated or packaged.
// Mirrors Zig's approach: find lib/ by walking up from the exe.

// Walk up from an executable path looking for a lib/ directory that
// contains libcxx/include. Returns an empty path if not found within
// 6 parent levels.
inline Path find_lib_from_executable(std::string_view exe_path) {
    if (exe_path.empty()) return Path();
    Path dir = Path(std::string(exe_path)).parent();
    for (int i = 0; i < 6; i++) {
        Path lib = dir / "lib";
        if ((lib / "libcxx" / "include").is_directory())
            return lib;
        if (dir == dir.parent()) break;
        dir = dir.parent();
    }
    return Path();
}

// Returns the bake resource directory (the "lib/" folder).
// Resolution order:
//   1. BAKE_LIB_DIR env var (explicit override)
//   2. BAKE_EXE env var — the real bake binary (set for build_app so it
//      resolves resources relative to bake, not the temporary build_app)
//   3. get_self_exe_path() — this process's own executable
//   4. Empty path (not found)
export inline const Path& find_lib_dir() {
    static Path result = []() -> Path {
        // 1. Explicit lib override
        if (const char* env = std::getenv("BAKE_LIB_DIR")) {
            Path p(env);
            if ((p / "libcxx" / "include").is_directory())
                return p;
        }
        // 2. Real bake executable path (exported by build_with_build_cpp)
        if (const char* exe = std::getenv("BAKE_EXE")) {
            Path lib = find_lib_from_executable(exe);
            if (!lib.string().empty()) return lib;
        }
        // 3. Current process executable
        Path lib = find_lib_from_executable(get_self_exe_path());
        if (!lib.string().empty()) return lib;
        // 4. Not found
        return Path();
    }();
    return result;
}

// Returns the workspace root (parent of lib/).
export inline const Path& find_workspace_root() {
    static Path result = []() -> Path {
        const Path& lib = find_lib_dir();
        if (lib.string().empty()) return Path();
        return lib.parent();
    }();
    return result;
}

// Returns the clang resource directory (builtin headers: stdarg.h, stddef.h).
export inline const Path& find_clang_resource_dir() {
    static Path result = []() -> Path {
        if (const char* env = std::getenv("BAKE_RESOURCE_DIR"))
            return Path(env);
        const Path& root = find_workspace_root();
        if (!root.string().empty()) {
            namespace fs = std::filesystem;
            fs::path base = root.fs() / "external" / "llvm-install" / "lib" / "clang";
            if (fs::is_directory(base)) {
                for (auto& entry : fs::directory_iterator(base))
                    if (entry.is_directory())
                        return Path(entry.path());
            }
        }
        return Path();
    }();
    return result;
}

// Returns the LLVM prefix (for finding clang-scan-deps, etc.).
export inline const Path& find_llvm_prefix() {
    static Path result = []() -> Path {
        if (const char* env = std::getenv("BAKE_LLVM_PREFIX"))
            return Path(env);
        const Path& root = find_workspace_root();
        if (!root.string().empty()) {
            Path p = root / "external" / "llvm-install";
            if (p.is_directory())
                return p;
        }
        return Path();
    }();
    return result;
}

// ===== Process =====

export struct ProcessResult {
    int exit_code = -1;
    std::string stdout_output;
    std::string stderr_output;
    bool success() const { return exit_code == 0; }
};

export ProcessResult run_process(const std::vector<std::string>& args,
                                  const Path& working_dir = Path(),
                                  bool capture_output = false);

export ProcessResult run_process(const std::string& cmd,
                                  const std::vector<std::string>& args,
                                  const Path& working_dir = Path(),
                                  bool capture_output = false);

// ===== Glob matching =====

export std::vector<Path> glob(const Path& base, std::string_view pattern);

// ===== Glob implementation =====

namespace detail {

inline bool glob_match(std::string_view text, std::string_view pattern) {
    std::size_t ti = 0, pi = 0;
    std::size_t star_t = std::string_view::npos, star_p = std::string_view::npos;
    while (ti < text.size() || pi < pattern.size()) {
        if (pi < pattern.size()) {
            if (pi + 1 < pattern.size() && pattern[pi] == '*' && pattern[pi + 1] == '*') {
                pi += 2;
                if (pi < pattern.size() && pattern[pi] == '/') pi++;
                star_p = pi;
                star_t = ti;
                continue;
            }
            if (pattern[pi] == '*') {
                star_p = pi;
                star_t = ti;
                pi++;
                continue;
            }
            if (pi < pattern.size() && ti < text.size() &&
                (pattern[pi] == '?' || pattern[pi] == text[ti])) {
                if (pattern[pi] == '?' && text[ti] == '/') {
                    // ? does not match /
                } else {
                    pi++;
                    ti++;
                    continue;
                }
            }
        }
        if (star_p != std::string_view::npos) {
            pi = star_p + 1;
            if (pi <= pattern.size() && pattern[star_p] == '*' &&
                (star_p == 0 || pattern[star_p - 1] != '*')) {
                if (star_t < text.size() && text[star_t] != '/') {
                    star_t++;
                    ti = star_t;
                    continue;
                }
            } else {
                if (star_t < text.size()) {
                    star_t++;
                    ti = star_t;
                    continue;
                }
            }
        }
        return false;
    }
    return true;
}

} // namespace detail

inline std::vector<Path> glob(const Path& base, std::string_view pattern) {
    std::vector<Path> results;
    if (!base.is_directory()) return results;

    // Always use recursive iteration — patterns with directory prefixes
    // (e.g. "src/*.cpp") need to descend into subdirectories.
    for (auto& entry : std::filesystem::recursive_directory_iterator(base.fs())) {
        if (!entry.is_regular_file()) continue;
        auto rel = std::filesystem::relative(entry.path(), base.fs());
        if (detail::glob_match(rel.string(), pattern))
            results.push_back(Path(entry.path()));
    }

    std::sort(results.begin(), results.end());
    return results;
}

// ===== Process implementation =====

inline ProcessResult run_process(const std::string& cmd,
                          const std::vector<std::string>& args,
                          const Path& working_dir,
                          bool capture_output) {
    std::vector<std::string> full_args;
    full_args.push_back(cmd);
    for (auto& a : args) full_args.push_back(a);
    return run_process(full_args, working_dir, capture_output);
}

#ifdef _WIN32

// Windows implementation using CreateProcessW
inline ProcessResult run_process(const std::vector<std::string>& args,
                          const Path& working_dir,
                          bool capture_output) {
    ProcessResult result;
    if (args.empty()) return result;

    // Build command line string with proper quoting
    std::string cmdline;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmdline += ' ';
        // Quote args containing spaces
        if (args[i].find(' ') != std::string::npos ||
            args[i].find('\t') != std::string::npos) {
            cmdline += '"';
            cmdline += args[i];
            cmdline += '"';
        } else {
            cmdline += args[i];
        }
    }

    // Convert to wide string
    std::wstring wcmdline(cmdline.begin(), cmdline.end());
    std::wstring wworkdir;
    if (!working_dir.string().empty()) {
        wworkdir = working_dir.fs().wstring();
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr, stdout_write = nullptr;
    HANDLE stderr_read = nullptr, stderr_write = nullptr;

    if (capture_output) {
        CreatePipe(&stdout_read, &stdout_write, &sa, 0);
        CreatePipe(&stderr_read, &stderr_write, &sa, 0);
        // Prevent child from inheriting read ends
        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (capture_output) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = stdout_write;
        si.hStdError = stderr_write;
        // stdin: use the parent's
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    BOOL ok = CreateProcessW(
        nullptr,                           // lpApplicationName (NULL = parse cmdline)
        const_cast<LPWSTR>(wcmdline.c_str()),
        nullptr, nullptr,                  // process/thread security
        capture_output ? TRUE : FALSE,     // inherit handles
        0,                                 // creation flags
        nullptr,                           // environment (inherit)
        wworkdir.empty() ? nullptr : wworkdir.c_str(),
        &si, &pi);

    if (!ok) {
        if (capture_output) {
            if (stdout_read) CloseHandle(stdout_read);
            if (stdout_write) CloseHandle(stdout_write);
            if (stderr_read) CloseHandle(stderr_read);
            if (stderr_write) CloseHandle(stderr_write);
        }
        return result;
    }

    // Close write ends in parent
    if (capture_output) {
        CloseHandle(stdout_write);
        CloseHandle(stderr_write);
    }

    // Read captured output
    if (capture_output) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(stdout_read, buf, sizeof(buf), &n, nullptr) && n > 0)
            result.stdout_output.append(buf, n);
        while (ReadFile(stderr_read, buf, sizeof(buf), &n, nullptr) && n > 0)
            result.stderr_output.append(buf, n);
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return result;
}

#else

// POSIX implementation using fork/exec
inline ProcessResult run_process(const std::vector<std::string>& args,
                          const Path& working_dir,
                          bool capture_output) {
    ProcessResult result;
    if (args.empty()) return result;

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

    if (capture_output) {
        if (pipe(stdout_pipe) != 0) return result;
        if (pipe(stderr_pipe) != 0) {
            close(stdout_pipe[0]); close(stdout_pipe[1]);
            return result;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (capture_output) {
            close(stdout_pipe[0]); close(stdout_pipe[1]);
            close(stderr_pipe[0]); close(stderr_pipe[1]);
        }
        return result;
    }

    if (pid == 0) {
        if (!working_dir.string().empty() && working_dir.is_directory())
            chdir(working_dir.string().c_str());

        if (capture_output) {
            dup2(stdout_pipe[1], STDOUT_FILENO);
            dup2(stderr_pipe[1], STDERR_FILENO);
            close(stdout_pipe[0]); close(stdout_pipe[1]);
            close(stderr_pipe[0]); close(stderr_pipe[1]);
        }

        std::vector<char*> argv_c;
        argv_c.reserve(args.size() + 1);
        for (auto& a : args)
            argv_c.push_back(const_cast<char*>(a.c_str()));
        argv_c.push_back(nullptr);

        execvp(argv_c[0], argv_c.data());
        std::println(std::cerr, "bake: failed to execute '{}': {}", argv_c[0], std::strerror(errno));
        _exit(127);
    }

    if (capture_output) {
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        char buf[4096];
        ssize_t n;
        while ((n = read(stdout_pipe[0], buf, sizeof(buf))) > 0)
            result.stdout_output.append(buf, n);
        close(stdout_pipe[0]);

        while ((n = read(stderr_pipe[0], buf, sizeof(buf))) > 0)
            result.stderr_output.append(buf, n);
        close(stderr_pipe[0]);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

#endif // _WIN32

} // namespace bake
