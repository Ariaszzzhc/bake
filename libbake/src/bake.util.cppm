module;

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <optional>
#include <functional>
#include <memory>
#include <algorithm>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

export module bake.util;

// ============================================================
// bake.util — foundational utilities
//   Path, SHA-256, glob, string utils, process spawning
// ============================================================

namespace bake {

// ===== String utilities =====

export inline std::string trim(std::string_view s) {
    size_t start = 0, end = s.size();
    while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r'))
        ++start;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n' || s[end - 1] == '\r'))
        --end;
    return std::string(s.substr(start, end - start));
}

export inline std::vector<std::string> split(std::string_view s, char delim) {
    std::vector<std::string> result;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
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
    for (size_t i = 1; i < parts.size(); ++i) {
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
    std::string native() const { return m_path.native(); }

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

    // Use a unique temp suffix to avoid collisions between concurrent processes.
    // Combine PID with a random component.
    std::string tmp = path.string() + "." + std::to_string(getpid()) + ".tmp";

    // Open with O_CREAT | O_TRUNC | O_WRONLY for explicit fd control
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::filesystem::remove(tmp);
        return false;
    }

    // Write all data
    size_t written = 0;
    const char* data = content.data();
    while (written < content.size()) {
        ssize_t n = write(fd, data + written, content.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            std::filesystem::remove(tmp);
            return false;
        }
        written += static_cast<size_t>(n);
    }

    // fsync ensures data reaches durable storage before rename
    if (fsync(fd) != 0) {
        close(fd);
        std::filesystem::remove(tmp);
        return false;
    }
    close(fd);

    // Atomic rename on POSIX (same filesystem)
    std::error_code ec;
    std::filesystem::rename(tmp, path.string(), ec);
    if (ec) {
        std::filesystem::remove(tmp);
        return false;
    }

    // fsync the parent directory to ensure the rename is durable.
    // Without this, a crash after rename may leave the directory entry
    // in an inconsistent state on disk.
    int dir_fd = open(path.parent().string().c_str(), O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    return true;
}

export inline bool file_exists(const Path& p) { return p.is_regular_file(); }
export inline bool dir_exists(const Path& p) { return p.is_directory(); }

// ===== SHA-256 =====

export class SHA256 {
public:
    static std::array<uint8_t, 32> compute(const uint8_t* data, size_t len) {
        uint32_t state[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };

        std::vector<uint8_t> msg(data, data + len);
        uint64_t bit_len = static_cast<uint64_t>(len) * 8;
        msg.push_back(0x80);
        while (msg.size() % 64 != 56) msg.push_back(0x00);
        for (int i = 7; i >= 0; --i)
            msg.push_back(static_cast<uint8_t>(bit_len >> (i * 8)));

        for (size_t offset = 0; offset < msg.size(); offset += 64)
            process_block(msg.data() + offset, state);

        std::array<uint8_t, 32> result{};
        for (int i = 0; i < 8; ++i) {
            result[i * 4]     = static_cast<uint8_t>(state[i] >> 24);
            result[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
            result[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
            result[i * 4 + 3] = static_cast<uint8_t>(state[i]);
        }
        return result;
    }

    static std::array<uint8_t, 32> compute(std::string_view data) {
        return compute(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    static std::array<uint8_t, 32> compute_file(const Path& path) {
        auto content = read_file(path);
        if (!content) return {};
        return compute(*content);
    }

    static std::string to_hex(const std::array<uint8_t, 32>& hash) {
        static const char hex[] = "0123456789abcdef";
        std::string result(64, '0');
        for (size_t i = 0; i < 32; ++i) {
            result[i * 2]     = hex[hash[i] >> 4];
            result[i * 2 + 1] = hex[hash[i] & 0x0f];
        }
        return result;
    }

    static std::string hex(std::string_view data) { return to_hex(compute(data)); }
    static std::string hex_file(const Path& path) { return to_hex(compute_file(path)); }

private:
    static constexpr uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    static void process_block(const uint8_t* block, uint32_t state[8]) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
};

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
    size_t ti = 0, pi = 0;
    size_t star_t = std::string_view::npos, star_p = std::string_view::npos;
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
        std::fprintf(stderr, "bake: failed to execute '%s': %s\n", argv_c[0], std::strerror(errno));
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

} // namespace bake
