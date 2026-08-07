export module bake.project;

import std;
import bake.util;
import tomlplusplus;
import nlohmann.json;

// ============================================================
// bake.project — bake.toml model, layout, project discovery
// ============================================================

namespace bake {

// ===== MoidType =====

export enum class MoidType {
    Executable,
    Lib,
    Dylib,
};

export std::string_view moid_type_str(MoidType type) {
    switch (type) {
        case MoidType::Executable: return "executable";
        case MoidType::Lib:        return "lib";
        case MoidType::Dylib:      return "dylib";
    }
    return "unknown";
}

export std::expected<MoidType, std::string>
parse_moid_type(std::string_view text) {
    if (text == "executable") return MoidType::Executable;
    if (text == "lib")        return MoidType::Lib;
    if (text == "dylib")      return MoidType::Dylib;
    return std::unexpected(
        "unknown moid type '" + std::string(text) + "'");
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

export std::string_view build_option_type_name(BuildOption::Type type) {
    switch (type) {
        case BuildOption::Type::Bool: return "boolean";
        case BuildOption::Type::Int: return "integer";
        case BuildOption::Type::String: return "string";
    }
    return "unknown";
}

export std::string format_build_option(const BuildOption& option) {
    switch (option.type) {
        case BuildOption::Type::Bool:
            return option.bool_value ? "true" : "false";
        case BuildOption::Type::Int:
            return std::to_string(option.int_value);
        case BuildOption::Type::String: {
            std::ostringstream output;
            output << std::quoted(option.str_value);
            return output.str();
        }
    }
    return "<unknown>";
}

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

export std::string normalize_dependency_url(std::string_view raw_url) {
    std::string url = trim(raw_url);
    while (url.size() > 1 && url.back() == '/') url.pop_back();
    if (ends_with(url, ".git")) url.resize(url.size() - 4);

    const std::size_t scheme = url.find("://");
    if (scheme != std::string::npos) {
        const std::size_t host_end = url.find('/', scheme + 3);
        const std::size_t end = host_end == std::string::npos
            ? url.size() : host_end;
        for (std::size_t i = 0; i < end; ++i) {
            if (url[i] >= 'A' && url[i] <= 'Z')
                url[i] = static_cast<char>(url[i] + ('a' - 'A'));
        }
    }
    return url;
}

// ===== Moid =====

export struct Moid {
    std::string name;
    std::string version = "0.1.0";
    MoidType type = MoidType::Executable;
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
    std::optional<Moid> moid;
    std::map<std::string, Dependency> dependencies;
    std::map<std::string, BuildOption> options;

    bool is_workspace() const { return workspace.has_value(); }
    bool has_moid() const { return moid.has_value(); }

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

        if (tbl.contains("package")) {
            std::println(std::cerr,
                         "bake: [package] is not supported; use [moid] in {}",
                         toml_path.string());
            return std::nullopt;
        }

        // [moid]
        if (auto* moid_tbl = tbl["moid"].as_table()) {
            Moid value;
            if (auto v = (*moid_tbl)["name"].value<std::string>())
                value.name = *v;
            if (auto v = (*moid_tbl)["version"].value<std::string>())
                value.version = *v;
            if (auto* type_node = moid_tbl->get("type")) {
                auto token = type_node->value<std::string>();
                if (!token) {
                    std::println(std::cerr,
                                 "bake: moid type must be a string in {}",
                                 toml_path.string());
                    return std::nullopt;
                }
                auto parsed = parse_moid_type(*token);
                if (!parsed) {
                    std::println(std::cerr, "bake: {} in {}",
                                 parsed.error(), toml_path.string());
                    return std::nullopt;
                }
                value.type = *parsed;
            }
            if (auto v = (*moid_tbl)["std"].value<std::string>())
                value.std_version = *v;
            m.moid = std::move(value);
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

    // Output directory for a given Moid type.
    Path output_for(MoidType type) const {
        return (type == MoidType::Executable) ? bin_dir : lib_dir;
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

// ===== Lockfile (JSON, flat identity-keyed entries) =====
//
// Format:
// {
//   "deps": {
//     "<key>": { "path": "../relative" },                          // path dep
//     "<key>": { "url": "...", "ref": "...", "ref_type": "tag",
//                "commit": "...", "integrity": "sha256-..." },      // git dep
//     "<key>": { "url": "...", "integrity": "sha256-..." }          // archive dep
//   }
// }
//
// Identity rules (lock key):
//   Moid package (has bake.toml):   key = [moid].name
//   Non-Moid git package:           key = "git:<url>@<commit>"
//   Non-Moid archive package:        key = "archive:<url>@<content-sha256>"
//   Non-Moid path package:           key = "path:<relative-path>"

export struct LockDep {
    std::string key;          // identity key
    // path dep
    std::string path;
    // git dep
    std::string url;
    std::string ref;          // tag / branch / commit value
    std::string ref_type;     // "tag" | "branch" | "commit"
    std::string commit;
    // git + archive dep
    std::string integrity;    // "sha256-<hex>"

    bool is_path_dep() const { return !path.empty(); }
    bool is_remote() const { return !url.empty(); }
    bool is_git() const { return !commit.empty(); }
    bool is_archive() const { return is_remote() && !is_git(); }

    // Extract the bare hex hash from "sha256-<hex>".
    std::string cache_hash() const {
        if (integrity.empty()) return {};
        auto pos = integrity.find('-');
        return pos == std::string::npos ? integrity : integrity.substr(pos + 1);
    }
};

export struct Lockfile {
    std::map<std::string, LockDep> deps;   // identity key → dep
    Path lock_path;

    static std::optional<Lockfile> load(const Path& path) {
        if (!path.is_regular_file()) return std::nullopt;

        auto content = read_file(path);
        if (!content) return std::nullopt;

        Lockfile lf;
        lf.lock_path = path;

        nlohmann::json doc;
        try {
            doc = nlohmann::json::parse(*content);
        } catch (...) {
            return std::nullopt;
        }

        if (!doc.is_object()) return std::nullopt;
        auto deps_it = doc.find("deps");
        if (deps_it == doc.end() || !deps_it->is_object()) return std::nullopt;

        for (const auto& item : deps_it->items()) {
            const auto& val = item.value();
            if (!val.is_object()) continue;
            LockDep dep;
            dep.key = item.key();
            if (auto v = val.value("path", ""); !v.empty()) dep.path = v;
            if (auto v = val.value("url", ""); !v.empty()) dep.url = v;
            if (auto v = val.value("ref", ""); !v.empty()) dep.ref = v;
            if (auto v = val.value("ref_type", ""); !v.empty()) dep.ref_type = v;
            if (auto v = val.value("commit", ""); !v.empty()) dep.commit = v;
            if (auto v = val.value("integrity", ""); !v.empty()) dep.integrity = v;
            lf.deps[dep.key] = std::move(dep);
        }

        return lf;
    }

    bool save(const Path& path) const {
        nlohmann::json doc = nlohmann::json::object();
        nlohmann::json deps_obj = nlohmann::json::object();

        for (auto& [key, dep] : deps) {
            nlohmann::json entry = nlohmann::json::object();
            if (dep.is_path_dep()) {
                entry["path"] = dep.path;
            } else if (dep.is_archive()) {
                entry["url"] = dep.url;
                entry["integrity"] = dep.integrity;
            } else {
                // git dep
                entry["url"] = dep.url;
                entry["ref"] = dep.ref;
                entry["ref_type"] = dep.ref_type;
                entry["commit"] = dep.commit;
                entry["integrity"] = dep.integrity;
            }
            deps_obj[key] = std::move(entry);
        }

        doc["deps"] = std::move(deps_obj);

        std::string content = doc.dump(2) + "\n";
        return atomic_write_file(path, content);
    }

    // Find a remote dep entry matching the given URL and ref (tag).
    // Returns nullptr if no match.
    const LockDep* find_remote(const std::string& dep_url,
                                const std::string& dep_ref) const {
        std::string norm = normalize_dependency_url(dep_url);
        for (auto& [key, dep] : deps) {
            if (!dep.is_remote()) continue;
            if (normalize_dependency_url(dep.url) == norm &&
                dep.ref == dep_ref) {
                return &dep;
            }
        }
        return nullptr;
    }

    // Recursively check staleness: walk all manifests in the dependency
    // closure (following path deps), and verify every remote dep has a
    // matching lock entry with non-empty commit + integrity.
    bool is_consistent(const Manifest& manifest, const Path& root) const {
        std::set<Path> visited;
        return check_consistency(manifest, root, visited);
    }

  private:
    bool check_consistency(const Manifest& manifest, const Path& root,
                           std::set<Path>& visited) const {
        for (auto& [alias, dep] : manifest.dependencies) {
            if (dep.is_path_dep) {
                // Recurse into path dep manifests
                Path dep_dir = (manifest.project_dir / dep.path.c_str())
                                   .lexically_normal();
                Path dep_toml = dep_dir / "bake.toml";
                if (!dep_toml.is_regular_file()) continue;
                auto canonical = dep_dir.absolute();
                if (visited.insert(canonical).second) {
                    auto sub = Manifest::load(dep_dir);
                    if (sub && sub->has_moid()) {
                        sub->project_dir = dep_dir;
                        if (!check_consistency(*sub, root, visited))
                            return false;
                    }
                }
                continue;
            }

            // Remote dep: must have a lock entry with matching url+ref
            const LockDep* locked = find_remote(dep.url, dep.tag);
            if (!locked) return false;
            if (locked->commit.empty()) return false;
            if (locked->integrity.empty()) return false;
        }
        return true;
    }

  public:
    // True when the entire dependency closure (recursively following path
    // deps) contains only path dependencies — no remote deps anywhere.
    // In that case no lockfile is needed at all.
    static bool has_only_path_deps(const Manifest& manifest) {
        std::set<Path> visited;
        return closure_has_only_path_deps(manifest, visited);
    }

  private:
    static bool closure_has_only_path_deps(const Manifest& manifest,
                                            std::set<Path>& visited) {
        for (auto& [name, dep] : manifest.dependencies) {
            if (!dep.is_path_dep) return false;
            // Recurse into path dep manifests
            Path dep_dir = (manifest.project_dir / dep.path.c_str())
                               .lexically_normal();
            Path dep_toml = dep_dir / "bake.toml";
            if (!dep_toml.is_regular_file()) continue;
            auto canonical = dep_dir.absolute();
            if (visited.insert(canonical).second) {
                auto sub = Manifest::load(dep_dir);
                if (sub && sub->has_moid()) {
                    sub->project_dir = dep_dir;
                    if (!closure_has_only_path_deps(*sub, visited))
                        return false;
                }
            }
        }
        return true;
    }

    bool empty() const { return deps.empty(); }
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

// Get the root of the global toolchain cache (content-addressed).
// Honours BAKE_CACHE_DIR for testing; defaults to ~/.cache/bake.
export Path get_toolchain_cache_root() {
    if (const char* env = std::getenv("BAKE_CACHE_DIR"))
        if (env[0] != '\0') return Path(env);
    return get_cache_dir().parent();
}

} // namespace bake
