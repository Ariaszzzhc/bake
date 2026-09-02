export module bake.buildsystem.project;

import std;
import bake.util;
import bake.toolchain.target;
import tomlplusplus;
import nlohmann.json;

// ============================================================
// bake.buildsystem.project — bake.toml model, layout, project discovery
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

// ===== BuildOption (bool-only) =====

export struct BuildOption {
    bool value = false;

    bool operator==(const BuildOption&) const = default;
};

// ===== Dependency =====

// Direct-archive URL detection — the extension discriminates archive
// dependencies (tarball/zip fetched verbatim) from git dependencies.
export std::string archive_extension(std::string_view url) {
    constexpr std::string_view extensions[] = {
        ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tar.xz", ".txz", ".zip",
    };
    for (auto extension : extensions)
        if (ends_with(url, extension)) return std::string(extension);
    return {};
}

export bool is_archive_url(std::string_view url) {
    return !archive_extension(url).empty();
}

export struct Dependency {
    std::string name;
    std::string url;      // git repo URL or direct archive URL (empty for path deps)
    std::string tag;      // git ref: tag    (at most one ref key; none = default branch)
    std::string branch;   // git ref: branch
    std::string rev;      // git ref: commit
    std::string path;     // relative path (for path deps)
    bool is_path_dep = false;
    std::vector<std::string> options;  // feature names to enable

    bool is_remote() const { return !url.empty(); }
    // Direct archive dependency — recognized by URL extension.
    bool is_archive() const { return is_remote() && is_archive_url(url); }

    // The single git ref of this dependency: {"tag"|"branch"|"rev", value},
    // or {"head", ""} when unset (default branch HEAD).
    std::pair<std::string, std::string> git_ref() const {
        if (!tag.empty()) return {"tag", tag};
        if (!branch.empty()) return {"branch", branch};
        if (!rev.empty()) return {"rev", rev};
        return {"head", ""};
    }
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
    std::string cxx_std = "c++17";
    std::string c_std = "c17";
};

// ===== Profile / Target / Link / Sources config =====

export struct ProfileConfig {
    std::optional<int> opt_level;           // 0,1,2,3
    std::optional<std::string> opt_size;    // "s","z"
    std::optional<bool> debug;
    std::optional<std::string> debug_kind;  // "line-tables-only"
    std::optional<bool> lto;
    std::optional<std::string> lto_kind;    // "thin"
    std::optional<bool> strip;
    std::vector<std::string> sanitize;      // ["address"],...
    std::optional<std::string> warnings;    // "none","all","extra","error"

    bool any_set() const {
        return opt_level || opt_size || debug || debug_kind ||
               lto || lto_kind || strip || !sanitize.empty() || warnings;
    }
};

export struct TargetCondition {
    std::string triple_pattern;
    std::vector<std::string> libraries;
    std::vector<std::string> frameworks;
    std::vector<std::string> defines;
    std::vector<std::string> flags;
    std::vector<std::string> include_dirs;
    // Target-scoped dependencies: merged into the effective dependency set
    // of matching build triples ([target."<glob>".dependencies]).
    std::map<std::string, Dependency> dependencies;
};

// A target pattern is valid when every '*' spans a whole '-'-delimited
// segment ("*-linux-musl" yes, "x86*-linux" no).
export bool valid_target_pattern(std::string_view pattern) {
    std::size_t start = 0;
    for (std::size_t i = 0; i <= pattern.size(); ++i) {
        if (i == pattern.size() || pattern[i] == '-') {
            auto segment = pattern.substr(start, i - start);
            if (segment.find('*') != std::string_view::npos &&
                segment != "*")
                return false;
            start = i + 1;
        }
    }
    return true;
}

export struct LinkConfig {
    std::vector<std::string> libraries;
    std::vector<std::string> frameworks;
};

export struct SourceExtConfig {
    std::vector<std::string> module_ext = {".cppm", ".ixx"};
    std::vector<std::string> source_ext = {".cpp", ".cc", ".cxx", ".c"};
    std::vector<std::string> header_ext = {".h", ".hpp", ".hxx", ".hh"};
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
    std::map<std::string, ProfileConfig> profiles;
    std::vector<TargetCondition> targets;
    std::optional<LinkConfig> link;
    std::optional<SourceExtConfig> sources_config;

    bool is_workspace() const { return workspace.has_value(); }
    bool has_moid() const { return moid.has_value(); }

    ProfileConfig resolve_profile(const std::string& name) const {
        ProfileConfig base;
        if (name == "dev") {
            base.opt_level = 0;
            base.debug = true;
            base.warnings = "all";
        } else if (name == "release") {
            base.opt_level = 3;
            base.debug = false;
            base.lto = true;
            base.lto_kind = "thin";
            base.warnings = "all";
        }
        auto it = profiles.find(name);
        if (it != profiles.end()) {
            const auto& usr = it->second;
            if (usr.opt_level) { base.opt_size.reset(); base.opt_level = usr.opt_level; }
            if (usr.opt_size) { base.opt_level.reset(); base.opt_size = usr.opt_size; }
            if (usr.debug) { base.debug_kind.reset(); base.debug = usr.debug; }
            if (usr.debug_kind) { base.debug.reset(); base.debug_kind = usr.debug_kind; }
            if (usr.lto) { base.lto_kind.reset(); base.lto = usr.lto; }
            if (usr.lto_kind) { base.lto.reset(); base.lto_kind = usr.lto_kind; }
            if (usr.strip) base.strip = usr.strip;
            if (!usr.sanitize.empty()) base.sanitize = usr.sanitize;
            if (usr.warnings) base.warnings = usr.warnings;
        }
        return base;
    }

    SourceExtConfig resolve_source_ext() const {
        if (sources_config) return *sources_config;
        return SourceExtConfig{};
    }

    // Load a manifest if and only if the source declares a moid ([package]).
    // The single is-moid judgment shared by the resolver (lock identity)
    // and graph resolution (graph node vs. consumer source_deps).
    static std::optional<Manifest> load_moid(const Path& dir) {
        auto manifest = load(dir);
        if (!manifest || !manifest->has_moid()) return std::nullopt;
        return manifest;
    }

    // Whether the whole dependency closure (following path deps into their
    // manifests) declares no remote dependency. Lock resolution can be
    // skipped entirely in that case.
    static bool has_only_path_deps(const Manifest& manifest) {
        std::set<Path> visited;
        return closure_has_only_path_deps(manifest, visited);
    }

  private:
    static bool closure_has_only_path_deps(const Manifest& manifest,
                                           std::set<Path>& visited) {
        // Every declared scope (global + all [target.*] tables) counts.
        std::vector<const std::map<std::string, Dependency>*> scopes;
        scopes.push_back(&manifest.dependencies);
        for (const auto& condition : manifest.targets)
            scopes.push_back(&condition.dependencies);

        for (auto* scope : scopes) {
            for (auto& [name, dep] : *scope) {
                (void)name;
                if (dep.is_remote()) return false;
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
        }
        return true;
    }

  public:

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
            if (value.is_boolean()) {
                return BuildOption{*value.value<bool>()};
            }
            return std::nullopt;
        };

        auto parse_string_array = [](const toml::node& value)
                -> std::vector<std::string> {
            std::vector<std::string> result;
            if (auto* arr = value.as_array()) {
                for (auto& elem : *arr) {
                    if (auto s = elem.value<std::string>())
                        result.push_back(*s);
                }
            }
            return result;
        };


        // Parse one dependency entry — shared by the global [dependencies]
        // and every [target."<glob>".dependencies] scope.
        auto parse_dependency_entry =
            [&](const std::string& name, const toml::node& val)
                -> std::optional<Dependency> {
            auto* t = val.as_table();
            if (!t) {
                std::println(std::cerr,
                             "bake: dependency '{}' must be a table in {}",
                             name, toml_path.string());
                return std::nullopt;
            }
            Dependency d;
            d.name = name;
            if (auto v = (*t)["url"].value<std::string>())    d.url = *v;
            if (auto v = (*t)["tag"].value<std::string>())    d.tag = *v;
            if (auto v = (*t)["branch"].value<std::string>()) d.branch = *v;
            if (auto v = (*t)["rev"].value<std::string>())    d.rev = *v;
            if (auto v = (*t)["path"].value<std::string>()) {
                d.path = *v;
                d.is_path_dep = true;
            }
            if (auto* opts = (*t)["options"].as_array()) {
                for (auto& elem : *opts) {
                    if (auto s = elem.value<std::string>())
                        d.options.push_back(*s);
                }
            }

            if (d.is_path_dep) {
                if (d.path.empty() || !d.url.empty() ||
                    !d.tag.empty() || !d.branch.empty() || !d.rev.empty()) {
                    std::println(std::cerr,
                        "bake: path dependency '{}' must not mix "
                        "url/tag/branch/rev in {}",
                        name, toml_path.string());
                    return std::nullopt;
                }
                return d;
            }

            if (d.url.empty()) {
                std::println(std::cerr,
                    "bake: dependency '{}' requires url or path in {}",
                    name, toml_path.string());
                return std::nullopt;
            }

            const int ref_count = (!d.tag.empty()) + (!d.branch.empty()) +
                                  (!d.rev.empty());
            if (ref_count > 1) {
                std::println(std::cerr,
                    "bake: dependency '{}' in {} uses more than one of "
 "tag/branch/rev", name, toml_path.string());
                return std::nullopt;
            }
            if (is_archive_url(d.url) && ref_count > 0) {
                std::println(std::cerr,
                    "bake: archive dependency '{}' in {} must not use "
                    "tag/branch/rev (the URL pins the content)",
                    name, toml_path.string());
                return std::nullopt;
            }
            return d;
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
        if (auto* pkg_tbl = tbl["package"].as_table()) {
            Moid value;
            if (auto v = (*pkg_tbl)["name"].value<std::string>())
                value.name = *v;
            if (auto v = (*pkg_tbl)["version"].value<std::string>())
                value.version = *v;
            if (auto* type_node = pkg_tbl->get("type")) {
                auto token = type_node->value<std::string>();
                if (!token) {
                    std::println(std::cerr,
                                 "bake: package type must be a string in {}",
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
            // [language] is the sole source of language standards.
            // No [package] std shorthand.
            m.moid = std::move(value);
        }

        // [language] — cxx/c standard control (defaults: c++17 / c17)
        if (auto* lang_tbl = tbl["language"].as_table()) {
            if (!m.moid) m.moid = Moid{};
            if (auto v = (*lang_tbl)["cxx"].value<std::string>())
                m.moid->cxx_std = *v;
            if (auto v = (*lang_tbl)["c"].value<std::string>())
                m.moid->c_std = *v;
        }

        // [dependencies]
        if (auto* deps = tbl["dependencies"].as_table()) {
            for (auto& [key, val] : *deps) {
                auto parsed =
                    parse_dependency_entry(std::string(key.str()), val);
                if (!parsed) return std::nullopt;
                m.dependencies[parsed->name] = std::move(*parsed);
            }
        }

        // [options] — bool only
        if (auto* opts = tbl["options"].as_table()) {
            for (auto& [key, val] : *opts) {
                std::string opt_name = std::string(key.str());
                if (auto parsed = parse_option_value(val)) {
                    m.options[opt_name] = std::move(*parsed);
                } else {
                    std::println(std::cerr,
                                 "bake: option '{}' must be a boolean in {}",
                                 opt_name, toml_path.string());
                    return std::nullopt;
                }
            }
        }

        // [profile.<name>]
        if (auto* profiles_tbl = tbl["profile"].as_table()) {
            for (auto& [prof_key, prof_val] : *profiles_tbl) {
                auto* pt = prof_val.as_table();
                if (!pt) continue;
                ProfileConfig pc;
                if (auto* opt_node = pt->get("opt")) {
                    if (opt_node->is_integer())
                        pc.opt_level = static_cast<int>(
                            *opt_node->value<std::int64_t>());
                    else if (auto s = opt_node->value<std::string>())
                        pc.opt_size = *s;
                }
                if (auto* dbg_node = pt->get("debug")) {
                    if (auto b = dbg_node->value<bool>())
                        pc.debug = *b;
                    else if (auto s = dbg_node->value<std::string>())
                        pc.debug_kind = *s;
                }
                if (auto* lto_node = pt->get("lto")) {
                    if (auto b = lto_node->value<bool>())
                        pc.lto = *b;
                    else if (auto s = lto_node->value<std::string>())
                        pc.lto_kind = *s;
                }
                if (auto b = (*pt)["strip"].value<bool>()) pc.strip = *b;
                if (auto* san = pt->get("sanitize"))
                    pc.sanitize = parse_string_array(*san);
                if (auto s = (*pt)["warnings"].value<std::string>())
                    pc.warnings = *s;
                m.profiles[std::string(prof_key.str())] = std::move(pc);
            }
        }

        // [target."<pattern>"]
        if (auto* targets_tbl = tbl["target"].as_table()) {
            for (auto& [tgt_key, tgt_val] : *targets_tbl) {
                auto* tt = tgt_val.as_table();
                if (!tt) continue;
                TargetCondition tc;
                tc.triple_pattern = std::string(tgt_key.str());

                if (!valid_target_pattern(tc.triple_pattern)) {
                    std::println(std::cerr,
                        "bake: invalid target pattern '{}': "
                        "'*' must match a whole segment",
                        tc.triple_pattern);
                    return std::nullopt;
                }
                if (auto* libs = (*tt)["libraries"].as_array())
                    for (auto& e : *libs)
                        if (auto s = e.value<std::string>()) tc.libraries.push_back(*s);
                if (auto* fws = (*tt)["frameworks"].as_array())
                    for (auto& e : *fws)
                        if (auto s = e.value<std::string>()) tc.frameworks.push_back(*s);
                if (auto* defs = (*tt)["defines"].as_array())
                    for (auto& e : *defs)
                        if (auto s = e.value<std::string>()) tc.defines.push_back(*s);
                if (auto* flags = (*tt)["flags"].as_array())
                    for (auto& e : *flags)
                        if (auto s = e.value<std::string>()) tc.flags.push_back(*s);
                if (auto* incs = (*tt)["include_dirs"].as_array())
                    for (auto& e : *incs)
                        if (auto s = e.value<std::string>()) tc.include_dirs.push_back(*s);
                if (auto* tdeps = (*tt)["dependencies"].as_table()) {
                    for (auto& [key, val] : *tdeps) {
                        auto parsed =
                            parse_dependency_entry(std::string(key.str()), val);
                        if (!parsed) return std::nullopt;
                        tc.dependencies[parsed->name] = std::move(*parsed);
                    }
                }
                m.targets.push_back(std::move(tc));
            }
        }

        // [link]
        if (auto* link_tbl = tbl["link"].as_table()) {
            LinkConfig lc;
            if (auto* libs = (*link_tbl)["libraries"].as_array())
                for (auto& e : *libs)
                    if (auto s = e.value<std::string>()) lc.libraries.push_back(*s);
            if (auto* fws = (*link_tbl)["frameworks"].as_array())
                for (auto& e : *fws)
                    if (auto s = e.value<std::string>()) lc.frameworks.push_back(*s);
            m.link = std::move(lc);
        }

        // [sources]
        if (auto* src_tbl = tbl["sources"].as_table()) {
            SourceExtConfig sec;
            if (auto* me = (*src_tbl)["module_ext"].as_array())
                sec.module_ext = parse_string_array(*me);
            if (auto* se = (*src_tbl)["source_ext"].as_array())
                sec.source_ext = parse_string_array(*se);
            if (auto* he = (*src_tbl)["header_ext"].as_array())
                sec.header_ext = parse_string_array(*he);
            m.sources_config = std::move(sec);
        }

        return m;
    }
};

namespace {

bool same_definition(const Dependency& a, const Dependency& b) {
    return a.url == b.url && a.tag == b.tag && a.branch == b.branch &&
           a.rev == b.rev && a.path == b.path && a.options == b.options;
}

}  // namespace

// Effective dependency set for a build triple: the global [dependencies]
// plus every [target."<glob>".dependencies] table whose pattern matches the
// triple, applied in declaration order. A repeated alias with an identical
// definition deduplicates; a different definition is an error naming both
// scopes. Scopes that do not match the triple are invisible.
export std::expected<std::map<std::string, Dependency>, std::string>
effective_dependencies(const Manifest& manifest, const std::string& triple) {
    std::map<std::string, Dependency> effective = manifest.dependencies;
    std::map<std::string, std::string> scope_of;
    for (auto& [alias, dep] : manifest.dependencies) {
        (void)dep;
        scope_of[alias] = "[dependencies]";
    }

    for (const auto& condition : manifest.targets) {
        if (!triple_matches(triple, condition.triple_pattern)) continue;
        const std::string scope =
            "[target.\"" + condition.triple_pattern + "\".dependencies]";
        for (const auto& [alias, dep] : condition.dependencies) {
            auto existing = effective.find(alias);
            if (existing == effective.end()) {
                effective.emplace(alias, dep);
                scope_of[alias] = scope;
                continue;
            }
            if (same_definition(existing->second, dep)) continue;
            return std::unexpected(
                "dependency '" + alias +
                "' has conflicting definitions in " + scope_of[alias] +
                " and " + scope);
        }
    }
    return effective;
}

// ===== Default source layout =====
//
// Default input discovery uses src/ for implementation and public/ for headers
// and module interfaces. All build outputs go under out/ and are managed by the
// build graph — Layout only describes source-side structure.

export struct Layout {
    Path root;          // source root (contains bake.toml)
    Path source_dir;    // root/src/
    Path public_dir;    // root/public/

    static Layout detect(const Path& root) {
        return Layout{
            root,
            root / "src",
            root / "public",
        };
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
