module;

#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

export module bake.buildsystem.engine;

import std;
import bake.util;
import bake.buildsystem.project;
import bake.buildsystem.moid;
import bake.buildsystem.graph;
import bake.buildsystem.cmdgen;
import bake.toolchain.target;
import bake.toolchain.runtime;
import bake.json;
namespace json = bake::json;


// ============================================================
// bake.buildsystem.engine — MoidDeclaration, unified DAG, executor
//
// Two-phase architecture:
//   Configure: resolve inputs + merge bake.toml config → MoidDeclaration JSON
//   Build:      declarations → unified DAG → graph.json → execute
// ============================================================

namespace bake {
export enum class ArtifactKind {
    Object,
    Module,
    StaticLibrary,
    SharedLibrary,
    Executable,
};

export struct ArtifactRef {
    ArtifactKind kind;
    Path path;
    std::string producer_action;

    auto operator<=>(const ArtifactRef& other) const {
        if (kind != other.kind) return kind <=> other.kind;
        const std::string normalized =
            path.fs().lexically_normal().generic_string();
        const std::string other_normalized =
            other.path.fs().lexically_normal().generic_string();
        if (normalized != other_normalized)
            return normalized <=> other_normalized;
        return producer_action <=> other.producer_action;
    }

    bool operator==(const ArtifactRef& other) const {
        return (*this <=> other) == 0;
    }
};

export struct CompileUsage {
    std::vector<Path> include_dirs;
    std::vector<std::string> defines;
    std::map<std::string, ArtifactRef> modules;
};

export struct LinkInterface {
    std::vector<ArtifactRef> objects;
    std::vector<ArtifactRef> libraries;
    std::vector<std::string> system_libraries;
    std::vector<std::string> frameworks;
};

export struct MoidExports {
    CompileUsage compile;
    LinkInterface link;
    std::optional<ArtifactRef> terminal;
};

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
class HeaderClosureScanner {
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

} // namespace

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

// ===== Module topological sort =====

// Compute the full transitive closure of module imports.
export std::vector<std::string> module_import_closure(
        const std::vector<std::string>& roots,
        const std::map<std::string, std::vector<std::string>>& imports) {
    std::set<std::string> visited;
    std::vector<std::string> queue(roots.begin(), roots.end());
    while (!queue.empty()) {
        std::string mod = std::move(queue.back());
        queue.pop_back();
        if (!visited.insert(mod).second) continue;
        auto it = imports.find(mod);
        if (it != imports.end()) {
            for (const auto& imp : it->second) {
                if (!visited.count(imp)) queue.push_back(imp);
            }
        }
    }
    return std::vector<std::string>(visited.begin(), visited.end());
}

// Topological sort of module interfaces using Kahn's algorithm.
// Returns module names in dependency order.
inline std::vector<std::string> topo_sort_modules(
        const std::map<std::string, ModuleInfo>& modules) {
    std::map<std::string, int> in_degree;
    std::map<std::string, std::vector<std::string>> adj;

    for (auto& [name, info] : modules)
        in_degree[name] = 0;

    for (auto& [name, info] : modules) {
        for (auto& imp : info.imports) {
            if (modules.count(imp)) {
                adj[imp].push_back(name);
                in_degree[name]++;
            }
        }
    }

    std::vector<std::string> queue;
    for (auto& [name, deg] : in_degree)
        if (deg == 0) queue.push_back(name);
    std::sort(queue.begin(), queue.end());

    std::vector<std::string> sorted;
    while (!queue.empty()) {
        std::string node = queue.front();
        queue.erase(queue.begin());
        sorted.push_back(node);

        for (auto& dependent : adj[node]) {
            if (--in_degree[dependent] == 0)
                queue.push_back(dependent);
        }
        std::sort(queue.begin(), queue.end());
    }

    if (sorted.size() != modules.size()) {
        for (auto& [name, info] : modules)
            if (std::find(sorted.begin(), sorted.end(), name) == sorted.end())
                sorted.push_back(name);
    }

    return sorted;
}

// ===== Build action =====

export struct BuildAction {
    enum class Type { CompileModule, Compile, Link, Archive };
    Type type;
    std::string id;              // content-derived unique identifier
    std::string moid_id;         // canonical owning moid identity
    std::string description;     // progress display text
    std::string moid;            // owning moid display name
    std::string moid_version;    // owning moid version (for display)
    std::vector<Path> inputs;
    std::vector<Path> outputs;
    std::vector<std::string> command;
    std::vector<std::size_t> depends_on;

    bool is_compile() const { return type == Type::Compile || type == Type::CompileModule; }
};

// ===== Build graph =====

export struct BuildGraph {
    std::vector<BuildAction> actions;
    Path state_dir;
    Path project_root;           // workspace root (execution working dir)
};

// ===== Default input declaration =====
//
// Discovers inputs from the default src/ and public/ layout.

export MoidDeclaration declare_default_inputs(
        const Manifest& manifest, const Layout& layout,
        const std::string& canonical_id,
        const std::vector<std::string>& active_features,
        const std::vector<MoidDependency>& dependencies) {
    MoidDeclaration declaration;
    declaration.id = canonical_id;
    declaration.name = manifest.moid->name;
    declaration.version = manifest.moid->version;
    declaration.type = manifest.moid->type;
    declaration.root = layout.root.absolute().string();
    declaration.cxx_std = manifest.moid->cxx_std;
    declaration.c_std = manifest.moid->c_std;
    declaration.active_features = active_features;
    declaration.dependencies = dependencies;

    const Path root = layout.root.absolute();
    auto source_pattern = [&](const Path& source) {
        std::error_code error;
        auto relative = std::filesystem::relative(
            source.fs(), root.fs(), error);
        if (!error && !relative.empty())
            return relative.lexically_normal().generic_string();
        return source.fs().lexically_normal().generic_string();
    };
    auto is_public_source = [&](const Path& source) {
        if (!layout.public_dir.is_directory()) return false;
        std::error_code error;
        auto relative = std::filesystem::relative(
            source.fs(), layout.public_dir.fs(), error);
        if (error || relative.empty()) return false;
        auto first = relative.begin();
        return first != relative.end() && *first != "..";
    };
    auto add_source = [&](const Path& source, bool is_public) {
        SourceGroup group;
        group.pattern = source_pattern(source);
        group.is_public = is_public;
        declaration.sources.push_back(std::move(group));
    };

    auto source_ext = manifest.resolve_source_ext();
    auto sources = discover_sources(layout.source_dir, layout.public_dir, source_ext);
    for (const auto& source : sources.cpp_files)
        add_source(source, false);
    for (const auto& source : sources.c_files)
        add_source(source, false);
    for (const auto& source : sources.module_interfaces)
        add_source(source, is_public_source(source));

    if (layout.public_dir.is_directory())
        declaration.public_include_dirs.push_back("public");

    for (const auto& [name, dependency] : manifest.dependencies) {
        if (!dependency.is_path_dep) continue;
        Path dependency_dir = layout.root / dependency.path;
        if ((dependency_dir / "bake.toml").is_regular_file()) continue;

        Path dependency_public = dependency_dir / "public";
        if (dependency_public.is_directory())
            declaration.public_include_dirs.push_back(
                source_pattern(dependency_public));

        Path dependency_source = dependency_dir / "src";
        if (!dependency_source.is_directory()) continue;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(dependency_source.fs())) {
            if (!entry.is_regular_file()) continue;
            Path source(entry.path());
            // Match against the configured extensions
            auto match_ext = [&source](const std::vector<std::string>& exts) {
                for (const auto& e : exts)
                    if (source.has_extension(e)) return true;
                return false;
            };
            if (match_ext(source_ext.source_ext) || match_ext(source_ext.module_ext))
                add_source(source, false);
        }
    }

    std::sort(declaration.sources.begin(), declaration.sources.end(),
              [](const SourceGroup& left, const SourceGroup& right) {
                  return left.pattern < right.pattern;
              });
    return declaration;
}

// ===== Unified DAG builder =====
//
// Translates a resolved MoidGraph into a flat BuildGraph of compile/link
// actions with explicit dependency edges.
//
// Data flow:
//
//   MoidGraph (topological moid order)
//        │
//        ▼
//   Resolve declarations → per-moid source lists, include dirs, module map
//        │
//        ▼
//   Module scan: cross-moid import resolution → topo-ordered module interfaces
//        │
//        ▼
//   Compile module interfaces → PCM + .o actions (with module dep edges)
//        │
//        ▼
//   Compile regular sources → .o actions (with module dep edges)
//        │
//        ▼
//   Link / archive → terminal artifacts (executable / .a / .dylib)
//
// Input discovery and bake.toml configuration are resolved before this point;
// the DAG builder consumes one MoidDeclaration and never distinguishes how its
// inputs were described.

export bool is_portable_name(std::string_view name) {
    auto is_ascii_alnum = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9');
    };
    if (name.empty() || !is_ascii_alnum(
                            static_cast<unsigned char>(name.front())) ||
        name.back() == '.') {
        return false;
    }
    for (unsigned char c : name) {
        if (!is_ascii_alnum(c) && c != '.' && c != '_' && c != '-' &&
            c != '+' && c != '@') {
            return false;
        }
    }

    std::string stem(name.substr(0, name.find('.')));
    for (char& c : stem) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
        return false;
    if (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9' &&
        (stem.substr(0, 3) == "COM" || stem.substr(0, 3) == "LPT")) {
        return false;
    }
    return true;
}

namespace {
Path resolve_path(const std::string& s, const Path& base) {
    Path p(s);
    if (!p.fs().is_absolute())
        p = base / s;
    return p.lexically_normal();
}

std::string moid_storage_key(std::string_view canonical_id) {
    return SHA256::hex(canonical_id).substr(0, 24);
}

std::string action_id_for(std::string_view kind, std::string_view owner_key,
                          std::string_view source_identity = {}) {
    std::string id = std::string(kind) + ":" + std::string(owner_key);
    if (!source_identity.empty())
        id += ":" + std::string(source_identity);
    return id;
}

std::string normalized_source_id(const Path& root, const Path& source) {
    std::error_code ec;
    auto rel = std::filesystem::relative(source.fs(), root.fs(), ec);
    if (!ec && !rel.empty())
        return rel.lexically_normal().generic_string();
    return source.fs().lexically_normal().generic_string();
}

std::optional<std::string> validate_terminal_relative_path(
        std::filesystem::path relative) {
    relative = relative.lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_path())
        return std::nullopt;
    const auto parent = relative.parent_path().generic_string();
    const auto filename = relative.filename().generic_string();
    if ((parent != "bin" && parent != "lib") ||
        filename.empty() ||
        filename == "." || filename == "..") {
        return std::nullopt;
    }
    return relative.generic_string();
}

std::optional<std::string> terminal_relative_path(
        const Path& out_dir, const Path& output) {
    auto relative = output.fs().lexically_normal().lexically_relative(
        out_dir.fs().lexically_normal());
    return validate_terminal_relative_path(std::move(relative));
}

std::string portable_terminal_key(std::string_view relative_output) {
    std::string key(relative_output);
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return key;
}

std::string normalized_artifact_path(const Path& path) {
    return path.fs().lexically_normal().generic_string();
}

bool same_artifact(const ArtifactRef& left, const ArtifactRef& right) {
    return left.kind == right.kind &&
           normalized_artifact_path(left.path) ==
               normalized_artifact_path(right.path) &&
           left.producer_action == right.producer_action;
}

void append_unique_path(std::vector<Path>& paths, const Path& path) {
    const std::string key = normalized_artifact_path(path);
    if (std::ranges::none_of(paths, [&](const Path& existing) {
            return normalized_artifact_path(existing) == key;
        })) {
        paths.push_back(path);
    }
}

void append_unique_string(std::vector<std::string>& values,
                          const std::string& value) {
    if (std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

void append_unique_artifact(std::vector<ArtifactRef>& artifacts,
                            const ArtifactRef& artifact) {
    if (std::ranges::none_of(artifacts, [&](const ArtifactRef& existing) {
            return same_artifact(existing, artifact);
        })) {
        artifacts.push_back(artifact);
    }
}

void append_unique_artifacts(std::vector<ArtifactRef>& target,
                             const std::vector<ArtifactRef>& source) {
    for (const auto& artifact : source)
        append_unique_artifact(target, artifact);
}

std::expected<void, std::string> merge_compile_usage(
        CompileUsage& target, const CompileUsage& source) {
    for (const auto& include_dir : source.include_dirs)
        append_unique_path(target.include_dirs, include_dir);
    for (const auto& define : source.defines)
        append_unique_string(target.defines, define);
    for (const auto& [logical_name, artifact] : source.modules) {
        auto [existing, inserted] =
            target.modules.emplace(logical_name, artifact);
        if (!inserted && !same_artifact(existing->second, artifact)) {
            return std::unexpected(
                "duplicate logical module name '" + logical_name +
                "' identifies both '" + existing->second.path.string() +
                "' and '" + artifact.path.string() + "'");
        }
    }
    return {};
}

void merge_link_requirements(LinkInterface& target,
                             const LinkInterface& source) {
    for (const auto& library : source.system_libraries)
        append_unique_string(target.system_libraries, library);
    for (const auto& framework : source.frameworks)
        append_unique_string(target.frameworks, framework);
}

ArtifactKind terminal_artifact_kind(MoidType type) {
    switch (type) {
        case MoidType::Lib:        return ArtifactKind::StaticLibrary;
        case MoidType::Dylib:      return ArtifactKind::SharedLibrary;
        case MoidType::Executable: return ArtifactKind::Executable;
    }
    return ArtifactKind::Object;
}

} // anonymous namespace

// ===== Configure stage: build.cpp + bake.toml → MoidDeclaration =====
//
// Step 2 of the pipeline (resolve → configure → build → execute):
// for each moid, obtain its input description from default discovery
// or build.cpp, merge the authoritative bake.toml configuration, and
// persist one MoidDeclaration JSON under out/.bake/.

namespace {

Path find_bake_build_source() {
  // 1. Source-tree layout (dev builds): <ws>/lib/bake/bake.build.cppm
  Path ws = find_workspace_root();
  if (!ws.string().empty()) {
    Path p = ws / "lib" / "bake" / "bake.build.cppm";
    if (p.is_regular_file())
      return p;
  }
  // 2. Installed layout: <prefix>/lib/bake/bake.build.cppm
  Path exe(get_self_exe_path());
  if (!exe.string().empty()) {
    Path prefix = exe.parent().parent();
    Path installed = prefix / "lib" / "bake" / "bake.build.cppm";
    if (installed.is_regular_file())
      return installed;
    // 3. <prefix>/share/bake/bake.build.cppm (legacy)
    Path shared = prefix / "share" / "bake" / "bake.build.cppm";
    if (shared.is_regular_file())
      return shared;
  }
  return {};
}

class ScopedEnv {
public:
  ScopedEnv(std::string name, const std::string &value)
      : name_(std::move(name)) {
    if (const char *previous = std::getenv(name_.c_str()))
      previous_ = previous;
#ifdef _WIN32
    active_ = _putenv_s(name_.c_str(), value.c_str()) == 0;
#else
    active_ = ::setenv(name_.c_str(), value.c_str(), 1) == 0;
#endif
  }
  ScopedEnv(const ScopedEnv &) = delete;
  ScopedEnv &operator=(const ScopedEnv &) = delete;
  ~ScopedEnv() {
    if (!active_)
      return;
#ifdef _WIN32
    (void)_putenv_s(name_.c_str(), previous_ ? previous_->c_str() : "");
#else
    if (previous_)
      (void)::setenv(name_.c_str(), previous_->c_str(), 1);
    else
      (void)::unsetenv(name_.c_str());
#endif
  }

private:
  std::string name_;
  std::optional<std::string> previous_;
  bool active_ = false;
};

// Serialise active feature names as repeated name-length:name records.
std::string serialize_features(const std::vector<std::string> &features) {
  std::string out;
  for (const auto &name : features)
    out += std::to_string(name.size()) + ":" + name;
  return out;
}

json::Value
declaration_features_json(const std::vector<std::string> &features) {
  return json::Value(features);
}

std::string
serialize_declaration_dependencies(const std::vector<MoidEdge> &edges) {
  json::Value dependencies = json::Value::array();
  for (const auto &edge : edges) {
    json::Value dep_features = json::Value::array();
    for (const auto &feature : edge.features)
      dep_features.push_back(feature);
    dependencies.push_back({
        {"alias", edge.alias},
        {"id", edge.target.value},
        {"features", dep_features},
    });
  }
  return dependencies.dump();
}
} // namespace

// ===== build.cpp → MoidDeclaration =====
//
// Compiles build.cpp + bake.build.cppm into a small executable. The script
// persists its declaration at BAKE_DECLARATION_PATH, which is then read through
// the same strict codec used for every input declaration.

MoidDeclaration compile_and_run_build_cpp(const Path &moid_dir,
                                          const Manifest &manifest,
                                          const MoidNode &node,
                                          const TargetSpec& target,
                                          const Path &out_dir) {

  const std::string identity_key = SHA256::hex(node.id.value).substr(0, 24);

  // build.cpp runs on the host — always use a native toolchain.
  TargetSpec native_target = detect_host_target();

  // The std module is a compiler concern: `bake c++` provisions and
  // injects it for any -std=c++23 compile (see driver.cppm). The build
  // system deliberately knows nothing about it.


  // Project-local scripts dir: only build.o and build_app live here.
  Path scripts_dir = out_dir / ".bake" / "scripts" / identity_key;
  scripts_dir.mkdir_recursive();

  Path wrapper_src = find_bake_build_source();
  if (wrapper_src.string().empty()) {
    std::println(std::cerr, "bake: cannot find bake.build.cppm");
    std::exit(1);
  }

  // Global cache dir for bake.build.pcm + bake.build.o.
  auto cache_info = bake_build_cache_info(native_target, wrapper_src);
  Path build_cache_dir = cache_info.dir / "bake.build";
  build_cache_dir.mkdir_recursive();

  // Copy wrapper into the cache dir (so it's self-contained and the
  // -I flag resolves correctly for the compiler).
  Path wrapper_dst = build_cache_dir / "bake.build.cppm";
  if (auto content = read_file(wrapper_src))
    write_file(wrapper_dst, *content);

  // (std module flags are injected by the bake c++ shim itself.)


  // Step 1: Compile bake.build.cppm → PCM + .o  (global cache)
  Path pcm = build_cache_dir / "bake.build.pcm";
  Path wrapper_o = build_cache_dir / "bake.build.o";

  if (!pcm.is_regular_file() || !wrapper_o.is_regular_file()) {
    // Atomic compile: write to temp names, then rename into place.
    std::string pid = std::to_string(
#ifdef _WIN32
        _getpid()
#else
        getpid()
#endif
    );
    Path tmp_pcm = Path(pcm.string() + "." + pid + ".tmp");
    Path tmp_o = Path(wrapper_o.string() + "." + pid + ".tmp");

    std::vector<std::string> cmd;
    cmd.push_back(bake_exe_path());
    cmd.push_back("c++");
    cmd.push_back("-c");
    cmd.push_back("-std=c++23");
    cmd.push_back("-stdlib=libc++");
    cmd.push_back("-Wno-reserved-module-identifier");
    cmd.push_back("-x");
    cmd.push_back("c++-module");
    cmd.push_back("-I" + build_cache_dir.string());

    cmd.push_back("-fmodule-output=" + tmp_pcm.string());
    cmd.push_back(wrapper_dst.string());
    cmd.push_back("-o");
    cmd.push_back(tmp_o.string());

    auto result = run_process(cmd, moid_dir, true);
    if (!result.success()) {
      std::print(std::cerr, "{}", result.stderr_output);
      std::println(std::cerr, "bake: failed to compile bake.build.cppm");
      tmp_pcm.remove();
      tmp_o.remove();
      std::exit(1);
    }

    std::error_code ec;
    std::filesystem::rename(tmp_pcm.string(), pcm.string(), ec);
    std::filesystem::rename(tmp_o.string(), wrapper_o.string(), ec);
    // If rename failed because another process won the race, the
    // final files exist and we're fine.
  }

  // Step 2: Compile build.cpp → .o  (project-local)
  Path build_cpp = moid_dir / "build.cpp";
  Path build_o = scripts_dir / "build.o";
  {
    std::vector<std::string> cmd;
    cmd.push_back(bake_exe_path());
    cmd.push_back("c++");
    cmd.push_back("-c");
    cmd.push_back("-std=c++23");
    cmd.push_back("-stdlib=libc++");
    cmd.push_back("-Wno-reserved-module-identifier");
    cmd.push_back("-I" + build_cache_dir.string());

    cmd.push_back("-fmodule-file=bake.build=" + pcm.string());
    cmd.push_back(build_cpp.string());
    cmd.push_back("-o");
    cmd.push_back(build_o.string());

    auto result = run_process(cmd, moid_dir, true);
    if (!result.success()) {
      std::print(std::cerr, "{}", result.stderr_output);
      std::println(std::cerr, "bake: failed to compile build.cpp");
      std::exit(1);
    }
  }

  // Step 3: Link → build_app (project-local; only needs std, no bake library)
  // libc++ injection is handled by the driver (bakeExecuteJob).
  Path build_app = scripts_dir / "build_app";
  {
    std::vector<std::string> cmd;
    cmd.push_back(bake_exe_path());
    cmd.push_back("c++");
    cmd.push_back(wrapper_o.string());
    cmd.push_back(build_o.string());
    cmd.push_back("-o");
    cmd.push_back(build_app.string());

    auto result = run_process(cmd, moid_dir, true);
    if (!result.success()) {
      std::print(std::cerr, "{}", result.stderr_output);
      std::println(std::cerr, "bake: failed to link build_app");
      std::exit(1);
    }
  }

  // Step 4: Run build_app, then read its persisted declaration.
  const auto &features = node.declaration.active_features;
  std::string features_str = serialize_features(features);
  std::string declaration_features =
      declaration_features_json(features).dump();
  std::string declaration_dependencies =
      serialize_declaration_dependencies(node.dependencies);
  std::string deps_str;
  for (auto &[dep_name, dep] : manifest.dependencies) {
    if (dep.is_path_dep) {
      Path dep_dir = (moid_dir / dep.path).lexically_normal();
      deps_str += dep_name + "=" + dep_dir.absolute().string() + "\n";
    }
  }
  // Non-moid source deps resolved by the graph (remote archives, raw
  // path dirs) — their source directories are needed by build.cpp.
  for (auto &[alias, dir] : node.source_deps) {
    deps_str += alias + "=" + dir + "\n";
  }

  Path declaration_path = moid_declaration_path(out_dir, node.id.value);
  if (declaration_path.exists())
    declaration_path.remove();

  std::string self = get_self_exe_path();
  {
    ScopedEnv source_env("BAKE_SOURCE_DIR", node.declaration.root);
    ScopedEnv build_env("BAKE_BUILD_DIR", out_dir.absolute().string());
    ScopedEnv id_env("BAKE_MOID_ID", node.id.value);
    ScopedEnv name_env("BAKE_MOID_NAME", manifest.moid->name);
    ScopedEnv version_env("BAKE_MOID_VERSION", manifest.moid->version);
    ScopedEnv type_env("BAKE_MOID_TYPE",
                       std::string(moid_type_str(manifest.moid->type)));
    ScopedEnv cxx_std_env("BAKE_MOID_CXX_STD", manifest.moid->cxx_std);
    ScopedEnv c_std_env("BAKE_MOID_C_STD", manifest.moid->c_std);
    ScopedEnv declaration_env("BAKE_DECLARATION_PATH",
                              declaration_path.string());
    ScopedEnv features_env("BAKE_FEATURES", features_str);
    ScopedEnv declaration_features_env("BAKE_DECLARATION_FEATURES",
                                       declaration_features);
    ScopedEnv declaration_dependencies_env("BAKE_DECLARATION_DEPENDENCIES",
                                           declaration_dependencies);
    ScopedEnv dependencies_env("BAKE_DEPS", deps_str);
    ScopedEnv executable_env("BAKE_EXE", self);
    ScopedEnv target_env(
        "BAKE_TARGET", target.is_native()
                           ? detect_host_target().triple_
                           : target.triple());

    // Do not capture stdout/stderr: declaration transport uses the file,
    // so build scripts retain their normal user-facing streams.
    auto result = run_process({build_app.string()}, moid_dir, false);
    if (!result.success()) {
      std::println(std::cerr, "bake: build_app failed");
      std::exit(1);
    }

    auto declaration = read_moid_declaration(declaration_path);
    if (!declaration) {
      std::println(std::cerr, "bake: {}", declaration.error());
      std::exit(1);
    }
    return std::move(*declaration);
  }
}

namespace {

// Merge bake.toml configuration (profile, target conditions, options, link)
// into a declaration. Called after obtaining the input description from
// either default discovery or build.cpp.
void merge_manifest_config(MoidDeclaration &decl, const Manifest &manifest,
                           const TargetSpec& target,
                           const std::string &profile_name) {
  // Language standards — manifest is the sole source of truth.
  decl.cxx_std = manifest.moid->cxx_std;
  decl.c_std = manifest.moid->c_std;

  // Profile → flags / link_flags / defines
  auto profile = manifest.resolve_profile(profile_name);
  bool is_release = (profile_name == "release");
  auto resolved = resolve_profile_flags(profile, is_release, target);

  decl.compile_flags = resolved.compile_flags;
  decl.link_flags = resolved.link_flags;

  // Target conditions (specificity-sorted: broad first, exact last)
  std::string current_triple =
      target.is_native() ? detect_host_target().triple_ : target.triple_;

  struct TargetMatch {
    const TargetCondition *cond;
    int wc;
  };
  std::vector<TargetMatch> matches;
  for (const auto &tgt : manifest.targets)
    if (triple_matches(current_triple, tgt.triple_pattern))
      matches.push_back(
          {&tgt, static_cast<int>(std::count(tgt.triple_pattern.begin(),
                                             tgt.triple_pattern.end(), '*'))});

  std::sort(
      matches.begin(), matches.end(),
      [](const TargetMatch &a, const TargetMatch &b) { return a.wc > b.wc; });

  std::vector<std::string> tgt_flags, tgt_libs, tgt_frameworks, tgt_defines,
      tgt_public_defines, tgt_includes;
  for (const auto &match : matches) {
    for (auto &f : match.cond->flags)
      tgt_flags.push_back(f);
    for (auto &l : match.cond->libraries)
      tgt_libs.push_back(l);
    for (auto &d : match.cond->defines)
      tgt_defines.push_back(d);
    for (auto &d : match.cond->public_defines)
      tgt_public_defines.push_back(d);
    for (auto &i : match.cond->include_dirs)
      tgt_includes.push_back(i);
    if (target.is_darwin())
      for (auto &f : match.cond->frameworks)
        tgt_frameworks.push_back(f);
  }
  for (auto &f : tgt_flags)
    decl.compile_flags.push_back(f);

  // Defines: auto macros + feature/target defines, split by visibility.
  // Everything applies to this moid's own translation units; only
  // `public_defines` entries and the informational BAKE_* auto macros
  // propagate to consumers (public_compile_defines → CompileUsage).
  decl.compile_defines.clear();
  decl.public_compile_defines.clear();
  auto add_define =
      [](std::vector<std::pair<std::string, std::string>> &out,
         const std::string &raw) {
          auto equals = raw.find('=');
          out.push_back(equals == std::string::npos
                            ? std::pair{raw, std::string{}}
                            : std::pair{raw.substr(0, equals),
                                        raw.substr(equals + 1)});
      };
  for (auto &[k, v] : generate_feature_macros(decl.name, manifest.features,
                                              decl.active_features)) {
    decl.compile_defines.push_back({k, v});
    decl.public_compile_defines.push_back({k, v});
  }
  for (const auto& feature_name : decl.active_features) {
    auto feature = manifest.features.find(feature_name);
    if (feature == manifest.features.end()) continue;
    for (const auto& define : feature->second.defines)
      add_define(decl.compile_defines, define);
    for (const auto& define : feature->second.public_defines) {
      add_define(decl.compile_defines, define);
      add_define(decl.public_compile_defines, define);
    }
  }
  for (auto &[k, v] : generate_package_macros(decl.name, decl.version)) {
    decl.compile_defines.push_back({k, v});
    decl.public_compile_defines.push_back({k, v});
  }
  // Profile defines (e.g. NDEBUG) describe this moid's own compile mode.
  for (auto &[k, v] : resolved.defines)
    decl.compile_defines.push_back({k, v});
  for (auto &d : tgt_defines)
    add_define(decl.compile_defines, d);
  for (auto &d : tgt_public_defines) {
    add_define(decl.compile_defines, d);
    add_define(decl.public_compile_defines, d);
  }

  for (auto &i : tgt_includes)
    decl.extra_include_dirs.push_back(i);

  // Libraries + frameworks: [link] + matching [target.*]
  decl.libraries.clear();
  decl.frameworks.clear();
  if (manifest.link) {
    for (auto &l : manifest.link->libraries)
      decl.libraries.push_back(l);
    for (auto &f : manifest.link->frameworks)
      decl.frameworks.push_back(f);
  }
  for (auto &l : tgt_libs)
    decl.libraries.push_back(l);
  for (auto &f : tgt_frameworks)
    decl.frameworks.push_back(f);
}

std::expected<void, std::string>
validate_resolved_declaration(const MoidDeclaration &declaration,
                              const MoidNode &node) {
  // The manifest is authoritative for type and name.
  // build.cpp cannot override these.
  if (declaration.type != node.declaration.type) {
    return std::unexpected("build.cpp declared type '" +
                           std::string(moid_type_str(declaration.type)) +
                           "' but bake.toml declares type '" +
                           std::string(moid_type_str(node.declaration.type)) +
                           "' for moid '" + node.declaration.name + "'");
  }
  if (declaration.name != node.declaration.name) {
    return std::unexpected("build.cpp declared name '" + declaration.name +
                           "' but bake.toml declares name '" +
                           node.declaration.name + "' for moid '" +
                           node.declaration.name + "'");
  }
  if (declaration.id != node.id.value) {
    return std::unexpected("moid declaration field 'id' does not match "
                           "resolved identity for moid '" +
                           node.declaration.name + "'");
  }
  if (declaration.root != node.declaration.root) {
    return std::unexpected("moid declaration field 'root' does not match "
                           "resolved root for moid '" +
                           node.declaration.name + "'");
  }
  if (declaration.version != node.declaration.version) {
    return std::unexpected("moid declaration field 'version' does not match "
                           "resolved version for moid '" +
                           node.declaration.name + "'");
  }
  if (declaration.active_features != node.declaration.active_features) {
    return std::unexpected("moid declaration field 'features' does not match "
                           "resolved features for moid '" +
                           node.declaration.name + "'");
  }
  if (declaration.dependencies.size() != node.declaration.dependencies.size()) {
    return std::unexpected("moid declaration field 'dependencies' does not "
                           "match resolved dependencies for moid '" +
                           node.declaration.name + "'");
  }
  for (std::size_t i = 0; i < declaration.dependencies.size(); ++i) {
    const auto &actual = declaration.dependencies[i];
    const auto &expected = node.declaration.dependencies[i];
    if (actual.alias != expected.alias || actual.id != expected.id ||
        actual.features != expected.features) {
      return std::unexpected("moid declaration field 'dependencies' does not "
                             "match resolved dependencies for moid '" +
                             node.declaration.name + "'");
    }
  }
  return {};
}

} // namespace

export std::expected<void, std::string>
configure_moid_graph(MoidGraph &graph, const TargetSpec& target, const Path &out_dir,
                     const Path &project_root,
                     const std::string &profile_name) {

  auto topology = configuration_topological_moids(graph);
  if (!topology)
    return std::unexpected(topology.error());

  (out_dir / ".bake").mkdir_recursive();

  for (const auto &id : *topology) {
    auto &node = graph.nodes.at(id);
    const Path moid_dir(node.declaration.root);

    // 1. Load manifest (single source of configuration truth)
    auto manifest = Manifest::load(moid_dir);
    if (!manifest || !manifest->has_moid()) {
      return std::unexpected("failed to load manifest for moid '" +
                             node.declaration.name + "'");
    }

    // 2. Get input description (sources, public headers, prebuilt libs)
    MoidDeclaration declaration;
    if ((moid_dir / "build.cpp").is_regular_file()) {
      // Cache: skip build.cpp compilation if inputs unchanged
      Path decl_path = moid_declaration_path(out_dir, node.id.value);
      bool cache_hit = false;
      if (decl_path.is_regular_file()) {
        namespace fs = std::filesystem;
        auto decl_time = fs::last_write_time(decl_path.fs());
        auto inputs_older = [&]() {
          if (fs::last_write_time((moid_dir / "build.cpp").fs()) > decl_time)
            return false;
          Path toml = moid_dir / "bake.toml";
          if (toml.is_regular_file() &&
              fs::last_write_time(toml.fs()) > decl_time)
            return false;
          Path wrapper = find_bake_build_source();
          if (wrapper.is_regular_file() &&
              fs::last_write_time(wrapper.fs()) > decl_time)
            return false;
          return true;
        };
        if (inputs_older()) {
          if (auto cached = read_moid_declaration(decl_path)) {
            if (validate_resolved_declaration(*cached, node)) {
              declaration = std::move(*cached);
              cache_hit = true;
            }
          }
        }
      }
      if (!cache_hit)
        declaration =
            compile_and_run_build_cpp(moid_dir, *manifest, node, target, out_dir);
    } else {
      // Default: scan src/ + public/
      auto layout = Layout::detect(moid_dir);
      declaration = declare_default_inputs(*manifest, layout, node.id.value,
                                           node.declaration.active_features,
                                           node.declaration.dependencies);
    }

    // Incremental mode: a build.cpp that describes no main-moid inputs
    // (only binaries, tests, or prebuilt libs) keeps default input
    // discovery for the main moid.
    if ((moid_dir / "build.cpp").is_regular_file() &&
        declaration.sources.empty() &&
        declaration.public_include_dirs.empty()) {
      auto layout = Layout::detect(moid_dir);
      auto discovered = declare_default_inputs(
          *manifest, layout, node.id.value,
          node.declaration.active_features,
          node.declaration.dependencies);
      declaration.sources = std::move(discovered.sources);
      declaration.public_include_dirs =
          std::move(discovered.public_include_dirs);
    }

    // 3. Merge bake.toml configuration
    merge_manifest_config(declaration, *manifest, target, profile_name);

    // 4. Validate
    auto validated = validate_resolved_declaration(declaration, node);
    if (!validated)
      return std::unexpected(validated.error());

    // 5. Persist (once — after merge)
    Path decl_path = moid_declaration_path(out_dir, node.id.value);
    auto written = write_moid_declaration(decl_path, declaration);
    if (!written)
      return std::unexpected(written.error());

    node.declaration = std::move(declaration);

    // 5b. A declared source dependency nothing consumes was downloaded and
    // locked for nothing — default discovery has no way to use one, and a
    // build.cpp that never queries dep_src_dir() left it idle.
    if (!node.source_deps.empty()) {
      for (const auto& [alias, dir] : node.source_deps) {
        (void)dir;
        if (std::ranges::find(node.declaration.used_source_deps, alias) ==
            node.declaration.used_source_deps.end()) {
          std::println(std::cerr,
                       "bake: warning: '{}' declares source dependency '{}' "
                       "that is never consumed (query it in build.cpp via "
                       "dep_src_dir)",
                       node.declaration.name, alias);
        }
      }
    }

    // Executable moids cannot be dependencies
    if (node.declaration.type != MoidType::Executable)
      continue;
    for (const auto &[_, consumer] : graph.nodes) {
      for (const auto &edge : consumer.dependencies) {
        if (edge.target != id)
          continue;
        return std::unexpected("moid '" + consumer.declaration.name +
                               "' cannot use executable moid '" +
                               node.declaration.name +
                               "' as a normal dependency");
      }
    }
  }

  return {};
}
export std::expected<BuildGraph, std::string> build_graph(
        const MoidGraph& outer_graph,
        const TargetSpec& target,
        const Path& out_dir,
        const Path& project_root) {

    auto topology = topological_moids(outer_graph);
    if (!topology) return std::unexpected(topology.error());

    BuildGraph graph;
    graph.state_dir = out_dir / ".bake";
    graph.project_root = project_root;

    (out_dir / ".obj").mkdir_recursive();
    (out_dir / ".bmi").mkdir_recursive();

    (out_dir / "bin").mkdir_recursive();
    (out_dir / "lib").mkdir_recursive();
    graph.state_dir.mkdir_recursive();

    struct TerminalClaim {
        std::string canonical_id;
        std::string display_name;
    };
    std::map<std::string, TerminalClaim> terminal_claims;

    // ── Resolve declarations into per-moid data ──

    struct ResolvedMoid {
        const MoidDeclaration* decl;
        std::string canonical_id;
        std::string storage_key;
        MoidType type = MoidType::Executable;
        Path source_dir;
        Path obj_dir;
        Path bmi_dir;
        Path output;
        std::vector<Path> cpp_files;
        std::vector<Path> c_files;
        std::vector<Path> module_interfaces;
        CompileUsage own_compile;
        std::set<std::string> public_module_sources;
        bool uses_cxx = false;
    };

    std::map<std::string, ResolvedMoid> moids;
    std::map<std::string, MoidExports> moid_exports;
    std::map<std::string, std::size_t> action_indices;
    std::vector<std::string> moid_order;
    std::map<std::string, std::string> storage_owners;
    // Memoized local-header closures (see HeaderClosureScanner).
    HeaderClosureScanner header_scanner;

    // Resolves one declaration (main moid or extra binary) into the graph:
    // expands sources, validates the terminal name, claims the output path.
    auto resolve_into_graph = [&](const std::string& canonical_id,
                                  const MoidDeclaration& decl)
            -> std::expected<void, std::string> {
        ResolvedMoid rm;
        rm.decl = &decl;
        rm.canonical_id = canonical_id;
        rm.storage_key = moid_storage_key(canonical_id);
        auto [storage, storage_inserted] =
            storage_owners.emplace(rm.storage_key, canonical_id);
        if (!storage_inserted && storage->second != canonical_id) {
            return std::unexpected(
                "canonical moid storage key collision between '" +
                storage->second + "' and '" + canonical_id + "'");
        }

        rm.source_dir = Path(decl.root);
        rm.type = decl.type;
        rm.obj_dir = out_dir / ".obj" / rm.storage_key;
        rm.bmi_dir = out_dir / ".bmi" / rm.storage_key;

        std::map<std::string, const SourceGroup*> source_groups;
        for (const auto& group : decl.sources) {
            Path source = resolve_path(group.pattern, rm.source_dir);
            std::error_code source_ec;
            auto canonical_source =
                std::filesystem::weakly_canonical(source.fs(), source_ec);
            if (!source_ec) source = Path(canonical_source);
            const std::string source_key = source.string();
            auto [existing, inserted] = source_groups.emplace(source_key, &group);
            if (!inserted) {
                const auto& previous = *existing->second;
                if (previous.is_public != group.is_public) {
                    return std::unexpected(
                        "conflicting source groups for '" +
                        normalized_source_id(rm.source_dir, source) +
                        "' in moid '" + decl.name + "'");
                }
                continue;
            }

            // Explicit public module declarations (b.public_modules) own
            // their interface role: the pattern may name any C++ source —
            // upstream module TUs don't always use module extensions
            // (fmt ships src/fmt.cc). Discovery only marks extension-
            // classified interfaces public, so this cannot misfire there.
            if (source.is_module_interface() || group.is_public) {
                rm.module_interfaces.push_back(source);
                rm.uses_cxx = true;
                if (group.is_public)
                    rm.public_module_sources.insert(source_key);
            } else if (source.is_c()) {
                rm.c_files.push_back(source);
            } else {
                rm.cpp_files.push_back(source);
                rm.uses_cxx = true;
            }
        }

        for (const auto& include_dir : decl.public_include_dirs) {
            append_unique_path(
                rm.own_compile.include_dirs,
                resolve_path(include_dir, rm.source_dir));
        }

        // Propagate public defines to consumers via CompileUsage. Private
        // defines stay confined to this moid's own translation units.
        for (const auto& [name, value] : decl.public_compile_defines) {
            std::string define = value.empty() ? name : name + "=" + value;
            append_unique_string(rm.own_compile.defines, define);
        }

        rm.uses_cxx = rm.uses_cxx || !is_c_standard(decl.cxx_std);

        const bool has_sources = !rm.cpp_files.empty() ||
            !rm.c_files.empty() || !rm.module_interfaces.empty();
        if (rm.type == MoidType::Executable && !has_sources) {
            return std::unexpected(
                "executable moid '" + decl.name + "' has no sources");
        }

        // Lib produces a terminal archive only when it has object-producing
        // sources; a header-only Lib has no terminal.
        const bool has_terminal = rm.type != MoidType::Lib || has_sources;
        if (has_terminal) {
            const std::filesystem::path declared_name(decl.name);
            if (decl.name.empty() || decl.name == "." || decl.name == ".." ||
                decl.name.find('\0') != std::string::npos ||
                decl.name.find('\\') != std::string::npos ||
                declared_name.is_absolute() || declared_name.has_root_name() ||
                declared_name.has_parent_path() ||
                declared_name.filename() != declared_name) {
                return std::unexpected(
                    "invalid terminal output name '" + decl.name +
                    "' for moid '" + canonical_id +
                    "': expected a single filename component");
            }
            if (!is_portable_name(decl.name)) {
                return std::unexpected(
                    "invalid terminal output name '" + decl.name +
                    "' for moid '" + canonical_id +
                    "': expected a portable ASCII name matching "
                    "[A-Za-z0-9][A-Za-z0-9._+@-]* without a trailing dot or "
                    "reserved device name");
            }

            std::string out_name = library_name(decl.name, rm.type, target);
            rm.output = (rm.type == MoidType::Executable)
                ? out_dir / "bin" / out_name
                : out_dir / "lib" / out_name;

            auto relative_output = terminal_relative_path(out_dir, rm.output);
            if (!relative_output) {
                return std::unexpected(
                    "terminal output '" + rm.output.string() +
                    "' is not contained by the output directory");
            }
            const std::string output_key =
                portable_terminal_key(*relative_output);
            TerminalClaim owner{canonical_id, decl.name};
            auto [claim, inserted] =
                terminal_claims.emplace(output_key, owner);
            if (!inserted && claim->second.canonical_id != canonical_id) {
                TerminalClaim left = claim->second;
                TerminalClaim right = std::move(owner);
                if (right.canonical_id < left.canonical_id)
                    std::swap(left, right);
                return std::unexpected(
                    "terminal output collision at '" + rm.output.string() +
                    "' between moid '" + left.display_name + "' (" +
                    left.canonical_id + ") and moid '" +
                    right.display_name + "' (" + right.canonical_id + ")");
            }
        }

        rm.obj_dir.mkdir_recursive();
        rm.bmi_dir.mkdir_recursive();
        if (has_terminal)
            rm.output.parent().mkdir_recursive();

        moids.emplace(canonical_id, std::move(rm));
        moid_exports.try_emplace(canonical_id);
        moid_order.push_back(canonical_id);
        return {};
    };

    for (const auto& id : *topology) {
        auto resolved = resolve_into_graph(
            id.value, outer_graph.nodes.at(id).declaration);
        if (!resolved) return std::unexpected(resolved.error());
    }

    // ── Extra binaries: synthetic executable moids that depend on the moid
    // declaring them. Each binary inherits the merged configuration and the
    // declaring moid's public usage through the normal dependency mechanism.
    std::map<std::string, MoidDeclaration> binary_declarations;
    for (std::size_t index = 0; index < moid_order.size(); ++index) {
        // Index-based: binaries appended below must not be visited as
        // declaring moids.
        const std::string main_id = moid_order[index];
        const MoidDeclaration& decl = *moids.at(main_id).decl;
        // Binaries belong to the moid being built: dependencies pulled in
        // from the graph never produce their binaries (Cargo semantics —
        // build the port itself to get its tools).
        if (!decl.binaries.empty() && decl.type == MoidType::Executable) {
            return std::unexpected(
                "binary declarations require moid '" + decl.name +
                "' to be a lib or dylib, not an executable");
        }
        if (std::find_if(outer_graph.roots.begin(), outer_graph.roots.end(),
                         [&](const MoidId& id) {
                             return id.value == main_id;
                         }) == outer_graph.roots.end())
            continue;

        for (const auto& binary : decl.binaries) {
            const std::string synthetic_id =
                "binary:" + main_id + ":" + binary.name;
            MoidDeclaration& synthetic = binary_declarations[synthetic_id];
            synthetic = decl;
            synthetic.id = synthetic_id;
            synthetic.name = binary.name;
            synthetic.type = MoidType::Executable;
            synthetic.sources = binary.sources;
            synthetic.public_include_dirs.clear();
            synthetic.extra_include_dirs = binary.include_dirs;
            synthetic.extra_include_dirs.insert(
                synthetic.extra_include_dirs.end(),
                decl.extra_include_dirs.begin(),
                decl.extra_include_dirs.end());
            synthetic.libraries.clear();
            synthetic.frameworks.clear();
            synthetic.prebuilt_libs.clear();
            synthetic.dependencies = {MoidDependency{"", main_id, {}}};
            synthetic.binaries.clear();
            synthetic.tests.clear();

            auto resolved = resolve_into_graph(synthetic_id, synthetic);
            if (!resolved) return std::unexpected(resolved.error());
        }
    }

    // Dependencies precede consumers in moid_order. Merge each reachable
    // Moid's own public usage once in that canonical order, then own usage.
    auto recompute_compile_exports = [&]()
            -> std::expected<void, std::string> {
        for (const auto& id : moid_order) {
            auto& rm = moids.at(id);
            std::set<std::string> reachable;
            std::vector<std::string> pending;
            for (const auto& dependency : rm.decl->dependencies)
                pending.push_back(dependency.id);
            while (!pending.empty()) {
                std::string dependency_id = std::move(pending.back());
                pending.pop_back();
                if (!moids.contains(dependency_id)) {
                    return std::unexpected(
                        "moid '" + rm.decl->name +
                        "' targets missing moid '" + dependency_id + "'");
                }
                if (!reachable.insert(dependency_id).second) continue;
                for (const auto& dependency :
                     moids.at(dependency_id).decl->dependencies) {
                    pending.push_back(dependency.id);
                }
            }

            CompileUsage usage;
            for (const auto& dependency_id : moid_order) {
                if (!reachable.contains(dependency_id)) continue;
                auto merged = merge_compile_usage(
                    usage, moids.at(dependency_id).own_compile);
                if (!merged) return std::unexpected(merged.error());
            }
            auto merged = merge_compile_usage(usage, rm.own_compile);
            if (!merged) return std::unexpected(merged.error());
            moid_exports.at(id).compile = std::move(usage);
        }
        return {};
    };

    auto compile_exports = recompute_compile_exports();
    if (!compile_exports)
        return std::unexpected(compile_exports.error());

    auto compile_include_dirs = [&](const ResolvedMoid& moid,
                                    const Path& source) {
        std::vector<Path> result =
            moid_exports.at(moid.canonical_id).compile.include_dirs;
        // Add moid-level extra include dirs from target conditions
        for (const auto& dir : moid.decl->extra_include_dirs) {
            append_unique_path(result, resolve_path(dir, moid.source_dir));
        }
        return result;
    };

    auto compile_defines = [&](const ResolvedMoid& moid,
                               const Path& source) {
        std::vector<std::pair<std::string, std::string>> result;
        auto append_define = [&](const std::string& name, const std::string& value) {
            if (std::find(result.begin(), result.end(), std::pair{name, value}) == result.end())
                result.emplace_back(name, value);
        };
        // Transitive public defines from dependencies
        for (const auto& definition :
             moid_exports.at(moid.canonical_id).compile.defines) {
            auto equals = definition.find('=');
            if (equals != std::string::npos)
                append_define(definition.substr(0, equals), definition.substr(equals + 1));
            else
                append_define(definition, "");
        }
        // Moid-level defines (option macros, package macros, target defines, NDEBUG)
        for (const auto& [name, value] : moid.decl->compile_defines)
            append_define(name, value);
        return result;
    };

    // ── Module scan: cross-moid import resolution ──

    using ModuleKey = std::pair<std::string, std::string>;
    using ModuleProviderMap = std::map<std::string, ModuleKey>;

    struct ScannedModule {
        ModuleInfo info;
        bool is_public = false;
    };

    std::map<ModuleKey, ScannedModule> all_modules;
    std::map<std::string, ModuleProviderMap> own_module_providers;
    std::map<std::string, ModuleInfo> consumers;  // source_path → info

    for (const auto& moid_id : moid_order) {
        auto& rm = moids.at(moid_id);
        for (auto& src : rm.module_interfaces) {
            auto info = scan_module_file(src);
            if (!info || info->module_name.empty()) continue;

            ModuleKey key{moid_id, info->module_name};
            ScannedModule scanned{
                *info, rm.public_module_sources.contains(src.string())};
            auto [existing, inserted] =
                all_modules.emplace(key, std::move(scanned));
            if (!inserted &&
                existing->second.info.source_path != info->source_path) {
                return std::unexpected(
                    "duplicate logical module name '" +
                    info->module_name + "' in moid '" + rm.decl->name +
                    "' is provided by both '" +
                    existing->second.info.source_path + "' and '" +
                    info->source_path + "'");
            }
            own_module_providers[moid_id].emplace(info->module_name, key);
        }
        for (auto& src : rm.cpp_files) {
            auto info = scan_module_file(src);
            if (info && (!info->imports.empty() || !info->module_name.empty()))
                consumers[src.string()] = *info;
        }
    }

    auto merge_module_providers = [&](ModuleProviderMap& target,
                                      const ModuleProviderMap& source,
                                      std::string_view consumer)
            -> std::expected<void, std::string> {
        for (const auto& [logical_name, provider] : source) {
            auto [existing, inserted] =
                target.emplace(logical_name, provider);
            if (!inserted && existing->second != provider) {
                return std::unexpected(
                    "duplicate logical module name '" + logical_name +
                    "' in compile usage for moid '" +
                    std::string(consumer) + "' identifies both '" +
                    all_modules.at(existing->second).info.source_path +
                    "' and '" + all_modules.at(provider).info.source_path +
                    "'");
            }
        }
        return {};
    };

    // Public roots are sufficient to resolve imports while constructing the
    // provider DAG. Private imports remain visible only to their owning Moid.
    std::map<std::string, ModuleProviderMap> public_module_roots;
    for (const auto& moid_id : moid_order) {
        auto& rm = moids.at(moid_id);
        ModuleProviderMap roots;
        for (const auto& dependency : rm.decl->dependencies) {
            auto merged = merge_module_providers(
                roots, public_module_roots.at(dependency.id), rm.decl->name);
            if (!merged) return std::unexpected(merged.error());
        }
        for (const auto& [logical_name, provider] :
             own_module_providers[moid_id]) {
            if (!all_modules.at(provider).is_public) continue;
            auto merged = merge_module_providers(
                roots, ModuleProviderMap{{logical_name, provider}},
                rm.decl->name);
            if (!merged) return std::unexpected(merged.error());
        }
        public_module_roots.emplace(moid_id, std::move(roots));
    }

    auto resolve_provider = [&](const std::string& owner,
                                const std::string& logical_name)
            -> std::optional<ModuleKey> {
        auto own_owner = own_module_providers.find(owner);
        if (own_owner != own_module_providers.end()) {
            auto own = own_owner->second.find(logical_name);
            if (own != own_owner->second.end()) return own->second;
        }
        auto visible_owner = public_module_roots.find(owner);
        if (visible_owner != public_module_roots.end()) {
            auto visible = visible_owner->second.find(logical_name);
            if (visible != visible_owner->second.end()) return visible->second;
        }
        return std::nullopt;
    };

    std::map<ModuleKey, std::vector<ModuleKey>> module_dependencies;
    std::map<ModuleKey, std::vector<ModuleKey>> module_dependents;
    std::map<ModuleKey, std::size_t> module_in_degree;
    for (const auto& [key, scanned] : all_modules) {
        module_in_degree[key] = 0;
        for (const auto& imported : scanned.info.imports) {
            auto dependency = resolve_provider(key.first, imported);
            if (!dependency) continue;
            auto& dependencies = module_dependencies[key];
            if (std::find(dependencies.begin(), dependencies.end(),
                          *dependency) != dependencies.end()) {
                continue;
            }
            dependencies.push_back(*dependency);
            module_dependents[*dependency].push_back(key);
            ++module_in_degree[key];
        }
    }

    std::set<ModuleKey> ready_modules;
    for (const auto& [key, degree] : module_in_degree) {
        if (degree == 0) ready_modules.insert(key);
    }
    std::vector<ModuleKey> sorted_modules;
    while (!ready_modules.empty()) {
        ModuleKey key = *ready_modules.begin();
        ready_modules.erase(ready_modules.begin());
        sorted_modules.push_back(key);
        for (const auto& dependent : module_dependents[key]) {
            if (--module_in_degree[dependent] == 0)
                ready_modules.insert(dependent);
        }
    }
    if (sorted_modules.size() != all_modules.size()) {
        for (const auto& [key, _] : all_modules) {
            if (std::find(sorted_modules.begin(), sorted_modules.end(), key) ==
                sorted_modules.end()) {
                sorted_modules.push_back(key);
            }
        }
    }

    auto module_provider_closure = [&](std::vector<ModuleKey> roots) {
        std::set<ModuleKey> visited;
        while (!roots.empty()) {
            ModuleKey key = std::move(roots.back());
            roots.pop_back();
            if (!visited.insert(key).second) continue;
            auto dependencies = module_dependencies.find(key);
            if (dependencies == module_dependencies.end()) continue;
            roots.insert(roots.end(), dependencies->second.begin(),
                         dependencies->second.end());
        }
        return std::vector<ModuleKey>(visited.begin(), visited.end());
    };

    // Export each public module plus the private provider closure required to
    // consume its BMI. Diamonds retain the same ModuleKey and merge cleanly.
    std::map<std::string, ModuleProviderMap> own_exported_modules;
    std::map<std::string, ModuleProviderMap> exported_module_providers;
    for (const auto& moid_id : moid_order) {
        auto& rm = moids.at(moid_id);
        ModuleProviderMap exported;
        for (const auto& dependency : rm.decl->dependencies) {
            auto merged = merge_module_providers(
                exported, exported_module_providers.at(dependency.id),
                rm.decl->name);
            if (!merged) return std::unexpected(merged.error());
        }

        ModuleProviderMap own_exported;
        for (const auto& [logical_name, provider] :
             own_module_providers[moid_id]) {
            if (!all_modules.at(provider).is_public) continue;
            for (const auto& required :
                 module_provider_closure({provider})) {
                auto merged = merge_module_providers(
                    own_exported,
                    ModuleProviderMap{{required.second, required}},
                    rm.decl->name);
                if (!merged) return std::unexpected(merged.error());
            }
        }
        auto merged = merge_module_providers(
            exported, own_exported, rm.decl->name);
        if (!merged) return std::unexpected(merged.error());
        own_exported_modules.emplace(moid_id, std::move(own_exported));
        exported_module_providers.emplace(moid_id, std::move(exported));
    }

    // Shared across compile and link: each object retains its typed producer.
    std::map<std::string, std::vector<ArtifactRef>> moid_objects;

    // ── Compile module interfaces (topological order) ──

    struct ModuleActionInfo {
        ArtifactRef module;
        std::size_t action_idx;
    };
    std::map<ModuleKey, ModuleActionInfo> module_actions;

    for (const auto& module_key : sorted_modules) {
        const auto& info = all_modules.at(module_key).info;
        const std::string& moid_id = module_key.first;
        const std::string& mod_name = module_key.second;
        Path src(info.source_path);

        auto& rm = moids.at(moid_id);
        const std::string& moid_name = rm.decl->name;

        Path pcm = rm.bmi_dir / (mod_name + ".pcm");
        std::string aid = action_id_for("module", rm.storage_key, mod_name);
        Path obj = rm.obj_dir / (src.stem_string() + "_" +
                                  SHA256::hex(aid).substr(0, 12) + ".o");

        CompileConfig cc;
        cc.source = src;
        cc.output = obj;
        cc.std_ver = rm.decl->cxx_std;
        cc.include_dirs = compile_include_dirs(rm, src);
        cc.defines = compile_defines(rm, src);
        cc.is_module_interface = true;
        cc.bmi_output = pcm;
        cc.use_pic = (rm.type != MoidType::Executable);

        cc.extra_flags = rm.decl->compile_flags;

        for (const auto& dependency : module_provider_closure(
                 module_dependencies[module_key])) {
            auto action = module_actions.find(dependency);
            if (action != module_actions.end()) {
                cc.module_deps.push_back(
                    {dependency.second, action->second.module.path});
            }
        }

        BuildAction action;
        action.type = BuildAction::Type::CompileModule;
        action.id = aid;
        action.moid_id = rm.canonical_id;
        action.moid = moid_name;
        action.moid_version = rm.decl->version;
        action.description = normalized_source_id(rm.source_dir, src);
        action.inputs = {src};
        for (auto& header : header_scanner.closure(
                 src, cc.include_dirs, cc.extra_flags))
            append_unique_path(action.inputs, header);
        for (auto& [_, dep_pcm] : cc.module_deps)
            action.inputs.push_back(dep_pcm);
        action.outputs = {obj, pcm};
        action.command = make_compile_command(target, cc);

        for (const auto& dependency : module_dependencies[module_key]) {
            auto dependency_action = module_actions.find(dependency);
            if (dependency_action != module_actions.end()) {
                action.depends_on.push_back(
                    dependency_action->second.action_idx);
            }
        }

        std::size_t idx = graph.actions.size();
        graph.actions.push_back(std::move(action));
        action_indices.emplace(aid, idx);

        ArtifactRef module{ArtifactKind::Module, pcm, aid};
        module_actions[module_key] = {module, idx};

        // Module interface objects must be linked too.
        append_unique_artifact(
            moid_objects[moid_id],
            ArtifactRef{ArtifactKind::Object, obj, aid});
    }

    for (const auto& moid_id : moid_order) {
        auto& own_compile = moids.at(moid_id).own_compile;
        for (const auto& [logical_name, provider] :
             own_exported_modules[moid_id]) {
            const auto& artifact = module_actions.at(provider).module;
            auto [existing, inserted] =
                own_compile.modules.emplace(logical_name, artifact);
            if (!inserted && !same_artifact(existing->second, artifact)) {
                return std::unexpected(
                    "duplicate logical module name '" + logical_name + "'");
            }
        }
    }

    compile_exports = recompute_compile_exports();
    if (!compile_exports)
        return std::unexpected(compile_exports.error());

    // ── Compile regular sources ──

    for (const auto& moid_id : moid_order) {
        auto& rm = moids.at(moid_id);
        const std::string& moid_name = rm.decl->name;
        std::vector<Path> all_srcs = rm.cpp_files;
        all_srcs.insert(all_srcs.end(), rm.c_files.begin(), rm.c_files.end());

        for (auto& src : all_srcs) {
            std::string src_id = normalized_source_id(rm.source_dir, src);
            std::string aid = action_id_for("compile", rm.storage_key, src_id);
            Path obj = rm.obj_dir / (src.stem_string() + "_" +
                                      SHA256::hex(aid).substr(0, 12) + ".o");

            CompileConfig cc;
            cc.source = src;
            cc.output = obj;
            cc.std_ver = src.is_c() ? rm.decl->c_std : rm.decl->cxx_std;
            cc.include_dirs = compile_include_dirs(rm, src);
            cc.defines = compile_defines(rm, src);
            cc.use_pic = (rm.type != MoidType::Executable);

            cc.extra_flags = rm.decl->compile_flags;

            std::vector<ModuleKey> consumer_roots;
            auto consumer_it = consumers.find(src.string());
            if (consumer_it != consumers.end()) {
                for (const auto& imported : consumer_it->second.imports) {
                    auto provider = resolve_provider(moid_id, imported);
                    if (provider &&
                        std::find(consumer_roots.begin(), consumer_roots.end(),
                                  *provider) == consumer_roots.end()) {
                        consumer_roots.push_back(*provider);
                    }
                }
                for (const auto& provider :
                     module_provider_closure(consumer_roots)) {
                    auto module_action = module_actions.find(provider);
                    if (module_action == module_actions.end()) continue;
                    const ArtifactRef* artifact =
                        &module_action->second.module;
                    auto exported = moid_exports.at(moid_id)
                                        .compile.modules.find(provider.second);
                    if (exported !=
                            moid_exports.at(moid_id).compile.modules.end() &&
                        same_artifact(exported->second, *artifact)) {
                        artifact = &exported->second;
                    }
                    cc.module_deps.push_back(
                        {provider.second, artifact->path});
                }
            }


            BuildAction action;
            action.type = BuildAction::Type::Compile;
            action.id = aid;
            action.moid_id = rm.canonical_id;
            action.moid = moid_name;
            action.moid_version = rm.decl->version;
            action.description = normalized_source_id(rm.source_dir, src);
            action.inputs = {src};
            for (auto& header : header_scanner.closure(
                     src, cc.include_dirs, cc.extra_flags))
                append_unique_path(action.inputs, header);
            for (auto& [_, dep_pcm] : cc.module_deps)
                action.inputs.push_back(dep_pcm);
            action.outputs = {obj};
            action.command = make_compile_command(target, cc);

            for (const auto& provider :
                 module_provider_closure(consumer_roots)) {
                auto module_action = module_actions.find(provider);
                if (module_action != module_actions.end()) {
                    auto dep_idx = module_action->second.action_idx;
                    if (std::find(action.depends_on.begin(),
                                  action.depends_on.end(),
                                  dep_idx) == action.depends_on.end()) {
                        action.depends_on.push_back(dep_idx);
                    }
                }
            }

            std::size_t idx = graph.actions.size();
            graph.actions.push_back(std::move(action));
            action_indices.emplace(aid, idx);
            append_unique_artifact(
                moid_objects[moid_id],
                ArtifactRef{ArtifactKind::Object, obj, aid});
        }
    }

    // ── Link / archive: produce terminal artifacts ──

    auto add_artifact_inputs = [](BuildAction& action,
                                  const std::vector<ArtifactRef>& artifacts) {
        for (const auto& artifact : artifacts)
            append_unique_path(action.inputs, artifact.path);
    };

    auto add_producer_dependencies = [&](
            BuildAction& action,
            const std::vector<ArtifactRef>& artifacts)
            -> std::expected<void, std::string> {
        for (const auto& artifact : artifacts) {
            auto producer = action_indices.find(artifact.producer_action);
            if (producer == action_indices.end()) {
                return std::unexpected(
                    "artifact '" + artifact.path.string() +
                    "' references missing producer action '" +
                    artifact.producer_action + "'");
            }
            if (std::find(action.depends_on.begin(), action.depends_on.end(),
                          producer->second) == action.depends_on.end()) {
                action.depends_on.push_back(producer->second);
            }
        }
        return {};
    };

    auto paths_for = [](const std::vector<ArtifactRef>& artifacts) {
        std::vector<Path> paths;
        for (const auto& artifact : artifacts)
            append_unique_path(paths, artifact.path);
        return paths;
    };

    for (const auto& moid_id : moid_order) {
        auto& rm = moids.at(moid_id);
        auto& exports = moid_exports.at(moid_id);
        const std::string& moid_name = rm.decl->name;

        LinkInterface dependency_link;
        for (const auto& dependency : rm.decl->dependencies) {
            auto dependency_exports = moid_exports.find(dependency.id);
            if (dependency_exports == moid_exports.end()) {
                return std::unexpected(
                    "moid '" + moid_name + "' targets missing moid '" +
                    dependency.id + "'");
            }
            append_unique_artifacts(
                dependency_link.objects,
                dependency_exports->second.link.objects);
            append_unique_artifacts(
                dependency_link.libraries,
                dependency_exports->second.link.libraries);
            merge_link_requirements(
                dependency_link, dependency_exports->second.link);
            rm.uses_cxx = rm.uses_cxx || moids.at(dependency.id).uses_cxx;
        }

        // The outer order is dependency-first. Reorder the typed library set in
        // reverse so every consumer archive precedes the providers it needs.
        std::vector<ArtifactRef> ordered_libraries;
        for (auto it = moid_order.rbegin(); it != moid_order.rend(); ++it) {
            const auto& terminal = moid_exports.at(*it).terminal;
            if (!terminal) continue;
            if (std::ranges::any_of(
                    dependency_link.libraries,
                    [&](const ArtifactRef& candidate) {
                        return same_artifact(candidate, *terminal);
                    })) {
                append_unique_artifact(ordered_libraries, *terminal);
            }
        }
        append_unique_artifacts(
            ordered_libraries, dependency_link.libraries);
        dependency_link.libraries = std::move(ordered_libraries);

        LinkInterface requirements;
        merge_link_requirements(requirements, dependency_link);
        for (const auto& library : rm.decl->libraries)
            append_unique_string(requirements.system_libraries, library);
        for (const auto& framework : rm.decl->frameworks)
            append_unique_string(requirements.frameworks, framework);
        for (const auto& lib : rm.decl->prebuilt_libs)
            append_unique_string(requirements.system_libraries, lib);

        std::vector<ArtifactRef> command_objects = moid_objects[moid_id];
        append_unique_artifacts(command_objects, dependency_link.objects);

        // Header-only Lib (no object-producing sources): no terminal, just
        // pass through transitive link requirements to downstream consumers.
        if (rm.type == MoidType::Lib && command_objects.empty()) {
            exports.terminal.reset();
            exports.link.objects.clear();
            exports.link.libraries = dependency_link.libraries;
            exports.link.system_libraries = requirements.system_libraries;
            exports.link.frameworks = requirements.frameworks;
            continue;
        }

        const std::string aid = action_id_for("link", rm.storage_key);
        ArtifactRef terminal{
            terminal_artifact_kind(rm.type), rm.output, aid};

        BuildAction action;
        action.id = aid;
        action.moid_id = rm.canonical_id;
        action.moid = moid_name;
        action.moid_version = rm.decl->version;
        action.outputs = {rm.output};

        if (rm.type == MoidType::Lib) {
            // Lib with objects: produce a static archive (out/lib/lib<name>.a).
            action.type = BuildAction::Type::Archive;
            action.description = "archive " + rm.output.filename_string();
            add_artifact_inputs(action, command_objects);
            auto producer_result =
                add_producer_dependencies(action, command_objects);
            if (!producer_result)
                return std::unexpected(producer_result.error());

            ArchiveCommand archive;
            archive.objects = paths_for(command_objects);
            archive.output = rm.output;
            action.command = make_archive_command(target, archive);

            exports.terminal = terminal;
            exports.link.objects.clear();
            exports.link.libraries.clear();
            append_unique_artifact(exports.link.libraries, terminal);
            append_unique_artifacts(
                exports.link.libraries, dependency_link.libraries);
            exports.link.system_libraries = requirements.system_libraries;
            exports.link.frameworks = requirements.frameworks;
        } else {
            // Dylib or Executable: link into a terminal output.
            action.type = BuildAction::Type::Link;
            action.description = "link " + rm.output.filename_string();
            add_artifact_inputs(action, command_objects);
            add_artifact_inputs(action, dependency_link.libraries);
            auto object_producers =
                add_producer_dependencies(action, command_objects);
            if (!object_producers)
                return std::unexpected(object_producers.error());
            auto library_producers =
                add_producer_dependencies(action, dependency_link.libraries);
            if (!library_producers)
                return std::unexpected(library_producers.error());

            LinkCommand link;
            link.objects = paths_for(command_objects);
            link.libraries = paths_for(dependency_link.libraries);
            link.output = rm.output;
            link.type = rm.type;
            link.use_cxx_linker = rm.uses_cxx;
            link.system_libraries = requirements.system_libraries;
            link.frameworks = requirements.frameworks;
            link.extra_flags = rm.decl->link_flags;
            action.command = make_link_command(target, link);

            exports.terminal = terminal;
            exports.link.objects.clear();
            exports.link.libraries.clear();
            append_unique_artifact(exports.link.libraries, terminal);
            exports.link.system_libraries = requirements.system_libraries;
            exports.link.frameworks = requirements.frameworks;
            if (rm.type == MoidType::Executable) {
                // Executables are terminal consumers, never dependency usage.
                exports.compile = {};
                exports.link = {};
                exports.terminal = terminal;
            }
        }

        std::size_t idx = graph.actions.size();
        graph.actions.push_back(std::move(action));
        action_indices.emplace(aid, idx);
    }

    return graph;
}

// ===== Graph JSON I/O =====

export BuildGraph read_graph_json(const Path& path);

export void write_graph_json(const BuildGraph& graph, const Path& path) {
    json::Value j;
    j["state_dir"] = graph.state_dir.string();
    j["project_root"] = graph.project_root.string();

    auto actions_arr = json::Value::array();
    for (const auto& action : graph.actions) {
        json::Value aj;
        switch (action.type) {
            case BuildAction::Type::CompileModule: aj["type"] = "compile_module"; break;
            case BuildAction::Type::Compile:       aj["type"] = "compile"; break;
            case BuildAction::Type::Link:          aj["type"] = "link"; break;
            case BuildAction::Type::Archive:       aj["type"] = "archive"; break;
        }
        aj["id"] = action.id;
        aj["moid_id"] = action.moid_id;
        aj["moid"] = action.moid;
        aj["moid_version"] = action.moid_version;
        aj["description"] = action.description;

        auto to_str_arr = [](const std::vector<Path>& paths) {
            std::vector<std::string> v;
            for (auto& p : paths) v.push_back(p.string());
            return v;
        };
        aj["inputs"] = to_str_arr(action.inputs);
        aj["outputs"] = to_str_arr(action.outputs);
        aj["command"] = action.command;

        std::vector<std::string> dep_ids;
        for (auto idx : action.depends_on) {
            if (idx < graph.actions.size())
                dep_ids.push_back(graph.actions[idx].id);
        }
        aj["depends_on"] = dep_ids;

        actions_arr.push_back(aj);
    }
    j["actions"] = actions_arr;

    write_file(path, j.dump(2));

    // Verify round-trip: read back and confirm structural consistency.
    auto reread = read_graph_json(path);
    if (reread.actions.size() != graph.actions.size()) {
        std::println(std::cerr,
            "bake: warning: graph.json round-trip mismatch at '{}': "
            "expected {} actions, read back {}",
            path.string(), graph.actions.size(), reread.actions.size());
    } else {
        for (std::size_t i = 0; i < graph.actions.size(); ++i) {
            const auto& orig = graph.actions[i];
            const auto& back = reread.actions[i];
            if (orig.id != back.id || orig.type != back.type ||
                orig.depends_on.size() != back.depends_on.size()) {
                std::println(std::cerr,
                    "bake: warning: graph.json round-trip mismatch "
                    "for action '{}' at '{}'", orig.id, path.string());
                break;
            }
        }
    }
}

export BuildGraph read_graph_json(const Path& path) {
    BuildGraph graph;

    auto content = read_file(path);
    if (!content) return graph;

    json::Value j;
    try {
        j = json::Value::parse(*content);
    } catch (...) {
        return graph;
    }

    graph.state_dir = Path(j.value("state_dir", ""));
    graph.project_root = Path(j.value("project_root", ""));

    if (!j.contains("actions")) return graph;

    // First pass: create actions
    std::map<std::string, std::size_t> id_to_idx;
    for (const auto& aj : j["actions"]) {
        BuildAction action;
        action.id = aj.value("id", "");
        action.moid_id = aj.value("moid_id", "");
        action.moid = aj.value("moid", "");
        action.moid_version = aj.value("moid_version", "");
        action.description = aj.value("description", action.id);

        std::string type_str = aj.value("type", "compile");
        if (type_str == "compile_module") action.type = BuildAction::Type::CompileModule;
        else if (type_str == "link")      action.type = BuildAction::Type::Link;
        else if (type_str == "archive")   action.type = BuildAction::Type::Archive;
        else                              action.type = BuildAction::Type::Compile;

        for (const auto& inp : aj.value("inputs", std::vector<std::string>{}))
            action.inputs.emplace_back(inp);
        for (const auto& out : aj.value("outputs", std::vector<std::string>{}))
            action.outputs.emplace_back(out);
        for (const auto& cmd : aj.value("command", std::vector<std::string>{}))
            action.command.push_back(cmd);

        id_to_idx[action.id] = graph.actions.size();
        graph.actions.push_back(std::move(action));
    }

    // Second pass: resolve depends_on
    for (const auto& aj : j["actions"]) {
        std::string id = aj.value("id", "");
        auto it = id_to_idx.find(id);
        if (it == id_to_idx.end()) continue;
        for (const auto& dep : aj.value("depends_on", std::vector<std::string>{})) {
            auto dep_it = id_to_idx.find(dep);
            if (dep_it != id_to_idx.end())
                graph.actions[it->second].depends_on.push_back(dep_it->second);
        }
    }

    return graph;
}

// ===== Incremental build =====

export bool needs_rebuild(const BuildAction& action) {
    for (auto& out : action.outputs)
        if (!out.exists()) return true;

    for (auto& in : action.inputs) {
        if (!in.exists()) continue;
        auto in_time = std::filesystem::last_write_time(in.fs());
        for (auto& out : action.outputs) {
            if (!out.exists()) return true;
            if (in_time > std::filesystem::last_write_time(out.fs()))
                return true;
        }
    }
    return false;
}

namespace {

std::string action_fingerprint(const BuildAction& action) {
    json::Value doc;
    doc["type"] = static_cast<int>(action.type);
    doc["id"] = action.id;
    doc["command"] = action.command;
    doc["depends_on"] = action.depends_on;
    auto to_arr = [](const std::vector<Path>& paths) {
        std::vector<std::string> v;
        for (auto& p : paths) v.push_back(p.string());
        return v;
    };
    doc["inputs"] = to_arr(action.inputs);
    doc["outputs"] = to_arr(action.outputs);
    return SHA256::hex(doc.dump());
}

std::map<std::string, std::string> load_fingerprints(const Path& path) {
    std::map<std::string, std::string> result;
    auto content = read_file(path);
    if (!content) return result;
    try {
        auto doc = json::Value::parse(*content);
        if (doc.value("schema", 0) != 1 || !doc.contains("actions"))
            return result;
        for (auto& item : doc["actions"].items()) {
            if (item.value().is_string())
                result[item.key()] = item.value().get<std::string>();
        }
    } catch (...) {}
    return result;
}

} // anonymous namespace

// ===== Executor =====

export int execute_graph(BuildGraph& graph, int jobs, bool verbose) {
    auto start_time = std::chrono::steady_clock::now();

    auto print_finished = [&]() {
        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count();
        std::println("    Finished in {:.2}s", elapsed);
    };

    if (graph.actions.empty()) {
        print_finished();
        return 0;
    }

    if (jobs <= 0) {
        jobs = static_cast<int>(std::thread::hardware_concurrency());
        if (jobs == 0) jobs = 4;
    }

    Path state_path = graph.state_dir / "fingerprints.json";
    auto previous = load_fingerprints(state_path);

    std::vector<std::string> fingerprints(graph.actions.size());
    for (std::size_t i = 0; i < graph.actions.size(); ++i)
        fingerprints[i] = action_fingerprint(graph.actions[i]);

    // Determine dirty set with propagation
    std::vector<bool> dirty(graph.actions.size(), false);
    for (std::size_t i = 0; i < graph.actions.size(); ++i) {
        auto& action = graph.actions[i];
        auto prev = previous.find(action.id);
        if (prev == previous.end() || prev->second != fingerprints[i] ||
            needs_rebuild(action))
            dirty[i] = true;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < graph.actions.size(); ++i) {
            if (dirty[i]) continue;
            for (auto dep : graph.actions[i].depends_on) {
                if (dep < dirty.size() && dirty[dep]) {
                    dirty[i] = true;
                    changed = true;
                    break;
                }
            }
        }
    }

    int pending = static_cast<int>(std::count(dirty.begin(), dirty.end(), true));

    if (pending == 0) {
        print_finished();
        return 0;
    }

    std::vector<int> status(graph.actions.size(), 0);  // 0=pending, 1=done, -1=failed
    for (std::size_t i = 0; i < graph.actions.size(); ++i)
        if (!dirty[i]) status[i] = 1;

    std::mutex output_mutex;
    std::atomic<int> failed{0};

    // Per-moid progress is keyed by identity; names are display-only.
    std::map<std::string, int> moid_total;
    std::map<std::string, int> moid_started;
    for (std::size_t i = 0; i < graph.actions.size(); ++i)
        if (dirty[i])
            moid_total[graph.actions[i].moid_id]++;

    std::set<std::string> announced;
    auto announce = [&](const BuildAction& action) {
        std::lock_guard<std::mutex> lock(output_mutex);
        if (announced.insert(action.moid_id).second) {
            if (action.moid_version.empty())
                std::println("   Compiling {}", action.moid);
            else
                std::println("   Compiling {} v{}",
                             action.moid, action.moid_version);
        }
        if (verbose) {
            int started = ++moid_started[action.moid_id];
            int total = moid_total[action.moid_id];
            std::println("    [{}/{}] {}", started, total, action.description);
        }
    };

    // Build reverse dependency edges for O(1) dependent lookup.
    std::vector<std::vector<std::size_t>> dependents(graph.actions.size());
    for (std::size_t i = 0; i < graph.actions.size(); ++i) {
        for (auto dep : graph.actions[i].depends_on) {
            if (dep < graph.actions.size())
                dependents[dep].push_back(i);
        }
    }

    // Count unresolved dirty dependencies per action.
    std::vector<int> pending_deps(graph.actions.size(), 0);
    for (std::size_t i = 0; i < graph.actions.size(); ++i) {
        if (!dirty[i]) continue;
        for (auto dep : graph.actions[i].depends_on) {
            if (dep < dirty.size() && dirty[dep])
                ++pending_deps[i];
        }
    }

    // Shared ready queue: degree-based scheduling over the flat action DAG —
    // moid-level and intra-moid parallelism are both just "no edge, no
    // order" here.
    std::deque<std::size_t> ready_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::atomic<int> completed{0};
    std::atomic<bool> stop{false};

    for (std::size_t i = 0; i < graph.actions.size(); ++i) {
        if (dirty[i] && pending_deps[i] == 0)
            ready_queue.push_back(i);
    }

    auto worker = [&]() {
        while (true) {
            std::size_t idx;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [&] {
                    return stop.load() || !ready_queue.empty();
                });
                if (stop.load()) return;
                idx = ready_queue.front();
                ready_queue.pop_front();
            }

            bool ok = true;
            auto& action = graph.actions[idx];
            announce(action);

            std::vector<std::string> command = action.command;
            std::optional<Path> archive_output;
            std::optional<Path> archive_temp;

            if (action.type == BuildAction::Type::Archive) {
                if (action.outputs.size() != 1) {
                    {
                        std::lock_guard<std::mutex> lock(output_mutex);
                        std::println(
                            std::cerr,
                            "bake: archive action '{}' must have exactly one output",
                            action.id);
                    }
                    ok = false;
                } else {
                    archive_output = action.outputs.front();
                    archive_temp = Path(
                        archive_output->string() + ".bake-archive-tmp");

                    std::error_code error;
                    std::filesystem::remove(archive_temp->fs(), error);
                    if (error) {
                        {
                            std::lock_guard<std::mutex> lock(output_mutex);
                            std::println(
                                std::cerr,
                                "bake: failed to remove temporary archive '{}': {}",
                                archive_temp->string(), error.message());
                        }
                        ok = false;
                        archive_temp.reset();
                    } else {
                        auto output_arg = std::find(
                            command.begin(), command.end(),
                            archive_output->string());
                        if (output_arg == command.end()) {
                            {
                                std::lock_guard<std::mutex> lock(output_mutex);
                                std::println(
                                    std::cerr,
                                    "bake: archive action '{}' has no output argument",
                                    action.id);
                            }
                            ok = false;
                            archive_temp.reset();
                        } else {
                            *output_arg = archive_temp->string();
                        }
                    }
                }
            }

            if (ok) {
                auto result = run_process(command, graph.project_root);
                if (!result.success()) {
                    if (archive_temp) {
                        std::error_code cleanup_error;
                        std::filesystem::remove(archive_temp->fs(),
                                                cleanup_error);
                    }
                    {
                        std::lock_guard<std::mutex> lock(output_mutex);
                        if (!result.stderr_output.empty())
                            std::print(std::cerr, "{}", result.stderr_output);
                        std::println(std::cerr,
                            "bake: action '{}' failed", action.id);
                    }
                    ok = false;
                } else if (archive_temp) {
                    auto replace_error =
                        atomic_replace_file(*archive_temp, *archive_output);
                    if (replace_error) {
                        std::error_code cleanup_error;
                        std::filesystem::remove(archive_temp->fs(),
                                                cleanup_error);
                        {
                            std::lock_guard<std::mutex> lock(output_mutex);
                            std::println(
                                std::cerr,
                                "bake: failed to publish archive '{}': {}",
                                archive_output->string(),
                                replace_error.message());
                        }
                        ok = false;
                    }
                }
            }

            status[idx] = ok ? 1 : -1;
            if (!ok) failed.fetch_add(1);

            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (ok) {
                    completed.fetch_add(1);
                    for (auto dep_idx : dependents[idx]) {
                        if (dirty[dep_idx] &&
                            --pending_deps[dep_idx] == 0) {
                            ready_queue.push_back(dep_idx);
                        }
                    }
                }
                if (failed.load() > 0 || completed.load() >= pending) {
                    stop.store(true);
                }
                queue_cv.notify_all();
            }
        }
    };

    // Launch a persistent shared thread pool.
    int n_workers = std::min(jobs, pending);
    std::vector<std::thread> pool;
    for (int t = 0; t < n_workers; ++t)
        pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    if (failed.load() > 0) {
        std::println(std::cerr, "bake: build failed");
        return 1;
    }

    // Save fingerprints
    json::Value state;
    state["schema"] = 1;
    state["actions"] = json::Value::object();
    for (std::size_t i = 0; i < graph.actions.size(); ++i)
        state["actions"][graph.actions[i].id] = fingerprints[i];
    atomic_write_file(state_path, state.dump(2));

    print_finished();

    return 0;
}

// ===== compile_commands.json =====

export void write_compile_commands(const BuildGraph& graph, const Path& output_path) {
    json::Value commands = json::Value::array();

    for (auto& action : graph.actions) {
        if (!action.is_compile()) continue;
        if (action.inputs.empty() || action.outputs.empty()) continue;

        json::Value entry;
        entry["directory"] = graph.project_root.absolute().string();

        std::string cmd_str;
        for (std::size_t i = 0; i < action.command.size(); ++i) {
            if (i > 0) cmd_str += " ";
            cmd_str += action.command[i];
        }
        entry["command"] = cmd_str;
        entry["file"] = action.inputs[0].absolute().string();
        entry["output"] = action.outputs[0].absolute().string();

        commands.push_back(entry);
    }

    write_file(output_path, commands.dump(2));
}

} // namespace bake
