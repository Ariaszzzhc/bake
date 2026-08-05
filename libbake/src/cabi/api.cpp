// ============================================================
// bake C ABI implementation
//
// Bridges the C API defined in bake_cabi.h to the C++23 module
// layer (bake.util, bake.project, bake.compiler).
//
// All extern "C" functions are noexcept.  Exceptions are caught
// internally and converted to return codes (0 / NULL) plus a
// thread-local error string retrievable via bake_last_error().
// ============================================================

#include <nlohmann/json.hpp>

import std;
import bake.util;
import bake.project;
import bake.compiler;
import bake.foreign;

#include "bake_cabi.h"

// ============================================================
// Thread-local error
// ============================================================

namespace {

thread_local std::string g_last_error;

void set_error(const std::string& msg) {
    g_last_error = msg;
}

// ============================================================
// Internal types
// ============================================================

enum class TargetType {
    Executable,
    StaticLib,
    SharedLib,
};

} // anonymous namespace

// ---- struct bake_builder (opaque handle) ----

struct bake_builder {
    std::optional<bake::Manifest> manifest;
    std::optional<bake::Lockfile> lockfile;
    bake::Toolchain toolchain;
    bake::Layout layout;

    // Cached directory strings (stable c_str() for the C API)
    std::string source_dir_str;
    std::string build_dir_str;

    std::vector<std::unique_ptr<bake_target>> targets;
    std::vector<std::unique_ptr<bake_step>> steps;
    std::vector<std::unique_ptr<bake_dependency>> deps;
};

// ---- struct bake_target (opaque handle) ----

struct bake_target {
    std::string name;
    TargetType type = TargetType::Executable;
    std::string std_ver = "c++20";

    std::vector<std::string> source_patterns;
    std::vector<std::string> include_dirs;
    std::vector<std::pair<std::string, std::string>> defines;

    std::vector<bake_target*> linked_targets;     // other bake targets
    std::vector<std::string> system_libs;          // -l flags
    std::vector<std::string> external_libs;        // full paths to .a/.so files

    std::vector<bake_step*> depends_on_steps;
};

// ---- struct bake_step (opaque handle) ----

struct bake_step {
    std::string name;
    std::string command;               // executable
    std::vector<std::string> args;     // arguments
    std::vector<std::string> outputs;  // declared output paths
};

// ---- struct bake_dependency (opaque handle) ----

struct bake_dependency {
    std::string name;
    bake_builder* builder = nullptr;   // back-pointer for manifest/lockfile lookup
    std::vector<std::pair<std::string, std::string>> cmake_defines;  // accumulated via .define()
    // Cached source directory (populated lazily)
    mutable std::string cached_src_dir;
    mutable bool src_dir_resolved = false;
};

// ---- struct bake_usage (opaque handle) ----

struct bake_usage {
    std::vector<std::string> includes;
    std::vector<std::string> defines;
    std::vector<std::string> links;
    // Null-terminated C string arrays for the C API returns
    std::vector<const char*> includes_c;
    std::vector<const char*> defines_c;
    std::vector<const char*> links_c;
};

// ============================================================
// JSON helper
// ============================================================

namespace {

nlohmann::json to_json_array(const std::vector<std::string>& vec) {
    auto arr = nlohmann::json::array();
    for (const auto& s : vec)
        arr.push_back(s);
    return arr;
}

// Convert an absolute filesystem path to one relative to the project root.
std::string rel_to_root(const std::filesystem::path& p,
                        const std::filesystem::path& root) {
    return std::filesystem::relative(p, root).string();
}

} // anonymous namespace

// ============================================================
// Error
// ============================================================

BAKE_API const char* bake_last_error(void) noexcept {
    if (g_last_error.empty())
        return nullptr;
    return g_last_error.c_str();
}

// ============================================================
// Builder — lifecycle
// ============================================================

BAKE_API bake_builder* bake_builder_new(void) noexcept {
    try {
        auto b = std::make_unique<bake_builder>();

        // Locate the project root (walks up from cwd to find bake.toml).
        auto root = bake::find_project_root();
        if (!root)
            root = bake::Path::current();

        // Load the manifest if present.
        b->manifest = bake::Manifest::load(*root);

        // Load the lockfile if present (for resolving remote dep source dirs).
        b->lockfile = bake::Lockfile::load(*root / "bake.lock");

        // Detect toolchain.
        b->toolchain = bake::Toolchain::detect();

        // Establish the directory layout.
        b->layout = bake::Layout::detect(*root);

        // Cache directory strings for the C API getters.
        b->source_dir_str = b->layout.source_dir.string();
        b->build_dir_str  = b->layout.bake_dir.string();

        return b.release();
    } catch (const std::exception& e) {
        set_error(std::string("bake_builder_new: ") + e.what());
        return nullptr;
    }
}

BAKE_API void bake_builder_free(bake_builder* b) noexcept {
    delete b;
}

// ============================================================
// Builder — build
// ============================================================

BAKE_API int bake_builder_build(bake_builder* b) noexcept {
    if (!b) {
        set_error("bake_builder_build: null builder");
        return -1;
    }

    try {
        // Ensure output directories exist.
        b->layout.bake_dir.mkdir_recursive();
        b->layout.obj_dir.mkdir_recursive();
        b->layout.bin_dir.mkdir_recursive();
        b->layout.lib_dir.mkdir_recursive();

        auto actions = nlohmann::json::array();

        // ---------------------------------------------------
        // Phase 1: emit custom actions for steps.
        // Targets may declare depends_on_step(name), so step
        // actions must appear before target actions.
        // ---------------------------------------------------
        for (const auto& step : b->steps) {
            nlohmann::json action;
            action["type"]       = "custom";
            action["id"]         = step->name;
            action["inputs"]     = nlohmann::json::array();
            action["outputs"]    = to_json_array(step->outputs);

            // command = [executable, arg0, arg1, ...]
            auto cmd = nlohmann::json::array();
            if (!step->command.empty())
                cmd.push_back(step->command);
            for (const auto& a : step->args)
                cmd.push_back(a);
            action["command"]    = cmd;
            action["depends_on"] = nlohmann::json::array();

            actions.push_back(std::move(action));
        }

        // ---------------------------------------------------
        // Phase 2: compile + link/archive actions per target.
        // ---------------------------------------------------
        for (const auto& target : b->targets) {
            std::vector<std::string> compile_ids;
            std::vector<std::string> obj_rel_paths;

            // -- Expand source glob patterns --
            for (const auto& pattern : target->source_patterns) {
                auto files = bake::glob(b->layout.root, pattern);

                for (const auto& src : files) {
                    std::string src_rel = rel_to_root(src.fs(), b->layout.root.fs());
                    std::string stem    = src.stem_string();
                    std::string obj_rel = "out/obj/" + stem + ".o";
                    std::string comp_id = target->name + "__" + src.filename_string();

                    compile_ids.push_back(comp_id);
                    obj_rel_paths.push_back(obj_rel);

                    // Build compile configuration.
                    bake::CompileConfig cc;
                    cc.source  = bake::Path(src_rel);
                    cc.output  = bake::Path(obj_rel);
                    cc.std_ver = target->std_ver;
                    cc.use_pic = (target->type == TargetType::SharedLib);

                    for (const auto& inc : target->include_dirs)
                        cc.include_dirs.push_back(bake::Path(inc));

                    for (const auto& [name, value] : target->defines)
                        cc.defines.push_back({name, value});

                    auto cmd = bake::make_compile_command(b->toolchain, cc);

                    nlohmann::json action;
                    action["type"]       = "compile";
                    action["id"]         = comp_id;
                    action["inputs"]     = to_json_array({src_rel});
                    action["outputs"]    = to_json_array({obj_rel});
                    action["command"]    = to_json_array(cmd);
                    action["depends_on"] = nlohmann::json::array();

                    actions.push_back(std::move(action));
                }
            }

            // -- Link / archive action --
            if (!obj_rel_paths.empty()) {
                bake::PackageType pkg_type;
                std::string type_str;

                switch (target->type) {
                    case TargetType::Executable:
                        pkg_type = bake::PackageType::Executable;
                        type_str = "link";
                        break;
                    case TargetType::StaticLib:
                        pkg_type = bake::PackageType::StaticLib;
                        type_str = "archive";
                        break;
                    case TargetType::SharedLib:
                        pkg_type = bake::PackageType::SharedLib;
                        type_str = "link";
                        break;
                }

                std::string out_name = bake::library_name(target->name, pkg_type);
                std::string out_subdir = (target->type == TargetType::Executable) ? "bin" : "lib";
                std::string out_rel  = "out/" + out_subdir + "/" + out_name;

                // Build link configuration.
                bake::LinkConfig lc;
                for (const auto& obj : obj_rel_paths)
                    lc.inputs.push_back(bake::Path(obj));
                lc.output = bake::Path(out_rel);
                lc.type   = pkg_type;

                for (const auto& lib : target->system_libs)
                    lc.link_libs.push_back(lib);

                // External library files (e.g. from CMake deps) are added
                // as additional link inputs — full paths to .a/.so files.
                for (const auto& elib : target->external_libs) {
                    std::string elib_rel = rel_to_root(
                        bake::Path(elib).fs(), b->layout.root.fs());
                    lc.inputs.push_back(bake::Path(elib_rel));
                }

                std::vector<std::string> cmd;
                if (target->type == TargetType::StaticLib)
                    cmd = bake::make_archive_command(b->toolchain, lc);
                else
                    cmd = bake::make_link_command(b->toolchain, lc);

                // depends_on: all compile actions for this target,
                // plus any steps the target depends on.
                std::vector<std::string> depends;
                for (const auto& id : compile_ids)
                    depends.push_back(id);
                for (const auto* step : target->depends_on_steps)
                    depends.push_back(step->name);

                nlohmann::json action;
                action["type"]       = type_str;
                action["id"]         = target->name + "__link";
                action["inputs"]     = to_json_array(obj_rel_paths);
                action["outputs"]    = to_json_array({out_rel});
                action["command"]    = to_json_array(cmd);
                action["depends_on"] = to_json_array(depends);

                actions.push_back(std::move(action));
            }
        }

        // ---------------------------------------------------
        // Phase 3: serialize to .bake/build.json
        // ---------------------------------------------------
        nlohmann::json build_json;
        build_json["actions"] = std::move(actions);

        bake::Path out_path = b->layout.bake_dir / "build.json";
        std::string content = build_json.dump(2);

        if (!bake::write_file(out_path, content)) {
            set_error("bake_builder_build: failed to write " + out_path.string());
            return -1;
        }

        return 0;
    } catch (const std::exception& e) {
        set_error(std::string("bake_builder_build: ") + e.what());
        return -1;
    }
}

// ============================================================
// Builder — target factories
// ============================================================

BAKE_API bake_target* bake_builder_executable(bake_builder* b, const char* name) noexcept {
    if (!b || !name) { set_error("bake_builder_executable: null argument"); return nullptr; }
    try {
        auto t = std::make_unique<bake_target>();
        t->name = name;
        t->type = TargetType::Executable;
        auto* raw = t.get();
        b->targets.push_back(std::move(t));
        return raw;
    } catch (const std::exception& e) {
        set_error(std::string("bake_builder_executable: ") + e.what());
        return nullptr;
    }
}

BAKE_API bake_target* bake_builder_static_lib(bake_builder* b, const char* name) noexcept {
    if (!b || !name) { set_error("bake_builder_static_lib: null argument"); return nullptr; }
    try {
        auto t = std::make_unique<bake_target>();
        t->name = name;
        t->type = TargetType::StaticLib;
        auto* raw = t.get();
        b->targets.push_back(std::move(t));
        return raw;
    } catch (const std::exception& e) {
        set_error(std::string("bake_builder_static_lib: ") + e.what());
        return nullptr;
    }
}

BAKE_API bake_target* bake_builder_shared_lib(bake_builder* b, const char* name) noexcept {
    if (!b || !name) { set_error("bake_builder_shared_lib: null argument"); return nullptr; }
    try {
        auto t = std::make_unique<bake_target>();
        t->name = name;
        t->type = TargetType::SharedLib;
        auto* raw = t.get();
        b->targets.push_back(std::move(t));
        return raw;
    } catch (const std::exception& e) {
        set_error(std::string("bake_builder_shared_lib: ") + e.what());
        return nullptr;
    }
}

// ============================================================
// Builder — step factory
// ============================================================

BAKE_API bake_step* bake_builder_step(bake_builder* b, const char* name) noexcept {
    if (!b || !name) { set_error("bake_builder_step: null argument"); return nullptr; }
    try {
        auto s = std::make_unique<bake_step>();
        s->name = name;
        auto* raw = s.get();
        b->steps.push_back(std::move(s));
        return raw;
    } catch (const std::exception& e) {
        set_error(std::string("bake_builder_step: ") + e.what());
        return nullptr;
    }
}

// ============================================================
// Builder — dependency factory
// ============================================================

BAKE_API bake_dependency* bake_builder_dependency(bake_builder* b, const char* name) noexcept {
    if (!b || !name) { set_error("bake_builder_dependency: null argument"); return nullptr; }
    try {
        auto d = std::make_unique<bake_dependency>();
        d->name = name;
        d->builder = b;
        auto* raw = d.get();
        b->deps.push_back(std::move(d));
        return raw;
    } catch (const std::exception& e) {
        set_error(std::string("bake_builder_dependency: ") + e.what());
        return nullptr;
    }
}

// ============================================================
// Builder — option / directory accessors
// ============================================================

BAKE_API int bake_builder_option_bool(const bake_builder* b, const char* name) noexcept {
    if (!b || !name || !b->manifest) return 0;
    auto it = b->manifest->options.find(name);
    if (it == b->manifest->options.end()) return 0;
    if (it->second.type != bake::BuildOption::Type::Bool) return 0;
    return it->second.bool_value ? 1 : 0;
}

BAKE_API int64_t bake_builder_option_int(const bake_builder* b, const char* name) noexcept {
    if (!b || !name || !b->manifest) return 0;
    auto it = b->manifest->options.find(name);
    if (it == b->manifest->options.end()) return 0;
    if (it->second.type != bake::BuildOption::Type::Int) return 0;
    return it->second.int_value;
}

BAKE_API const char* bake_builder_option_str(const bake_builder* b, const char* name) noexcept {
    if (!b || !name || !b->manifest) return nullptr;
    auto it = b->manifest->options.find(name);
    if (it == b->manifest->options.end()) return nullptr;
    if (it->second.type != bake::BuildOption::Type::String) return nullptr;
    return it->second.str_value.c_str();
}

BAKE_API const char* bake_builder_source_dir(const bake_builder* b) noexcept {
    if (!b) return nullptr;
    return b->source_dir_str.c_str();
}

BAKE_API const char* bake_builder_build_dir(const bake_builder* b) noexcept {
    if (!b) return nullptr;
    return b->build_dir_str.c_str();
}

// ============================================================
// Target — fluent configuration
// ============================================================

BAKE_API bake_target* bake_target_std(bake_target* t, const char* std_ver) noexcept {
    if (!t || !std_ver) return t;
    try {
        t->std_ver = std_ver;
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_std: ") + e.what());
    }
    return t;
}

BAKE_API bake_target* bake_target_sources(bake_target* t, const char* pattern) noexcept {
    if (!t || !pattern) return t;
    try {
        t->source_patterns.push_back(pattern);
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_sources: ") + e.what());
    }
    return t;
}

BAKE_API bake_target* bake_target_include_dirs(bake_target* t, const char* dirs) noexcept {
    if (!t || !dirs) return t;
    try {
        t->include_dirs.push_back(dirs);
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_include_dirs: ") + e.what());
    }
    return t;
}

BAKE_API bake_target* bake_target_define(bake_target* t, const char* name, const char* value) noexcept {
    if (!t || !name) return t;
    try {
        t->defines.push_back({name, value ? value : ""});
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_define: ") + e.what());
    }
    return t;
}

BAKE_API bake_target* bake_target_link(bake_target* t, bake_target* other) noexcept {
    if (!t || !other) return t;
    try {
        t->linked_targets.push_back(other);
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_link: ") + e.what());
    }
    return t;
}

BAKE_API bake_target* bake_target_link_system(bake_target* t, const char* lib) noexcept {
    if (!t || !lib) return t;
    try {
        t->system_libs.push_back(lib);
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_link_system: ") + e.what());
    }
    return t;
}

BAKE_API bake_target* bake_target_depends_on_step(bake_target* t, bake_step* step) noexcept {
    if (!t || !step) return t;
    try {
        t->depends_on_steps.push_back(step);
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_depends_on_step: ") + e.what());
    }
    return t;
}

// ============================================================
// Step — configuration
// ============================================================

BAKE_API bake_step* bake_step_outputs(bake_step* s, const char* outputs) noexcept {
    if (!s || !outputs) return s;
    try {
        s->outputs.push_back(outputs);
    } catch (const std::exception& e) {
        set_error(std::string("bake_step_outputs: ") + e.what());
    }
    return s;
}

BAKE_API bake_step* bake_step_run(bake_step* s, const char* command,
                                  const char* const* args, int nargs) noexcept {
    if (!s) return s;
    try {
        if (command)
            s->command = command;
        s->args.clear();
        for (int i = 0; i < nargs; ++i) {
            if (args[i])
                s->args.push_back(args[i]);
        }
    } catch (const std::exception& e) {
        set_error(std::string("bake_step_run: ") + e.what());
    }
    return s;
}

// ============================================================
// Dependency — Phase 4 CMake bridge
// ============================================================

// Resolve a dependency's source directory.
// - Path deps: manifest.project_dir / dep.path
// - Remote deps: cache_dir / lockfile_node.tree_sha256
BAKE_API const char* bake_dep_src_dir(const bake_dependency* d) noexcept {
    if (!d || !d->builder || !d->builder->manifest) return nullptr;
    try {
        if (d->src_dir_resolved) {
            return d->cached_src_dir.empty() ? nullptr : d->cached_src_dir.c_str();
        }

        d->src_dir_resolved = true;

        auto& manifest = *d->builder->manifest;
        auto dep_it = manifest.dependencies.find(d->name);
        if (dep_it == manifest.dependencies.end()) return nullptr;

        auto& dep = dep_it->second;
        if (dep.is_path_dep) {
            const_cast<bake_dependency*>(d)->cached_src_dir =
                (manifest.project_dir / dep.path).string();
        } else {
            // Remote dep: look up in lockfile
            if (!d->builder->lockfile) return nullptr;
            auto root_it = d->builder->lockfile->root_deps.find(d->name);
            if (root_it == d->builder->lockfile->root_deps.end()) return nullptr;
            auto node_it = d->builder->lockfile->nodes.find(root_it->second);
            if (node_it == d->builder->lockfile->nodes.end()) return nullptr;
            const_cast<bake_dependency*>(d)->cached_src_dir =
                (bake::get_cache_dir() / node_it->second.tree_sha256).string();
        }

        return d->cached_src_dir.empty() ? nullptr : d->cached_src_dir.c_str();
    } catch (const std::exception& e) {
        set_error(std::string("bake_dep_src_dir: ") + e.what());
        return nullptr;
    }
}

// Link a dependency's build output to a target.
// For now this is a no-op — usage requirements are applied via bake_dep_cmake_build.
BAKE_API void bake_dep_link_to(bake_dependency* d, bake_target* t) noexcept {
    if (!d || !t) return;
    try {
        // Resolve the dep's source dir and look for build artifacts.
        const char* src = bake_dep_src_dir(d);
        if (!src) return;

        // Check if this is a bake-native dep with build output
        // (For Phase 4, link_to is primarily for bake-native deps.
        // CMake deps use build_with_cmake().expose_to() instead.)
        // TODO: implement bake-native dep linking in a future iteration.
    } catch (const std::exception& e) {
        set_error(std::string("bake_dep_link_to: ") + e.what());
    }
}

// Run the CMake bridge: configure → build → install → extract usage requirements.
// Results are applied directly to the consumer target.
BAKE_API bake_usage* bake_dep_cmake_build(bake_dependency* d, bake_target* consumer,
                                          const char* const* keys,
                                          const char* const* vals,
                                          int ndefines) noexcept {
    if (!d || !consumer) {
        set_error("bake_dep_cmake_build: null argument");
        return nullptr;
    }
    try {
        // Resolve source directory
        const char* src = bake_dep_src_dir(d);
        if (!src || src[0] == '\0') {
            set_error(std::string("bake_dep_cmake_build: cannot resolve source dir for '") +
                      d->name + "'");
            return nullptr;
        }

        // Determine staging directory: <project_root>/.bake/staging/<dep_name>/
        bake::Path staging = d->builder->layout.bake_dir / "staging" / d->name;
        staging.mkdir_recursive();

        // Build CMakeConfig
        bake::CMakeConfig config;
        config.source_dir = bake::Path(src);
        config.staging_dir = staging;
        config.toolchain = &d->builder->toolchain;

        // Merge defines from the wrapper's .define() calls
        for (int i = 0; i < ndefines; ++i) {
            if (keys[i] && vals[i]) {
                config.defines.push_back({keys[i], vals[i]});
            }
        }

        // Run the CMake bridge
        std::println("bake: building CMake dependency '{}'...", d->name);
        auto usage = bake::run_cmake_bridge(config);
        if (!usage) {
            set_error(std::string("bake_dep_cmake_build: CMake bridge failed for '") +
                      d->name + "'");
            return nullptr;
        }

        // Apply usage requirements to the consumer target
        for (const auto& inc : usage->includes) {
            consumer->include_dirs.push_back(inc);
        }
        for (const auto& def : usage->defines) {
            // Defines come as "KEY=VALUE" strings
            auto eq = def.find('=');
            if (eq != std::string::npos) {
                consumer->defines.push_back({def.substr(0, eq), def.substr(eq + 1)});
            } else {
                consumer->defines.push_back({def, ""});
            }
        }
        for (const auto& lib : usage->libraries) {
            consumer->external_libs.push_back(lib.string());
        }

        // Build the bake_usage return struct
        auto u = std::make_unique<bake_usage>();
        u->includes = usage->includes;
        u->defines = usage->defines;
        for (const auto& lib : usage->libraries) {
            u->links.push_back(lib.string());
        }

        // Populate null-terminated C string arrays
        for (const auto& s : u->includes) u->includes_c.push_back(s.c_str());
        u->includes_c.push_back(nullptr);
        for (const auto& s : u->defines) u->defines_c.push_back(s.c_str());
        u->defines_c.push_back(nullptr);
        for (const auto& s : u->links) u->links_c.push_back(s.c_str());
        u->links_c.push_back(nullptr);

        return u.release();
    } catch (const std::exception& e) {
        set_error(std::string("bake_dep_cmake_build: ") + e.what());
        return nullptr;
    }
}

// ============================================================
// Usage requirements
// ============================================================

BAKE_API const char* const* bake_usage_includes(bake_usage* u) noexcept {
    if (!u) return nullptr;
    return u->includes_c.data();
}

BAKE_API const char* const* bake_usage_defines(bake_usage* u) noexcept {
    if (!u) return nullptr;
    return u->defines_c.data();
}

BAKE_API const char* const* bake_usage_links(bake_usage* u) noexcept {
    if (!u) return nullptr;
    return u->links_c.data();
}

BAKE_API void bake_usage_free(bake_usage* u) noexcept {
    delete u;
}
