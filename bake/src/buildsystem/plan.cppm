export module bake.buildsystem.plan;

import std;
import bake.util;
import bake.json;
import bake.buildsystem.project;
import bake.buildsystem.moid;
import bake.buildsystem.graph;
import bake.buildsystem.cmdgen;
import bake.buildsystem.source;
import bake.toolchain.target;

namespace json = bake::json;

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
                binary_moid_id(main_id, binary.name);
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

namespace {

bool exact_graph_fields(const json::Value& object,
                        std::initializer_list<std::string_view> fields) {
    if (!object.is_object() || object.size() != fields.size()) return false;
    for (auto field : fields)
        if (!object.contains(field)) return false;
    return true;
}

std::expected<std::string, std::string> graph_string(
        const json::Value& object, std::string_view field,
        std::string_view parent) {
    const auto* value = object.find(field);
    if (!value || !value->is_string())
        return std::unexpected(std::string(parent) + "." +
                               std::string(field) + " must be a string");
    return value->get<std::string>();
}

std::expected<std::vector<std::string>, std::string> graph_strings(
        const json::Value& object, std::string_view field,
        std::string_view parent) {
    const auto* value = object.find(field);
    if (!value || !value->is_array())
        return std::unexpected(std::string(parent) + "." +
                               std::string(field) + " must be an array");
    std::vector<std::string> result;
    result.reserve(value->size());
    for (std::size_t index = 0; index < value->size(); ++index) {
        if (!(*value)[index].is_string())
            return std::unexpected(std::string(parent) + "." +
                                   std::string(field) + "[" +
                                   std::to_string(index) +
                                   "] must be a string");
        result.push_back((*value)[index].get<std::string>());
    }
    return result;
}

std::string action_type_name(BuildAction::Type type) {
    switch (type) {
        case BuildAction::Type::CompileModule: return "compile_module";
        case BuildAction::Type::Compile: return "compile";
        case BuildAction::Type::Link: return "link";
        case BuildAction::Type::Archive: return "archive";
    }
    std::unreachable();
}

std::expected<BuildAction::Type, std::string> parse_action_type(
        std::string_view type) {
    if (type == "compile_module") return BuildAction::Type::CompileModule;
    if (type == "compile") return BuildAction::Type::Compile;
    if (type == "link") return BuildAction::Type::Link;
    if (type == "archive") return BuildAction::Type::Archive;
    return std::unexpected("unknown build action type '" +
                           std::string(type) + "'");
}

std::vector<std::string> path_strings(const std::vector<Path>& paths) {
    std::vector<std::string> result;
    result.reserve(paths.size());
    for (const auto& path : paths) result.push_back(path.string());
    return result;
}

}

export json::Value build_graph_to_json(const BuildGraph& graph) {
    json::Value document = json::Value::object();
    document["state_dir"] = graph.state_dir.string();
    document["project_root"] = graph.project_root.string();
    document["actions"] = json::Value::array();
    for (const auto& action : graph.actions) {
        json::Value entry = json::Value::object();
        entry["type"] = action_type_name(action.type);
        entry["id"] = action.id;
        entry["moid_id"] = action.moid_id;
        entry["moid"] = action.moid;
        entry["moid_version"] = action.moid_version;
        entry["description"] = action.description;
        entry["inputs"] = path_strings(action.inputs);
        entry["outputs"] = path_strings(action.outputs);
        entry["command"] = action.command;
        std::vector<std::string> dependencies;
        dependencies.reserve(action.depends_on.size());
        for (std::size_t index : action.depends_on) {
            if (index < graph.actions.size())
                dependencies.push_back(graph.actions[index].id);
        }
        entry["depends_on"] = dependencies;
        document["actions"].push_back(std::move(entry));
    }
    return document;
}

export std::expected<BuildGraph, std::string> build_graph_from_json(
        const json::Value& document) {
    if (!exact_graph_fields(document,
                            {"state_dir", "project_root", "actions"}))
        return std::unexpected(
            "build graph must contain exactly state_dir, project_root, actions");
    auto state_dir = graph_string(document, "state_dir", "build_graph");
    auto project_root = graph_string(document, "project_root", "build_graph");
    if (!state_dir) return std::unexpected(state_dir.error());
    if (!project_root) return std::unexpected(project_root.error());
    if (state_dir->empty() || project_root->empty())
        return std::unexpected("build graph paths must not be empty");
    const auto* actions = document.find("actions");
    if (!actions || !actions->is_array())
        return std::unexpected("build_graph.actions must be an array");

    BuildGraph graph;
    graph.state_dir = Path(*state_dir);
    graph.project_root = Path(*project_root);
    std::map<std::string, std::size_t> action_indices;
    std::vector<std::vector<std::string>> dependencies;
    graph.actions.reserve(actions->size());
    dependencies.reserve(actions->size());
    for (std::size_t index = 0; index < actions->size(); ++index) {
        const auto& entry = (*actions)[index];
        const std::string parent =
            "build_graph.actions[" + std::to_string(index) + "]";
        if (!exact_graph_fields(entry, {"type", "id", "moid_id", "moid",
                                        "moid_version", "description", "inputs",
                                        "outputs", "command", "depends_on"}))
            return std::unexpected(parent + " has missing or unknown fields");
        auto type_text = graph_string(entry, "type", parent);
        auto id = graph_string(entry, "id", parent);
        auto moid_id = graph_string(entry, "moid_id", parent);
        auto moid = graph_string(entry, "moid", parent);
        auto version = graph_string(entry, "moid_version", parent);
        auto description = graph_string(entry, "description", parent);
        auto inputs = graph_strings(entry, "inputs", parent);
        auto outputs = graph_strings(entry, "outputs", parent);
        auto command = graph_strings(entry, "command", parent);
        auto action_dependencies = graph_strings(entry, "depends_on", parent);
        if (!type_text) return std::unexpected(type_text.error());
        if (!id) return std::unexpected(id.error());
        if (!moid_id) return std::unexpected(moid_id.error());
        if (!moid) return std::unexpected(moid.error());
        if (!version) return std::unexpected(version.error());
        if (!description) return std::unexpected(description.error());
        if (!inputs) return std::unexpected(inputs.error());
        if (!outputs) return std::unexpected(outputs.error());
        if (!command) return std::unexpected(command.error());
        if (!action_dependencies)
            return std::unexpected(action_dependencies.error());
        auto type = parse_action_type(*type_text);
        if (!type) return std::unexpected(type.error());
        if (id->empty() || moid_id->empty() || outputs->empty() ||
            command->empty())
            return std::unexpected(parent + " has an empty required value");
        if (!action_indices.emplace(*id, index).second)
            return std::unexpected("duplicate build action id '" + *id + "'");
        BuildAction action;
        action.type = *type;
        action.id = std::move(*id);
        action.moid_id = std::move(*moid_id);
        action.moid = std::move(*moid);
        action.moid_version = std::move(*version);
        action.description = std::move(*description);
        for (auto& input : *inputs) action.inputs.emplace_back(std::move(input));
        for (auto& output : *outputs) action.outputs.emplace_back(std::move(output));
        action.command = std::move(*command);
        graph.actions.push_back(std::move(action));
        dependencies.push_back(std::move(*action_dependencies));
    }

    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        std::set<std::size_t> seen;
        for (const auto& dependency : dependencies[index]) {
            auto found = action_indices.find(dependency);
            if (found == action_indices.end())
                return std::unexpected("build action '" + graph.actions[index].id +
                                       "' references unknown dependency '" +
                                       dependency + "'");
            if (found->second >= index)
                return std::unexpected("build action '" + graph.actions[index].id +
                                       "' has a non-topological dependency");
            if (!seen.insert(found->second).second)
                return std::unexpected("build action '" + graph.actions[index].id +
                                       "' repeats a dependency");
            graph.actions[index].depends_on.push_back(found->second);
        }
    }
    return graph;
}

export std::expected<BuildGraph, std::string> read_graph_json(
        const Path& path) {
    auto content = read_file(path);
    if (!content)
        return std::unexpected("failed to read build graph '" + path.string() +
                               "'");
    try {
        return build_graph_from_json(json::Value::parse(*content));
    } catch (const std::exception& error) {
        return std::unexpected("invalid build graph JSON: " +
                               std::string(error.what()));
    }
}

export std::expected<void, std::string>
write_graph_json(const BuildGraph& graph, const Path& path) {
    for (std::size_t action = 0; action < graph.actions.size(); ++action) {
        for (std::size_t dependency : graph.actions[action].depends_on) {
            if (dependency >= action) {
                return std::unexpected(
                    "action '" + graph.actions[action].id +
                    "' has an invalid dependency index");
            }
        }
    }
    auto document = build_graph_to_json(graph);
    auto validated = build_graph_from_json(document);
    if (!validated) return std::unexpected(validated.error());
    if (!atomic_write_file(path, document.dump(2)))
        return std::unexpected("failed to atomically write graph.json at '" +
                               path.string() + "'");
    return {};
}

} // namespace bake
