export module bake.buildsystem.source;

import std;
import bake.util;
import bake.buildsystem.project;

namespace bake {
// ===== Source discovery =====

export struct SourceSet {
    std::vector<Path> cpp_files;
    std::vector<Path> c_files;
    std::vector<Path> module_interfaces;
    std::vector<Path> public_headers;

    bool empty() const {
        return cpp_files.empty() && c_files.empty() &&
               module_interfaces.empty() && public_headers.empty();
    }
};

export SourceSet discover_sources(
        const Path& src_dir, const Path& public_dir,
        const SourceExtConfig& ext) {
    SourceSet sources;

    auto match_ext = [](const Path& p, const std::vector<std::string>& exts) {
        for (const auto& e : exts)
            if (p.has_extension(e)) return true;
        return false;
    };

    if (src_dir.is_directory()) {
        for (auto& entry : std::filesystem::recursive_directory_iterator(src_dir.fs())) {
            if (!entry.is_regular_file()) continue;
            Path p(entry.path());
            if (match_ext(p, ext.source_ext)) {
                if (match_ext(p, ext.module_ext)) {
                    sources.module_interfaces.push_back(p);
                } else {
                    sources.cpp_files.push_back(p);
                }
            } else if (match_ext(p, ext.module_ext)) {
                sources.module_interfaces.push_back(p);
            }
        }
    }

    if (public_dir.is_directory()) {
        for (auto& entry : std::filesystem::recursive_directory_iterator(public_dir.fs())) {
            if (!entry.is_regular_file()) continue;
            Path p(entry.path());
            if (match_ext(p, ext.module_ext)) {
                sources.module_interfaces.push_back(p);
            } else if (match_ext(p, ext.header_ext)) {
                sources.public_headers.push_back(p);
            }
        }
    }

    std::sort(sources.cpp_files.begin(), sources.cpp_files.end());
    std::sort(sources.c_files.begin(), sources.c_files.end());
    std::sort(sources.module_interfaces.begin(), sources.module_interfaces.end());
    std::sort(sources.public_headers.begin(), sources.public_headers.end());

    return sources;
}

// ===== Module scanning (P1689) =====

export struct ModuleInfo {
    std::string source_path;
    std::string module_name;
    bool is_interface = false;
    std::vector<std::string> imports;
};

namespace {

// Strip // and /* */ comments, preserving newlines so line structure
// survives for the line-based scanners below.
std::string strip_comments(const std::string& content) {
    std::string stripped;
    stripped.reserve(content.size());
    bool in_block = false;
    bool in_line = false;
    for (std::size_t i = 0; i < content.size(); ++i) {
        char c = content[i];
        if (in_block) {
            if (c == '*' && i + 1 < content.size() && content[i + 1] == '/') {
                in_block = false;
                ++i;
                stripped += ' ';
            } else if (c == '\n') {
                stripped += '\n';
            }
        } else if (in_line) {
            if (c == '\n') {
                in_line = false;
                stripped += '\n';
            }
        } else {
            if (c == '/' && i + 1 < content.size()) {
                if (content[i + 1] == '*') {
                    in_block = true;
                    ++i;
                    stripped += ' ';
                    continue;
                }
                if (content[i + 1] == '/') {
                    in_line = true;
                    ++i;
                    stripped += ' ';
                    continue;
                }
            }
            stripped += c;
        }
    }
    return stripped;
}

// One #include target: the name between quotes or angle brackets, plus
// whether it was quoted (quoted includes also search the includer's
// directory first, matching the preprocessor).
std::optional<std::pair<std::string, bool>> parse_include_line(
    std::string_view raw) {
    auto start = raw.find_first_not_of(" \t");
    if (start == std::string_view::npos) return std::nullopt;
    std::string_view line = raw.substr(start);
    if (!line.starts_with('#')) return std::nullopt;
    line.remove_prefix(1);
    auto head = line.find_first_not_of(" \t");
    if (head == std::string_view::npos ||
        !line.substr(head).starts_with("include"))
        return std::nullopt;
    line.remove_prefix(head + 7);
    auto name = line.find_first_not_of(" \t");
    if (name == std::string_view::npos) return std::nullopt;
    const bool quoted = line[name] == '"';
    if (!quoted && line[name] != '<') return std::nullopt;
    const char close = quoted ? '"' : '>';
    auto end = line.find(close, name + 1);
    if (end == std::string_view::npos) return std::nullopt;
    return std::make_pair(
        std::string(line.substr(name + 1, end - name - 1)), quoted);
}

} // namespace

// Local-header closure of one translation unit, resolved against the
// action's include directories: every reachable project header becomes a
// build input so mtime comparison and fingerprints see header edits.
// Unresolved names are system/toolchain headers — their paths are
// content-addressed, so any re-provisioning changes the command and the
// fingerprint; they are skipped here. -include forced headers arrive as
// raw flag tokens; the "-include <file>" pairs are picked out below.
//
// Three memoization levels keep large graphs cheap: per-file parse
// results (the include directives of a header are identical for every TU
// that reaches it), per-file resolved children (same include-dir list),
// and per-source closures. A 900-TU vendored tree parses each of its
// ~250 headers once, not 900 times.
export class HeaderClosureScanner {
public:
    std::vector<Path> closure(const Path& source,
                              const std::vector<Path>& include_dirs,
                              const std::vector<std::string>& extra_flags) {
        const std::string dirs_key = join_dirs(include_dirs);
        const std::string closure_key = source.string() + '\x1f' + dirs_key;
        auto cached = closures_.find(closure_key);
        if (cached != closures_.end()) return cached->second;

        std::vector<Path> result;
        std::set<std::string> visited{source.string()};
        std::deque<Path> pending{source};

        // BFS across the include graph; the cap bounds pathological
        // vendored trees without affecting real projects.
        while (!pending.empty() && result.size() < 512) {
            Path file = pending.front();
            pending.pop_front();
            for (auto& child : children(file, include_dirs, dirs_key)) {
                if (visited.insert(child.string()).second) {
                    result.push_back(child);
                    pending.push_back(child);
                }
            }
        }
        for (std::size_t i = 0; i + 1 < extra_flags.size(); ++i) {
            if (extra_flags[i] != "-include") continue;
            auto resolved =
                resolve(extra_flags[i + 1], true, source, include_dirs);
            ++i;
            if (!resolved) continue;
            if (visited.insert(resolved->string()).second)
                result.push_back(*resolved);
        }

        closures_.emplace(std::move(closure_key), result);
        return result;
    }

private:
    static std::string join_dirs(const std::vector<Path>& include_dirs) {
        std::string key;
        for (const auto& dir : include_dirs) {
            key += dir.string();
            key += '\x1e';
        }
        return key;
    }

    static std::optional<Path> resolve(const std::string& name, bool quoted,
                                       const Path& includer,
                                       const std::vector<Path>& include_dirs) {
        std::vector<Path> candidates;
        if (quoted) candidates.push_back(includer.parent() / name);
        for (const auto& dir : include_dirs)
            candidates.push_back(dir / name);
        for (const auto& candidate : candidates) {
            std::error_code ec;
            auto canonical =
                std::filesystem::weakly_canonical(candidate.fs(), ec);
            if (ec) continue;
            Path resolved(canonical);
            if (resolved.is_regular_file()) return resolved;
        }
        return std::nullopt;
    }

    // Resolved local headers one file includes, memoized per
    // (file, include-dir list).
    const std::vector<Path>& children(const Path& file,
                                      const std::vector<Path>& include_dirs,
                                      const std::string& dirs_key) {
        const std::string key = file.string() + '\x1f' + dirs_key;
        auto cached = children_.find(key);
        if (cached != children_.end()) return cached->second;

        std::vector<Path> resolved;
        for (auto& [name, quoted] : parsed_includes(file)) {
            auto hit = resolve(name, quoted, file, include_dirs);
            if (hit) resolved.push_back(*hit);
        }
        auto [inserted, _] = children_.emplace(std::move(key),
                                               std::move(resolved));
        return inserted->second;
    }

    // Include directives of one file (comment-stripped text scan),
    // memoized by path: identical for every TU reaching the file.
    const std::vector<std::pair<std::string, bool>>& parsed_includes(
        const Path& file) {
        auto cached = parsed_.find(file.string());
        if (cached != parsed_.end()) return cached->second;

        std::vector<std::pair<std::string, bool>> names;
        if (auto content = read_file(file)) {
            std::istringstream lines(strip_comments(*content));
            std::string line;
            while (std::getline(lines, line)) {
                if (auto include = parse_include_line(line))
                    names.push_back(std::move(*include));
            }
        }
        auto [inserted, _] = parsed_.emplace(file.string(),
                                             std::move(names));
        return inserted->second;
    }

    std::map<std::string, std::vector<std::pair<std::string, bool>>> parsed_;
    std::map<std::string, std::vector<Path>> children_;
    std::map<std::string, std::vector<Path>> closures_;
};

// Text-based module scanner: extracts module name and imports by parsing
// `export module <name>;` and `import <name>;` declarations directly.
// This replaces the former clang-scan-deps subprocess — bake no longer needs
// any external binary for C++ module dependency scanning.
export std::optional<ModuleInfo> scan_module_file(const Path& source) {
    auto content = read_file(source);
    if (!content) return std::nullopt;

    // Strip comments so declarations inside /* */ or after // are ignored.
    std::string stripped = strip_comments(*content);

    ModuleInfo info;
    info.source_path = source.string();

    std::istringstream ss(stripped);
    std::string line;
    while (std::getline(ss, line)) {
        // Trim leading whitespace.
        auto pos = line.find_first_not_of(" \t");
        if (pos == std::string::npos) continue;
        std::string_view sv(line.data() + pos, line.size() - pos);

        // export module <name>;
        if (sv.starts_with("export module ")) {
            sv.remove_prefix(14);
            auto semi = sv.find(';');
            if (semi == std::string_view::npos) continue;
            auto name = sv.substr(0, semi);
            // Trim surrounding whitespace from the name.
            auto b = name.find_first_not_of(" \t");
            auto e = name.find_last_not_of(" \t");
            if (b == std::string_view::npos) continue;
            info.module_name = std::string(name.substr(b, e - b + 1));
            info.is_interface = true;
        }
        // module <name>;  (implementation unit — implicitly imports its module)
        else if (sv.starts_with("module ") && !sv.starts_with("module;")) {
            sv.remove_prefix(7);
            auto semi = sv.find(';');
            if (semi == std::string_view::npos) continue;
            auto name = sv.substr(0, semi);
            auto b = name.find_first_not_of(" \t");
            auto e = name.find_last_not_of(" \t");
            if (b == std::string_view::npos) continue;
            info.imports.push_back(std::string(name.substr(b, e - b + 1)));
        }
        // import <name>;  (also covers "export import <name>;")
        else if (sv.starts_with("import ") || sv.starts_with("export import ")) {
            if (sv.starts_with("export "))
                sv.remove_prefix(7);
            if (sv.starts_with("import "))
                sv.remove_prefix(7);
            auto semi = sv.find(';');
            if (semi == std::string_view::npos) continue;
            auto name = sv.substr(0, semi);
            auto b = name.find_first_not_of(" \t");
            auto e = name.find_last_not_of(" \t");
            if (b == std::string_view::npos) continue;
            info.imports.push_back(std::string(name.substr(b, e - b + 1)));
        }
    }

    return info;
}

} // namespace bake
