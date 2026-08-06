export module bake.project;

import std;
import bake.util;
import tomlplusplus;

// ============================================================
// bake.project — bake.toml model, layout, project discovery
// ============================================================

namespace bake {

// ===== PackageType =====

export enum class PackageType {
    Executable,
    StaticLib,
    SharedLib,
};

export inline std::string_view package_type_str(PackageType t) {
    switch (t) {
        case PackageType::Executable:  return "executable";
        case PackageType::StaticLib:   return "static-lib";
        case PackageType::SharedLib:   return "shared-lib";
    }
    return "unknown";
}

export inline std::optional<PackageType> parse_package_type(std::string_view s) {
    if (s == "executable")    return PackageType::Executable;
    if (s == "static-lib")    return PackageType::StaticLib;
    if (s == "shared-lib")    return PackageType::SharedLib;
    return std::nullopt;
}

// ===== BuildOption =====

export struct BuildOption {
    enum class Type { Bool, Int, String };
    Type type = Type::String;
    bool bool_value = false;
    std::int64_t int_value = 0;
    std::string str_value;

    static BuildOption from_bool(bool v) {
        BuildOption o; o.type = Type::Bool; o.bool_value = v; return o;
    }
    static BuildOption from_int(std::int64_t v) {
        BuildOption o; o.type = Type::Int; o.int_value = v; return o;
    }
    static BuildOption from_string(std::string v) {
        BuildOption o; o.type = Type::String; o.str_value = std::move(v); return o;
    }

    bool operator==(const BuildOption&) const = default;
};

// ===== Dependency =====

export struct Dependency {
    std::string name;
    std::string url;           // git URL (empty for path deps)
    std::string tag;           // git tag/branch (empty for path deps)
    std::string path;          // relative path (for path deps)
    bool is_path_dep = false;
    // Configuration owned and type-checked by the dependency package.
    std::map<std::string, BuildOption> options;
};

// ===== Package =====

export struct Package {
    std::string name;
    std::string version = "0.1.0";
    PackageType type = PackageType::Executable;
    std::string std_version = "c++17";
};

// ===== Workspace =====

export struct Workspace {
    std::vector<std::string> members;
};

// ===== Manifest =====

export struct Manifest {
    Path project_dir;           // directory containing this bake.toml
    std::optional<Workspace> workspace;
    std::optional<Package> package;
    std::map<std::string, Dependency> dependencies;
    std::map<std::string, BuildOption> options;

    bool is_workspace() const { return workspace.has_value(); }
    bool has_package() const { return package.has_value(); }

    // Try to load bake.toml from the given directory.
    // Returns nullopt if no bake.toml exists.
    static std::optional<Manifest> load(const Path& dir) {
        Path toml_path = dir / "bake.toml";
        if (!toml_path.is_regular_file()) return std::nullopt;

        Manifest m;
        m.project_dir = dir;

        toml::table tbl;
        try {
            tbl = toml::parse_file(toml_path.string());
        } catch (const std::exception& e) {
            std::println(std::cerr, "bake: error parsing {}: {}",
                         toml_path.string(), e.what());
            return std::nullopt;
        }

        auto parse_option_value = [](const toml::node& value)
                -> std::optional<BuildOption> {
            // toml++ value<T>() performs numeric conversions, so inspect the
            // node type before extracting the value.
            if (value.is_boolean()) {
                return BuildOption::from_bool(*value.value<bool>());
            }
            if (value.is_integer()) {
                return BuildOption::from_int(*value.value<std::int64_t>());
            }
            if (value.is_string()) {
                return BuildOption::from_string(*value.value<std::string>());
            }
            return std::nullopt;
        };

        // [workspace]
        if (auto* ws = tbl["workspace"].as_table()) {
            Workspace w;
            if (auto* members = (*ws)["members"].as_array()) {
                for (auto& elem : *members) {
                    if (auto s = elem.value<std::string>()) {
                        w.members.push_back(*s);
                    }
                }
            }
            m.workspace = std::move(w);
        }

        // [package]
        if (auto* pkg = tbl["package"].as_table()) {
            Package p;
            if (auto v = (*pkg)["name"].value<std::string>())    p.name = *v;
            if (auto v = (*pkg)["version"].value<std::string>()) p.version = *v;
            if (auto v = (*pkg)["type"].value<std::string>()) {
                if (auto t = parse_package_type(*v)) p.type = *t;
            }
            if (auto v = (*pkg)["std"].value<std::string>()) p.std_version = *v;
            m.package = std::move(p);
        }

        // [dependencies]
        if (auto* deps = tbl["dependencies"].as_table()) {
            for (auto& [key, val] : *deps) {
                Dependency d;
                d.name = key.str();
                if (auto* t = val.as_table()) {
                    if (auto v = (*t)["url"].value<std::string>())  d.url = *v;
                    if (auto v = (*t)["tag"].value<std::string>())  d.tag = *v;
                    if (auto v = (*t)["path"].value<std::string>()) {
                        d.path = *v;
                        d.is_path_dep = true;
                    }
                    if (auto* opts = (*t)["options"].as_table()) {
                        for (auto& [opt_key, opt_value] : *opts) {
                            auto parsed = parse_option_value(opt_value);
                            if (!parsed) {
                                std::println(
                                    std::cerr,
                                    "bake: dependency '{}': option '{}' must be "
                                    "a bool, integer, or string",
                                    d.name, opt_key.str());
                                return std::nullopt;
                            }
                            d.options[std::string(opt_key.str())] =
                                std::move(*parsed);
                        }
                    }
                }
                m.dependencies[d.name] = std::move(d);
            }
        }

        // [options]
        if (auto* opts = tbl["options"].as_table()) {
            for (auto& [key, val] : *opts) {
                std::string opt_name = std::string(key.str());
                if (auto parsed = parse_option_value(val))
                    m.options[opt_name] = std::move(*parsed);
            }
        }

        return m;
    }
};

// ===== Layout (directory conventions) =====

export struct Layout {
    Path root;              // source root (contains bake.toml)
    Path source_dir;        // root/src/
    Path public_dir;        // root/public/
    Path tests_dir;         // root/tests/
    Path bake_dir;          // private build-script state/staging

    // All mutable build products belong to the top-level project's out/.
    Path out_dir;           // <top-level>/out/
    Path package_dir;       // out/ for root, out/.pkgs/<package>/ for deps
    Path package_file;      // exported package usage requirements
    Path bin_dir;
    Path lib_dir;
    Path obj_dir;           // out/.obj/[<member-or-package>/]
    Path bmi_dir;           // out/.bmi/[<member-or-package>/]
    std::string package_name;
    bool dependency_layout = false;

    // Detect layout. Pass ws_root for workspace members so all outputs
    // go under the workspace root's out/ directory.
    static Layout detect(const Path& root, const Path& ws_root = Path{}) {
        Layout l;
        l.root = root;
        l.source_dir = root / "src";
        l.public_dir = root / "public";
        l.tests_dir = root / "tests";

        Path out_base = ws_root.string().empty() ? root : ws_root;
        l.out_dir = out_base / "out";
        l.package_dir = l.out_dir;
        l.bin_dir = l.out_dir / "bin";
        l.lib_dir = l.out_dir / "lib";

        if (ws_root.string().empty() || ws_root == root) {
            l.bake_dir = l.out_dir / ".bake";
            l.obj_dir = l.out_dir / ".obj";
            l.bmi_dir = l.out_dir / ".bmi";
        } else {
            // Per-member subdirs to avoid name collisions in workspace builds
            std::string member = root.filename_string();
            l.bake_dir = l.out_dir / ".bake" / member;
            l.obj_dir = l.out_dir / ".obj" / member;
            l.bmi_dir = l.out_dir / ".bmi" / member;
        }
        l.package_file = l.bake_dir / "package.json";
        return l;
    }

    // A dependency keeps immutable sources at root, while every generated
    // file is placed in the consuming top-level project's out directory.
    static Layout for_dependency(const Path& root, const Path& project_out,
                                 std::string_view package_name) {
        Layout l;
        l.root = root;
        l.source_dir = root / "src";
        l.public_dir = root / "public";
        l.tests_dir = root / "tests";
        l.out_dir = project_out;
        l.package_dir = project_out / ".pkgs" / std::string(package_name);
        l.package_file = l.package_dir / "package.json";
        l.bake_dir = l.package_dir / ".bake";
        l.bin_dir = l.package_dir / "bin";
        l.lib_dir = l.package_dir / "lib";
        l.obj_dir = project_out / ".obj" / std::string(package_name);
        l.bmi_dir = project_out / ".bmi" / std::string(package_name);
        l.package_name = package_name;
        l.dependency_layout = true;
        return l;
    }

    void create_directories() const {
        bake_dir.mkdir_recursive();
        obj_dir.mkdir_recursive();
        bmi_dir.mkdir_recursive();
        bin_dir.mkdir_recursive();
        lib_dir.mkdir_recursive();
    }

    // Output directory for a given package type.
    Path output_for(PackageType type) const {
        return (type == PackageType::Executable) ? bin_dir : lib_dir;
    }
};

// ===== Project discovery =====

// Walk up from cwd to find the nearest bake.toml.
export std::optional<Path> find_project_root(const Path& start = Path::current()) {
    Path dir = start.absolute();
    while (true) {
        Path toml = dir / "bake.toml";
        if (toml.is_regular_file()) {
            return dir;
        }
        auto parent = dir.parent();
        if (parent == dir || parent.string().empty()) break;
        dir = parent;
    }
    return std::nullopt;
}

// ===== Lockfile =====

export struct LockNode {
    std::string id;                         // e.g., "fmt-10.2.1"
    std::string url;
    std::string tag;
    std::string commit;
    std::string transport_sha256;
    std::string tree_sha256;
    bool native = false;                    // has bake.toml
    std::vector<std::string> dependencies;  // child node IDs
};

export struct Lockfile {
    std::map<std::string, std::string> root_deps;   // dep_name → node_id
    std::map<std::string, LockNode> nodes;           // node_id → node
    Path lock_path;

    static std::optional<Lockfile> load(const Path& path) {
        if (!path.is_regular_file()) return std::nullopt;

        Lockfile lf;
        lf.lock_path = path;

        toml::table tbl;
        try {
            tbl = toml::parse_file(path.string());
        } catch (...) {
            return std::nullopt;
        }

        // [root_deps]
        if (auto* roots = tbl["root_deps"].as_table()) {
            for (auto& [key, val] : *roots) {
                if (auto v = val.value<std::string>()) {
                    lf.root_deps[std::string(key.str())] = *v;
                }
            }
        }

        // [nodes.<id>]
        if (auto* nodes = tbl["nodes"].as_table()) {
            for (auto& [key, val] : *nodes) {
                if (auto* node_tbl = val.as_table()) {
                    LockNode node;
                    node.id = std::string(key.str());
                    node.url     = (*node_tbl)["url"].value_or("");
                    node.tag     = (*node_tbl)["tag"].value_or("");
                    node.commit  = (*node_tbl)["commit"].value_or("");
                    node.transport_sha256 = (*node_tbl)["transport_sha256"].value_or("");
                    node.tree_sha256      = (*node_tbl)["tree_sha256"].value_or("");
                    node.native  = (*node_tbl)["native"].value_or(false);

                    if (auto* deps = (*node_tbl)["dependencies"].as_array()) {
                        for (auto& d : *deps) {
                            if (auto s = d.value<std::string>()) {
                                node.dependencies.push_back(*s);
                            }
                        }
                    }
                    lf.nodes[node.id] = std::move(node);
                }
            }
        }

        return lf;
    }

    bool save(const Path& path) const {
        std::string content = "# AUTO-GENERATED. Do not edit.\n\n";

        // [root_deps]
        content += "[root_deps]\n";
        for (auto& [name, node_id] : root_deps) {
            content += name + " = \"" + node_id + "\"\n";
        }
        content += "\n";

        // [nodes.<id>]
        for (auto& [id, node] : nodes) {
            content += "[nodes.\"" + id + "\"]\n";
            content += "url              = \"" + node.url + "\"\n";
            content += "tag              = \"" + node.tag + "\"\n";
            content += "commit           = \"" + node.commit + "\"\n";
            content += "transport_sha256 = \"" + node.transport_sha256 + "\"\n";
            content += "tree_sha256      = \"" + node.tree_sha256 + "\"\n";
            content += "native           = " + std::string(node.native ? "true" : "false") + "\n";

            content += "dependencies     = [";
            for (std::size_t i = 0; i < node.dependencies.size(); ++i) {
                if (i > 0) content += ", ";
                content += "\"" + node.dependencies[i] + "\"";
            }
            content += "]\n\n";
        }

        return atomic_write_file(path, content);
    }

    // Check if lockfile is consistent with manifest dependencies.
    // A consistent lock has every non-path manifest dep pinned with a
    // commit and both hashes populated, AND every node's dependency edges
    // reference nodes that exist in the lock (transitive completeness).
    bool is_consistent(const Manifest& manifest) const {
        for (auto& [name, dep] : manifest.dependencies) {
            // Path deps are not tracked in the lockfile — skip entirely.
            if (dep.is_path_dep) continue;

            auto it = root_deps.find(name);
            if (it == root_deps.end()) return false;

            // Node must exist
            if (nodes.find(it->second) == nodes.end()) return false;

            const auto& node = nodes.at(it->second);

            // URL and tag must match the manifest
            if (node.url != dep.url) return false;
            if (node.tag != dep.tag) return false;

            // Commit and both hashes must be populated (trustworthy pin)
            if (node.commit.empty()) return false;
            if (node.transport_sha256.empty()) return false;
            if (node.tree_sha256.empty()) return false;
        }

        // Transitive completeness: every node's child reference must exist.
        for (auto& [id, node] : nodes) {
            for (auto& child_id : node.dependencies) {
                if (nodes.find(child_id) == nodes.end()) return false;
            }
        }

        return true;
    }

    // True when the manifest declares only path dependencies (or none).
    // In that case no lockfile is needed at all.
    static bool has_only_path_deps(const Manifest& manifest) {
        for (auto& [name, dep] : manifest.dependencies) {
            if (!dep.is_path_dep) return false;
        }
        return true;
    }

    bool empty() const { return root_deps.empty(); }
};

// Get the global source cache directory
export Path get_cache_dir() {
#ifdef _WIN32
    const char* home = std::getenv("LOCALAPPDATA");
    if (!home) home = "C:\\";
    return Path(home) / "bake" / "src";
#else
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return Path(home) / ".cache" / "bake" / "src";
#endif
}

} // namespace bake
