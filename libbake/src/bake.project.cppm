module;

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
#include <filesystem>
#include <fstream>
#include <variant>
#include <cstdlib>

#include <toml.hpp>

export module bake.project;

import bake.util;

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

// ===== Dependency =====

export struct Dependency {
    std::string name;
    std::string url;           // git URL (empty for path deps)
    std::string tag;           // git tag/branch (empty for path deps)
    std::string path;          // relative path (for path deps)
    bool is_path_dep = false;
};

// ===== BuildOption =====

export struct BuildOption {
    enum class Type { Bool, Int, String };
    Type type = Type::String;
    bool bool_value = false;
    int64_t int_value = 0;
    std::string str_value;

    static BuildOption from_bool(bool v) {
        BuildOption o; o.type = Type::Bool; o.bool_value = v; return o;
    }
    static BuildOption from_int(int64_t v) {
        BuildOption o; o.type = Type::Int; o.int_value = v; return o;
    }
    static BuildOption from_string(std::string v) {
        BuildOption o; o.type = Type::String; o.str_value = std::move(v); return o;
    }
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
            std::fprintf(stderr, "bake: error parsing %s: %s\n",
                         toml_path.string().c_str(), e.what());
            return std::nullopt;
        }

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
                }
                m.dependencies[d.name] = std::move(d);
            }
        }

        // [options]
        if (auto* opts = tbl["options"].as_table()) {
            for (auto& [key, val] : *opts) {
                std::string opt_name = std::string(key.str());
                if (auto v = val.value<bool>()) {
                    m.options[opt_name] = BuildOption::from_bool(*v);
                } else if (auto v = val.value<int64_t>()) {
                    m.options[opt_name] = BuildOption::from_int(*v);
                } else if (auto v = val.value<std::string>()) {
                    m.options[opt_name] = BuildOption::from_string(*v);
                }
            }
        }

        return m;
    }
};

// ===== Layout (directory conventions) =====

export struct Layout {
    Path root;              // project root (contains bake.toml)
    Path source_dir;        // root/src/
    Path public_dir;        // root/public/
    Path tests_dir;         // root/tests/
    Path bake_dir;          // root/.bake/ (build script staging)

    // Unified output directory
    Path out_dir;           // <out_root>/out/
    Path bin_dir;           // out/bin/   (executables)
    Path lib_dir;           // out/lib/   (static/shared libs)
    Path obj_dir;           // out/obj/[<member>/]
    Path bmi_dir;           // out/bmi/[<member>/]

    // Detect layout. Pass ws_root for workspace members so all outputs
    // go under the workspace root's out/ directory.
    static Layout detect(const Path& root, const Path& ws_root = Path{}) {
        Layout l;
        l.root = root;
        l.source_dir = root / "src";
        l.public_dir = root / "public";
        l.tests_dir = root / "tests";
        l.bake_dir = root / ".bake";

        Path out_base = ws_root.string().empty() ? root : ws_root;
        l.out_dir = out_base / "out";
        l.bin_dir = l.out_dir / "bin";
        l.lib_dir = l.out_dir / "lib";

        if (ws_root.string().empty() || ws_root == root) {
            l.obj_dir = l.out_dir / "obj";
            l.bmi_dir = l.out_dir / "bmi";
        } else {
            // Per-member subdirs to avoid name collisions in workspace builds
            std::string member = root.filename_string();
            l.obj_dir = l.out_dir / "obj" / member;
            l.bmi_dir = l.out_dir / "bmi" / member;
        }
        return l;
    }

    void create_directories() const {
        source_dir.mkdir_recursive();
        public_dir.mkdir_recursive();
        bake_dir.mkdir_recursive();
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
            for (size_t i = 0; i < node.dependencies.size(); ++i) {
                if (i > 0) content += ", ";
                content += "\"" + node.dependencies[i] + "\"";
            }
            content += "]\n\n";
        }

        return atomic_write_file(path, content);
    }

    // Check if lockfile is consistent with manifest dependencies.
    // A consistent lock has every non-path manifest dep pinned with a
    // commit and both hashes populated.
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
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return Path(home) / ".cache" / "bake" / "src";
}

} // namespace bake
