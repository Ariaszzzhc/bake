module;

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
#include <filesystem>
#include <fstream>
#include <variant>

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
    Path source_dir;        // src/
    Path public_dir;        // public/
    Path tests_dir;         // tests/
    Path build_dir;         // .bake/
    Path artifacts_dir;     // build/ (output executables/libs)
    Path obj_dir;           // .bake/obj/
    Path dep_dir;           // .bake/dep/ (staging for dependencies)

    static Layout detect(const Path& root) {
        Layout l;
        l.root = root;
        l.source_dir = root / "src";
        l.public_dir = root / "public";
        l.tests_dir = root / "tests";
        l.build_dir = root / ".bake";
        l.artifacts_dir = root / "build";
        l.obj_dir = l.build_dir / "obj";
        l.dep_dir = l.build_dir / "dep";
        return l;
    }

    void create_directories() const {
        source_dir.mkdir_recursive();
        public_dir.mkdir_recursive();
        build_dir.mkdir_recursive();
        obj_dir.mkdir_recursive();
        dep_dir.mkdir_recursive();
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

} // namespace bake
