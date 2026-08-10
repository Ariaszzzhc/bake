export module bake.engine;

import std;
import bake.util;
import bake.project;
import bake.moid;
import bake.graph;
import bake.compiler;
import nlohmann.json;

// ============================================================
// bake.engine — MoidDeclaration, unified DAG, executor
//
// Two-phase architecture:
//   Configure: convention or build.cpp → MoidDeclaration JSON
//   Build:      declarations → unified DAG → graph.json → execute
// ============================================================

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

export SourceSet discover_sources(const Path& src_dir, const Path& public_dir) {
    SourceSet sources;

    if (src_dir.is_directory()) {
        for (auto& entry : std::filesystem::recursive_directory_iterator(src_dir.fs())) {
            if (!entry.is_regular_file()) continue;
            Path p(entry.path());
            if (p.is_cpp()) {
                sources.cpp_files.push_back(p);
            } else if (p.is_c()) {
                sources.c_files.push_back(p);
            } else if (p.is_module_interface()) {
                sources.module_interfaces.push_back(p);
            }
        }
    }

    if (public_dir.is_directory()) {
        for (auto& entry : std::filesystem::recursive_directory_iterator(public_dir.fs())) {
            if (!entry.is_regular_file()) continue;
            Path p(entry.path());
            if (p.is_module_interface()) {
                sources.module_interfaces.push_back(p);
            } else if (p.has_extension(".hpp") || p.has_extension(".h") ||
                       p.has_extension(".hxx") || p.has_extension(".hh")) {
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

// Text-based module scanner: extracts module name and imports by parsing
// `export module <name>;` and `import <name>;` declarations directly.
// This replaces the former clang-scan-deps subprocess — bake no longer needs
// any external binary for C++ module dependency scanning.
export std::optional<ModuleInfo> scan_module_file(const Path& source) {
    auto content = read_file(source);
    if (!content) return std::nullopt;

    // Strip comments so declarations inside /* */ or after // are ignored.
    std::string stripped;
    stripped.reserve(content->size());
    bool in_block = false;
    bool in_line = false;
    for (std::size_t i = 0; i < content->size(); ++i) {
        char c = (*content)[i];
        if (in_block) {
            if (c == '*' && i + 1 < content->size() && (*content)[i + 1] == '/') {
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
            if (c == '/' && i + 1 < content->size()) {
                if ((*content)[i + 1] == '*') {
                    in_block = true;
                    ++i;
                    stripped += ' ';
                    continue;
                }
                if ((*content)[i + 1] == '/') {
                    in_line = true;
                    ++i;
                    stripped += ' ';
                    continue;
                }
            }
            stripped += c;
        }
    }

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

// ===== Convention declaration =====
//
// Scans src/ and public/ per bake conventions and produces a declaration.

export MoidDeclaration convention_declare(
        const Manifest& manifest, const Layout& layout,
        const std::string& canonical_id,
        const std::map<std::string, BuildOption>& options,
        const std::vector<MoidDependency>& dependencies) {
    MoidDeclaration declaration;
    declaration.id = canonical_id;
    declaration.name = manifest.moid->name;
    declaration.version = manifest.moid->version;
    declaration.type = manifest.moid->type;
    declaration.root = layout.root.absolute().string();
    declaration.std_version = manifest.moid->std_version;
    declaration.options = options;
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

    auto sources = discover_sources(layout.source_dir, layout.public_dir);
    for (const auto& source : sources.cpp_files)
        add_source(source, false);
    for (const auto& source : sources.c_files)
        add_source(source, false);
    for (const auto& source : sources.module_interfaces)
        add_source(source, is_public_source(source));

    if (layout.public_dir.is_directory())
        declaration.public_include_dirs.push_back("public");

    for (const auto& [name, dependency] : manifest.dependencies) {
        // A raw path dependency has no declaration of its own. Keep its source
        // and include inputs in the consumer declaration as convention mode did.
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
            if (source.is_cpp() || source.is_c() || source.is_module_interface())
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
// Convention mode and build.cpp mode both produce the same MoidDeclaration
// JSON, so they converge here — the DAG builder never distinguishes them.

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

bool is_portable_terminal_name(std::string_view name) {
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

std::optional<std::string> validate_terminal_relative_path(
        std::filesystem::path relative) {
    relative = relative.lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_path())
        return std::nullopt;
    const auto parent = relative.parent_path().generic_string();
    const auto filename = relative.filename().generic_string();
    if ((parent != "bin" && parent != "lib" &&
         !parent.starts_with("bin-") && !parent.starts_with("lib-")) ||
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

export std::expected<BuildGraph, std::string> build_graph(
        const MoidGraph& outer_graph,
        const Toolchain& tc,
        const Path& out_dir,
        const Path& project_root,
        const ModuleFileMap& prebuilt_modules) {

    auto topology = topological_moids(outer_graph);
    if (!topology) return std::unexpected(topology.error());

    BuildGraph graph;
    graph.state_dir = out_dir / ".bake";
    graph.project_root = project_root;

    (out_dir / ".obj").mkdir_recursive();
    (out_dir / ".bmi").mkdir_recursive();

    // Output subdirectory: "bin" for native, "bin-<triple>" for cross-compile.
    std::string bin_subdir = tc.target.is_native()
        ? "bin" : "bin-" + tc.target.triple();
    std::string lib_subdir = tc.target.is_native()
        ? "lib" : "lib-" + tc.target.triple();
    (out_dir / bin_subdir).mkdir_recursive();
    (out_dir / lib_subdir).mkdir_recursive();
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
        std::map<std::string, std::vector<std::string>> source_flags;
        std::map<std::string, std::vector<std::pair<std::string, std::string>>>
            source_defines;
        std::map<std::string, std::vector<Path>> source_include_dirs;
        bool uses_cxx = false;
    };

    std::map<std::string, ResolvedMoid> moids;
    std::map<std::string, MoidExports> moid_exports;
    std::map<std::string, std::size_t> action_indices;
    std::vector<std::string> moid_order;
    std::map<std::string, std::string> storage_owners;

    for (const auto& id : *topology) {
        const auto& decl = outer_graph.nodes.at(id).declaration;
        ResolvedMoid rm;
        rm.decl = &decl;
        rm.canonical_id = id.value;
        rm.storage_key = moid_storage_key(id.value);
        auto [storage, storage_inserted] =
            storage_owners.emplace(rm.storage_key, id.value);
        if (!storage_inserted && storage->second != id.value) {
            return std::unexpected(
                "canonical moid storage key collision between '" +
                storage->second + "' and '" + id.value + "'");
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
                if (previous.is_public != group.is_public ||
                    previous.options.flags != group.options.flags ||
                    previous.options.defines != group.options.defines ||
                    previous.options.include_dirs != group.options.include_dirs) {
                    return std::unexpected(
                        "conflicting source groups for '" +
                        normalized_source_id(rm.source_dir, source) +
                        "' in moid '" + decl.name + "'");
                }
                continue;
            }

            if (source.is_module_interface()) {
                rm.module_interfaces.push_back(source);
                rm.uses_cxx = true;
                if (group.is_public)
                    rm.public_module_sources.insert(source_key);
            } else if (source.is_cpp()) {
                rm.cpp_files.push_back(source);
                rm.uses_cxx = true;
            } else if (source.is_c()) {
                rm.c_files.push_back(source);
            }

            rm.source_flags[source_key] = group.options.flags;
            for (const auto& define : group.options.defines) {
                rm.source_defines[source_key].emplace_back(define, "");
                if (group.is_public)
                    append_unique_string(rm.own_compile.defines, define);
            }
            for (const auto& include_dir : group.options.include_dirs) {
                rm.source_include_dirs[source_key].push_back(
                    resolve_path(include_dir, rm.source_dir));
            }
        }

        for (const auto& include_dir : decl.public_include_dirs) {
            append_unique_path(
                rm.own_compile.include_dirs,
                resolve_path(include_dir, rm.source_dir));
        }
        rm.uses_cxx = rm.uses_cxx || !is_c_standard(decl.std_version);

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
                    "' for moid '" + id.value +
                    "': expected a single filename component");
            }
            if (!is_portable_terminal_name(decl.name)) {
                return std::unexpected(
                    "invalid terminal output name '" + decl.name +
                    "' for moid '" + id.value +
                    "': expected a portable ASCII name matching "
                    "[A-Za-z0-9][A-Za-z0-9._+@-]* without a trailing dot or "
                    "reserved device name");
            }

            std::string out_name = library_name(decl.name, rm.type, tc.target);
            rm.output = (rm.type == MoidType::Executable)
                ? out_dir / bin_subdir / out_name
                : out_dir / lib_subdir / out_name;

            auto relative_output = terminal_relative_path(out_dir, rm.output);
            if (!relative_output) {
                return std::unexpected(
                    "terminal output '" + rm.output.string() +
                    "' is not contained by the output directory");
            }
            const std::string output_key =
                portable_terminal_key(*relative_output);
            TerminalClaim owner{id.value, decl.name};
            auto [claim, inserted] =
                terminal_claims.emplace(output_key, owner);
            if (!inserted && claim->second.canonical_id != id.value) {
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

        moids.emplace(id.value, std::move(rm));
        moid_exports.try_emplace(id.value);
        moid_order.push_back(id.value);
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
        auto it = moid.source_include_dirs.find(source.string());
        if (it != moid.source_include_dirs.end()) {
            for (const auto& include_dir : it->second)
                append_unique_path(result, include_dir);
        }
        return result;
    };

    auto compile_defines = [&](const ResolvedMoid& moid,
                               const Path& source) {
        std::vector<std::pair<std::string, std::string>> result;
        auto append_define = [&](const std::string& definition) {
            const auto equals = definition.find('=');
            std::pair<std::string, std::string> parsed = equals == std::string::npos
                ? std::pair{definition, std::string{}}
                : std::pair{definition.substr(0, equals),
                            definition.substr(equals + 1)};
            if (std::find(result.begin(), result.end(), parsed) == result.end())
                result.push_back(std::move(parsed));
        };
        for (const auto& definition :
             moid_exports.at(moid.canonical_id).compile.defines) {
            append_define(definition);
        }
        auto own = moid.source_defines.find(source.string());
        if (own != moid.source_defines.end()) {
            for (const auto& [name, value] : own->second) {
                append_define(value.empty() ? name : name + "=" + value);
            }
        }
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
        cc.std_ver = rm.decl->std_version;
        cc.include_dirs = compile_include_dirs(rm, src);
        cc.defines = compile_defines(rm, src);
        cc.is_module_interface = true;
        cc.bmi_output = pcm;
        cc.use_pic = (rm.type != MoidType::Executable);

        auto flags_it = rm.source_flags.find(src.string());
        if (flags_it != rm.source_flags.end())
            cc.extra_flags = flags_it->second;

        for (const auto& dependency : module_provider_closure(
                 module_dependencies[module_key])) {
            auto action = module_actions.find(dependency);
            if (action != module_actions.end()) {
                cc.module_deps.push_back(
                    {dependency.second, action->second.module.path});
            }
        }

        for (auto& [pname, pcm_path] : prebuilt_modules) {
            if (!pcm_path.string().empty() && pcm_path.is_regular_file()) {
                bool found = false;
                for (auto& [e, _] : cc.module_deps)
                    if (e == pname) { found = true; break; }
                if (!found)
                    cc.module_deps.push_back({pname, pcm_path});
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
        for (auto& [_, dep_pcm] : cc.module_deps)
            action.inputs.push_back(dep_pcm);
        action.outputs = {obj, pcm};
        action.command = make_compile_command(tc, cc);

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
            cc.std_ver = rm.decl->std_version;
            cc.include_dirs = compile_include_dirs(rm, src);
            cc.defines = compile_defines(rm, src);
            cc.use_pic = (rm.type != MoidType::Executable);

            auto flags_it = rm.source_flags.find(src.string());
            if (flags_it != rm.source_flags.end())
                cc.extra_flags = flags_it->second;

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

            if (!src.is_c()) {
                for (auto& [pname, pcm_path] : prebuilt_modules) {
                    if (!pcm_path.string().empty() && pcm_path.is_regular_file()) {
                        bool found = false;
                        for (auto& [e, _] : cc.module_deps)
                            if (e == pname) { found = true; break; }
                        if (!found)
                            cc.module_deps.push_back({pname, pcm_path});
                    }
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
            for (auto& [_, dep_pcm] : cc.module_deps)
                action.inputs.push_back(dep_pcm);
            action.outputs = {obj};
            action.command = make_compile_command(tc, cc);

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
            action.command = make_archive_command(tc, archive);

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
            action.command = make_link_command(tc, link);

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
    nlohmann::json j;
    j["state_dir"] = graph.state_dir.string();
    j["project_root"] = graph.project_root.string();

    auto actions_arr = nlohmann::json::array();
    for (const auto& action : graph.actions) {
        nlohmann::json aj;
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

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(*content);
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
    nlohmann::json doc;
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
        auto doc = nlohmann::json::parse(*content);
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
    int skipped = static_cast<int>(graph.actions.size()) - pending;

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

    // Shared ready queue with Moid-level progress gate.
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
    nlohmann::json state;
    state["schema"] = 1;
    state["actions"] = nlohmann::json::object();
    for (std::size_t i = 0; i < graph.actions.size(); ++i)
        state["actions"][graph.actions[i].id] = fingerprints[i];
    atomic_write_file(state_path, state.dump(2));

    print_finished();

    return 0;
}

// ===== compile_commands.json =====

export void write_compile_commands(const BuildGraph& graph, const Path& output_path) {
    nlohmann::json commands = nlohmann::json::array();

    for (auto& action : graph.actions) {
        if (!action.is_compile()) continue;
        if (action.inputs.empty() || action.outputs.empty()) continue;

        nlohmann::json entry;
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
