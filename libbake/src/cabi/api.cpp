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
import bake.engine;

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
    std::vector<std::string> configuration_errors;
};

// ---- struct bake_target (opaque handle) ----

// A source glob pattern with optional per-source compile flags.
struct source_entry {
    std::string pattern;
    std::vector<std::string> flags;  // extra compiler flags for matched files
};

struct bake_target {
    std::string name;
    TargetType type = TargetType::Executable;
    std::string std_ver = "c++20";

    std::vector<source_entry> source_entries;
    std::vector<std::string> include_dirs;
    std::vector<std::string> private_include_dirs;
    std::vector<std::pair<std::string, std::string>> defines;
    std::vector<std::pair<std::string, std::string>> private_defines;

    std::vector<bake_target*> linked_targets;     // other bake targets
    std::vector<std::string> system_libs;          // -l flags
    std::vector<std::string> frameworks;           // Apple frameworks

    // Usage requirements imported from Bake-native package dependencies.
    // Static libraries propagate these to their consumers; executables and
    // shared libraries consume them in their own link action.
    std::vector<std::string> dependency_include_dirs;
    std::vector<std::pair<std::string, std::string>> dependency_defines;
    std::vector<std::string> dependency_link_inputs;
    std::vector<std::string> dependency_system_libs;
    std::vector<std::string> dependency_frameworks;
    bool dependency_uses_cxx = false;

    // Populated while serializing the build graph and then exported through
    // out/.pkgs/<package>/package.json for consumers.
    std::string output_path;
    bool uses_cxx = false;

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
    // Cached source directory (populated lazily)
    mutable std::string cached_src_dir;
    mutable bool src_dir_resolved = false;
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

template <typename T>
void append_unique(std::vector<T>& values, const T& value) {
    if (std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

// Convert an absolute filesystem path to one relative to the project root.
std::string rel_to_root(const std::filesystem::path& p,
                        const std::filesystem::path& root) {
    std::error_code ec;
    auto relative = std::filesystem::relative(p, root, ec);
    if (!ec && !relative.empty())
        return relative.lexically_normal().generic_string();
    return p.lexically_normal().generic_string();
}

std::string build_action_id(std::string_view kind,
                            std::string_view package,
                            std::string_view target,
                            std::string_view identity = {}) {
    std::string result = std::string(kind) + ":" + std::string(package) +
                         ":" + std::string(target);
    if (!identity.empty()) result += ":" + std::string(identity);
    return result;
}

std::string action_owner(std::string_view package, std::string_view target) {
    if (package == target) return std::string(package);
    return std::string(package) + "/" + std::string(target);
}

std::optional<std::string> relative_if_within(
    const std::filesystem::path& path,
    const std::filesystem::path& root) {
    std::error_code ec;
    auto relative = std::filesystem::relative(path, root, ec);
    if (ec || relative.empty()) return std::nullopt;
    relative = relative.lexically_normal();
    if (*relative.begin() == "..") return std::nullopt;
    return relative.generic_string();
}

struct SourceActionNames {
    std::string identity;
    std::string display;
};

SourceActionNames source_action_names(const bake_builder& builder,
                                      const bake::Path& source) {
    if (auto local = relative_if_within(
            source.fs(), builder.layout.root.fs())) {
        auto display = relative_if_within(
            source.fs(), builder.layout.source_dir.fs());
        return {*local, display.value_or(*local)};
    }

    for (const auto& dependency : builder.deps) {
        if (!dependency->src_dir_resolved ||
            dependency->cached_src_dir.empty()) {
            continue;
        }
        if (auto relative = relative_if_within(
                source.fs(), dependency->cached_src_dir)) {
            return {"dependency/" + dependency->name + "/" + *relative,
                    *relative};
        }
    }

    return {source.fs().lexically_normal().generic_string(),
            source.filename_string()};
}

std::vector<bake::Path> expand_source_pattern(
    const bake::Path& project_root, std::string_view pattern) {
    namespace fs = std::filesystem;

    fs::path pattern_path{std::string(pattern)};
    if (!pattern_path.is_absolute()) {
        return bake::glob(project_root, pattern);
    }

    pattern_path = pattern_path.lexically_normal();
    fs::path search_root = pattern_path.root_path();
    fs::path relative_pattern;
    bool found_glob = false;

    for (const auto& component : pattern_path.relative_path()) {
        const std::string text = component.generic_string();
        if (!found_glob && text.find_first_of("*?") == std::string::npos) {
            search_root /= component;
        } else {
            found_glob = true;
            relative_pattern /= component;
        }
    }

    if (!found_glob) {
        if (fs::is_regular_file(pattern_path)) {
            return {bake::Path(pattern_path)};
        }
        return {};
    }

    return bake::glob(bake::Path(search_root), relative_pattern.generic_string());
}

std::string absolute_from_root(const bake::Path& root, std::string_view value) {
    std::filesystem::path path{std::string(value)};
    if (!path.is_absolute()) path = root.fs() / path;
    return std::filesystem::absolute(path).lexically_normal().string();
}

std::optional<std::string> dependency_relative_path(
    bake_dependency* dependency,
    std::string_view value,
    bool require_raw_source) {
    if (!dependency || !dependency->builder) return std::nullopt;

    const char* source_dir = bake_dep_src_dir(dependency);
    if (!source_dir) {
        dependency->builder->configuration_errors.push_back(
            "dependency '" + dependency->name + "' is not declared or resolved");
        return std::nullopt;
    }

    std::filesystem::path relative{std::string(value)};
    if (relative.is_absolute()) {
        dependency->builder->configuration_errors.push_back(
            "dependency-relative path for '" + dependency->name +
            "' must not be absolute: " + std::string(value));
        return std::nullopt;
    }
    relative = relative.lexically_normal();
    if (!relative.empty() && *relative.begin() == "..") {
        dependency->builder->configuration_errors.push_back(
            "dependency-relative path for '" + dependency->name +
            "' escapes its source root: " + std::string(value));
        return std::nullopt;
    }

    std::filesystem::path root{source_dir};
    if (require_raw_source &&
        std::filesystem::is_regular_file(root / "bake.toml")) {
        dependency->builder->configuration_errors.push_back(
            "dependency '" + dependency->name +
            "' is a Bake package; use link_to() instead of compiling its sources");
        return std::nullopt;
    }

    return (root / relative).lexically_normal().string();
}

nlohmann::json defines_to_json(
    const std::vector<std::pair<std::string, std::string>>& defines) {
    auto result = nlohmann::json::array();
    for (const auto& [name, value] : defines) {
        result.push_back({{"name", name}, {"value", value}});
    }
    return result;
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

        // The CLI supplies source/output ownership explicitly when it launches
        // build_app. Falling back to cwd keeps manually executed build scripts
        // usable without permitting dependencies to write into their sources.
        std::optional<bake::Path> root;
        if (const char* configured_root =
                std::getenv("BAKE_INTERNAL_SOURCE_ROOT");
            configured_root && *configured_root) {
            root = bake::Path(configured_root);
        } else {
            root = bake::find_project_root();
            if (!root) root = bake::Path::current();
        }

        // Load the manifest if present.
        b->manifest = bake::Manifest::load(*root);

        // Establish the directory layout before reading any process-boundary
        // files. A dependency's source root and mutable output root differ.
        const char* configured_out =
            std::getenv("BAKE_INTERNAL_PROJECT_OUT");
        const char* configured_package =
            std::getenv("BAKE_INTERNAL_PACKAGE_NAME");
        if (configured_out && *configured_out &&
            configured_package && *configured_package) {
            b->layout = bake::Layout::for_dependency(
                *root, bake::Path(configured_out), configured_package);
            if (b->manifest && b->manifest->package &&
                b->manifest->package->name != configured_package) {
                b->configuration_errors.push_back(
                    "build context package '" +
                    std::string(configured_package) +
                    "' does not match manifest package '" +
                    b->manifest->package->name + "'");
            }
        } else if (configured_out && *configured_out) {
            // Workspace member: output goes under the workspace root's out/.
            // configured_out = ws_root/out, so ws_root = parent.
            bake::Path ws_root(configured_out);
            ws_root = ws_root.parent();
            b->layout = bake::Layout::detect(*root, ws_root);
        } else {
            b->layout = bake::Layout::detect(*root);
        }

        // bake writes the effective, type-preserving CLI overrides before it
        // starts build_app. Keep this file as the process boundary between the
        // CLI and the independently compiled build.cpp executable.
        if (b->manifest) {
            bake::Path options_path = b->layout.bake_dir / "options.json";
            if (auto content = bake::read_file(options_path)) {
                try {
                    auto document = nlohmann::json::parse(*content);
                    if (document.value("schema", 0) != 1 ||
                        !document.contains("options") ||
                        !document["options"].is_object()) {
                        b->configuration_errors.push_back(
                            "invalid .bake/options.json schema");
                    } else {
                        for (auto& [name, value] : document["options"].items()) {
                            auto option = b->manifest->options.find(name);
                            if (option == b->manifest->options.end()) continue;

                            switch (option->second.type) {
                                case bake::BuildOption::Type::Bool:
                                    if (value.is_boolean()) {
                                        option->second = bake::BuildOption::from_bool(
                                            value.get<bool>());
                                    } else {
                                        b->configuration_errors.push_back(
                                            "option '" + name + "' has the wrong type");
                                    }
                                    break;
                                case bake::BuildOption::Type::Int:
                                    if (value.is_number_integer()) {
                                        option->second = bake::BuildOption::from_int(
                                            value.get<int64_t>());
                                    } else {
                                        b->configuration_errors.push_back(
                                            "option '" + name + "' has the wrong type");
                                    }
                                    break;
                                case bake::BuildOption::Type::String:
                                    if (value.is_string()) {
                                        option->second = bake::BuildOption::from_string(
                                            value.get<std::string>());
                                    } else {
                                        b->configuration_errors.push_back(
                                            "option '" + name + "' has the wrong type");
                                    }
                                    break;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    b->configuration_errors.push_back(
                        std::string("cannot parse .bake/options.json: ") + e.what());
                }
            }
        }

        // Load the lockfile used for this build context. Dependency lock state
        // is consumer-local; a versioned lock in the dependency source is only
        // copied/read by the CLI and is never rewritten by build_app.
        b->lockfile = bake::Lockfile::load(
            b->layout.dependency_layout
                ? b->layout.bake_dir / "bake.lock"
                : *root / "bake.lock");

        // Detect toolchain.
        b->toolchain = bake::Toolchain::detect();

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
        if (!b->configuration_errors.empty()) {
            std::string message = "bake_builder_build: invalid dependency configuration";
            for (const auto& error : b->configuration_errors) {
                message += "\n  - " + error;
            }
            set_error(message);
            std::println(std::cerr, "{}", message);
            return -1;
        }

        // Ensure output directories exist.
        b->layout.bake_dir.mkdir_recursive();
        b->layout.obj_dir.mkdir_recursive();
        b->layout.bin_dir.mkdir_recursive();
        b->layout.lib_dir.mkdir_recursive();

        const std::string package_name =
            b->manifest && b->manifest->package
                ? b->manifest->package->name
                : (b->layout.package_name.empty()
                       ? b->layout.root.filename_string()
                       : b->layout.package_name);
        const std::string package_version =
            b->manifest && b->manifest->package
                ? b->manifest->package->version
                : "";
        auto step_action_id = [&](std::string_view step_name) {
            return build_action_id("custom", package_name, step_name);
        };

        auto actions = nlohmann::json::array();

        // ---------------------------------------------------
        // Phase 1: emit custom actions for steps.
        // Targets may declare depends_on_step(name), so step
        // actions must appear before target actions.
        // ---------------------------------------------------
        for (const auto& step : b->steps) {
            nlohmann::json action;
            action["type"]       = "custom";
            action["id"]         = step_action_id(step->name);
            action["description"] = package_name + ": run " + step->name;
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
        // Uses the engine's ModuleGraph for C++20 module scanning,
        // DAG, and topological sort — same as convention builds.
        // ---------------------------------------------------
        // Modules compiled by earlier targets are importable by later
        // targets in the same package (mirrors cross-member visibility
        // in convention workspace builds).
        std::map<std::string, bake::Path> module_bmi;
        std::map<std::string, std::string> module_action;
        for (const auto& target : b->targets) {
            std::vector<std::string> compile_ids;
            std::vector<std::string> object_paths;
            std::set<std::string> emitted_compile_ids;
            bool has_cxx_sources = false;

            // -- Expand source entries (pattern + per-source flags) --
            std::vector<bake::Path> all_sources;
            std::map<std::string, std::vector<std::string>> source_flag_map;
            for (const auto& entry : target->source_entries) {
                auto files = expand_source_pattern(b->layout.root, entry.pattern);
                for (auto& src : files) {
                    all_sources.push_back(src);
                    for (auto& f : entry.flags)
                        source_flag_map[src.string()].push_back(f);
                }
            }

            // -- Build include_dirs and defines --
            // Include dirs are made absolute: the module scanner runs with a
            // per-source working directory, so root-relative paths would not
            // resolve there.
            std::vector<bake::Path> include_dirs;
            auto add_include_dir = [&](const std::string& inc) {
                include_dirs.push_back(
                    bake::Path(absolute_from_root(b->layout.root, inc)));
            };
            for (const auto& inc : target->include_dirs)
                add_include_dir(inc);
            for (const auto& inc : target->private_include_dirs)
                add_include_dir(inc);
            for (const auto& inc : target->dependency_include_dirs)
                add_include_dir(inc);

            std::vector<std::pair<std::string, std::string>> defines;
            for (const auto& [name, value] : target->defines)
                defines.push_back({name, value});
            for (const auto& [name, value] : target->private_defines)
                defines.push_back({name, value});
            for (const auto& [name, value] : target->dependency_defines)
                defines.push_back({name, value});

            // -- Separate sources and build module graph --
            bake::SourceSet src_set;
            for (auto& src : all_sources) {
                if (src.has_extension(".cppm")) {
                    src_set.module_interfaces.push_back(src);
                    has_cxx_sources = true;
                } else if (src.is_cpp()) {
                    src_set.cpp_files.push_back(src);
                    has_cxx_sources = true;
                } else {
                    src_set.c_files.push_back(src);
                }
            }

            bake::ModuleGraph mod_graph;
            if (!src_set.module_interfaces.empty() || !src_set.cpp_files.empty()) {
                b->layout.bmi_dir.mkdir_recursive();
                mod_graph = bake::ModuleGraph::build(
                    b->toolchain, src_set, target->std_ver, include_dirs);
            }

            // Helper to make a CompileConfig with common fields
            auto make_cc = [&](bake::Path src, std::string src_rel,
                               std::string obj_path) {
                bake::CompileConfig cc;
                cc.source  = bake::Path(src_rel);
                cc.output  = bake::Path(obj_path);
                cc.std_ver = target->std_ver;
                cc.use_pic = (target->type == TargetType::SharedLib);
                cc.include_dirs = include_dirs;
                cc.defines = defines;
                // Per-source flags from the source entry that matched this file
                auto it = source_flag_map.find(src.string());
                if (it != source_flag_map.end())
                    cc.extra_flags = it->second;
                return cc;
            };

            // -- Phase 2a: Compile module interfaces (topological order) --
            for (auto& mod_name : mod_graph.sorted) {
                auto& info = mod_graph.modules[mod_name];
                bake::Path src(info.source_path);
                std::string src_rel = rel_to_root(src.fs(), b->layout.root.fs());
                std::string comp_id = build_action_id(
                    "module", package_name, target->name, src_rel);
                std::string object_hash = bake::SHA256::hex(comp_id).substr(0, 12);
                std::string object_path =
                    (b->layout.obj_dir /
                     (target->name + "__" + src.stem_string() + "_" +
                      object_hash + ".o"))
                        .absolute().string();
                bake::Path bmi = b->layout.bmi_dir / (mod_name + ".pcm");
                module_bmi[mod_name] = bmi;
                module_action[mod_name] = comp_id;

                bake::CompileConfig cc = make_cc(src, src_rel, object_path);
                cc.is_module_interface = true;
                cc.bmi_output = bmi;
                nlohmann::json deps = nlohmann::json::array();
                for (auto& imp : info.imports) {
                    auto it = module_bmi.find(imp);
                    if (it != module_bmi.end())
                        cc.module_deps.push_back({imp, it->second});
                    auto ait = module_action.find(imp);
                    if (ait != module_action.end() && ait->second != comp_id)
                        deps.push_back(ait->second);
                }

                compile_ids.push_back(comp_id);
                object_paths.push_back(object_path);

                auto cmd = bake::make_compile_command(b->toolchain, cc);
                nlohmann::json action;
                action["type"]       = "compile_module";
                action["id"]         = comp_id;
                action["description"] =
                    action_owner(package_name, target->name) + ": " +
                    src.filename_string();
                action["inputs"]     = to_json_array({src_rel});
                action["outputs"]    = to_json_array({object_path, bmi.string()});
                action["command"]    = to_json_array(cmd);
                action["depends_on"] = std::move(deps);
                actions.push_back(std::move(action));
            }

            // -- Phase 2b: Compile regular sources --
            for (auto& src : src_set.cpp_files) {
                std::string src_rel = rel_to_root(src.fs(), b->layout.root.fs());
                const auto source_names = source_action_names(*b, src);
                std::string comp_id = build_action_id(
                    "compile", package_name, target->name, source_names.identity);
                if (!emitted_compile_ids.insert(comp_id).second) continue;
                std::string object_hash = bake::SHA256::hex(comp_id).substr(0, 12);
                std::string object_path =
                    (b->layout.obj_dir /
                     (target->name + "__" + src.stem_string() + "_" +
                      object_hash + ".o"))
                        .absolute().string();

                bake::CompileConfig cc = make_cc(src, src_rel, object_path);
                // Inject module deps. Clang's -fmodule-file doesn't resolve
                // transitive dependencies, so we must provide ALL available
                // module BMIs to every consumer, not just direct imports.
                nlohmann::json deps = nlohmann::json::array();
                for (auto& [name, bmi] : module_bmi) {
                    cc.module_deps.push_back({name, bmi});
                }
                // Record dependency on the actions that built those modules
                for (auto& imp : mod_graph.consumers[src.string()].imports) {
                    auto ait = module_action.find(imp);
                    if (ait != module_action.end())
                        deps.push_back(ait->second);
                }

                compile_ids.push_back(comp_id);
                object_paths.push_back(object_path);
                auto cmd = bake::make_compile_command(b->toolchain, cc);
                nlohmann::json action;
                action["type"]       = "compile";
                action["id"]         = comp_id;
                action["description"] =
                    action_owner(package_name, target->name) + ": " +
                    source_names.display;
                action["inputs"]     = to_json_array({src_rel});
                action["outputs"]    = to_json_array({object_path});
                action["command"]    = to_json_array(cmd);
                action["depends_on"] = std::move(deps);
                actions.push_back(std::move(action));
            }
            for (auto& src : src_set.c_files) {
                std::string src_rel = rel_to_root(src.fs(), b->layout.root.fs());
                const auto source_names = source_action_names(*b, src);
                std::string comp_id = build_action_id(
                    "compile", package_name, target->name, source_names.identity);
                if (!emitted_compile_ids.insert(comp_id).second) continue;
                std::string object_hash = bake::SHA256::hex(comp_id).substr(0, 12);
                std::string object_path =
                    (b->layout.obj_dir /
                     (target->name + "__" + src.stem_string() + "_" +
                      object_hash + ".o"))
                        .absolute().string();

                bake::CompileConfig cc = make_cc(src, src_rel, object_path);
                compile_ids.push_back(comp_id);
                object_paths.push_back(object_path);
                auto cmd = bake::make_compile_command(b->toolchain, cc);
                nlohmann::json action;
                action["type"]       = "compile";
                action["id"]         = comp_id;
                action["description"] =
                    action_owner(package_name, target->name) + ": " +
                    source_names.display;
                action["inputs"]     = to_json_array({src_rel});
                action["outputs"]    = to_json_array({object_path});
                action["command"]    = to_json_array(cmd);
                action["depends_on"] = nlohmann::json::array();
                actions.push_back(std::move(action));
            }

            // -- Link / archive action --
            if (!object_paths.empty()) {
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
                bake::Path output =
                    ((target->type == TargetType::Executable)
                         ? b->layout.bin_dir
                         : b->layout.lib_dir) /
                    out_name;
                std::string output_path = output.absolute().string();

                // Build link configuration.
                bake::LinkConfig lc;
                for (const auto& obj : object_paths)
                    lc.inputs.push_back(bake::Path(obj));
                // Add outputs of linked targets (e.g. bake links libbake)
                for (const auto* linked : target->linked_targets) {
                    if (!linked->output_path.empty())
                        lc.inputs.push_back(bake::Path(linked->output_path));
                }
                lc.output = bake::Path(output_path);
                lc.type   = pkg_type;
                lc.use_cxx_linker = has_cxx_sources || target->dependency_uses_cxx;

                if (target->type != TargetType::StaticLib) {
                    for (const auto& input : target->dependency_link_inputs)
                        lc.inputs.push_back(bake::Path(input));
                }

                for (const auto& lib : target->system_libs)
                    append_unique(lc.link_libs, lib);
                if (target->type != TargetType::StaticLib) {
                    for (const auto& lib : target->dependency_system_libs)
                        append_unique(lc.link_libs, lib);
                }
                for (const auto& framework : target->frameworks)
                    append_unique(lc.frameworks, framework);
                if (target->type != TargetType::StaticLib) {
                    for (const auto& framework : target->dependency_frameworks)
                        append_unique(lc.frameworks, framework);
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
                    depends.push_back(step_action_id(step->name));

                std::vector<std::string> link_inputs = object_paths;
                if (target->type != TargetType::StaticLib) {
                    link_inputs.insert(link_inputs.end(),
                                       target->dependency_link_inputs.begin(),
                                       target->dependency_link_inputs.end());
                }

                nlohmann::json action;
                action["type"]       = type_str;
                action["id"]         = build_action_id(
                    type_str, package_name, target->name);
                action["description"] =
                    action_owner(package_name, target->name) + ": " +
                    (type_str == "archive" ? "archive " : "link ") +
                    out_name;
                action["inputs"]     = to_json_array(link_inputs);
                action["outputs"]    = to_json_array({output_path});
                action["command"]    = to_json_array(cmd);
                action["depends_on"] = to_json_array(depends);

                actions.push_back(std::move(action));

                target->output_path = output_path;
                target->uses_cxx = lc.use_cxx_linker;
            }
        }

        // ---------------------------------------------------
        // Phase 3: serialize to .bake/build.json
        // ---------------------------------------------------
        nlohmann::json build_json;
        build_json["package"] = {
            {"name", package_name},
            {"version", package_version},
        };
        build_json["actions"] = std::move(actions);

        bake::Path out_path = b->layout.bake_dir / "build.json";
        std::string content = build_json.dump(2);

        if (!bake::write_file(out_path, content)) {
            set_error("bake_builder_build: failed to write " + out_path.string());
            return -1;
        }

        // Export one package-level link interface. A package normally exposes
        // the target named by [package].name; a single library target is an
        // unambiguous fallback for meta packages whose package/display name
        // differs from the upstream library filename.
        bake_target* exported = nullptr;
        if (b->manifest && b->manifest->package) {
            const auto& package_name = b->manifest->package->name;
            for (const auto& target : b->targets) {
                if (target->type != TargetType::Executable &&
                    target->name == package_name && !target->output_path.empty()) {
                    exported = target.get();
                    break;
                }
            }
        }
        if (!exported) {
            for (const auto& target : b->targets) {
                if (target->type == TargetType::Executable || target->output_path.empty())
                    continue;
                if (exported) {
                    exported = nullptr; // ambiguous: do not guess
                    break;
                }
                exported = target.get();
            }
        }

        nlohmann::json package_json;
        package_json["schema"] = 1;
        package_json["include_dirs"] = nlohmann::json::array();
        package_json["defines"] = nlohmann::json::array();
        package_json["link_inputs"] = nlohmann::json::array();
        package_json["system_libs"] = nlohmann::json::array();
        package_json["frameworks"] = nlohmann::json::array();
        package_json["uses_cxx"] = false;

        if (exported) {
            for (const auto& include_dir : exported->include_dirs) {
                package_json["include_dirs"].push_back(
                    absolute_from_root(b->layout.root, include_dir));
            }
            for (const auto& include_dir : exported->dependency_include_dirs)
                package_json["include_dirs"].push_back(include_dir);

            auto public_defines = exported->defines;
            public_defines.insert(public_defines.end(),
                                  exported->dependency_defines.begin(),
                                  exported->dependency_defines.end());
            package_json["defines"] = defines_to_json(public_defines);
            package_json["link_inputs"].push_back(exported->output_path);
            if (exported->type == TargetType::StaticLib) {
                for (const auto& input : exported->dependency_link_inputs)
                    package_json["link_inputs"].push_back(input);
                for (const auto& lib : exported->dependency_system_libs) {
                    if (std::find(package_json["system_libs"].begin(),
                                  package_json["system_libs"].end(), lib) ==
                        package_json["system_libs"].end())
                        package_json["system_libs"].push_back(lib);
                }
                for (const auto& framework : exported->dependency_frameworks) {
                    if (std::find(package_json["frameworks"].begin(),
                                  package_json["frameworks"].end(), framework) ==
                        package_json["frameworks"].end())
                        package_json["frameworks"].push_back(framework);
                }
            }
            for (const auto& lib : exported->system_libs) {
                if (std::find(package_json["system_libs"].begin(),
                              package_json["system_libs"].end(), lib) ==
                    package_json["system_libs"].end())
                    package_json["system_libs"].push_back(lib);
            }
            for (const auto& framework : exported->frameworks) {
                if (std::find(package_json["frameworks"].begin(),
                              package_json["frameworks"].end(), framework) ==
                    package_json["frameworks"].end())
                    package_json["frameworks"].push_back(framework);
            }
            package_json["uses_cxx"] = exported->uses_cxx;
        }

        bake::Path package_path = b->layout.package_file;
        if (!bake::write_file(package_path, package_json.dump(2))) {
            set_error("bake_builder_build: failed to write " + package_path.string());
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
        t->source_entries.push_back({pattern, {}});
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_sources: ") + e.what());
    }
    return t;
}

BAKE_API bake_target* bake_target_sources_with_flags(
    bake_target* t, const char* pattern,
    const char* const* flags, int num_flags) noexcept {
    if (!t || !pattern) return t;
    try {
        source_entry entry;
        entry.pattern = pattern;
        for (int i = 0; i < num_flags; ++i)
            if (flags[i]) entry.flags.push_back(flags[i]);
        t->source_entries.push_back(std::move(entry));
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_sources_with_flags: ") + e.what());
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

BAKE_API bake_target* bake_target_private_include_dirs(
    bake_target* t, const char* dirs) noexcept {
    if (!t || !dirs) return t;
    try {
        t->private_include_dirs.push_back(dirs);
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_private_include_dirs: ") + e.what());
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

BAKE_API bake_target* bake_target_private_define(
    bake_target* t, const char* name, const char* value) noexcept {
    if (!t || !name) return t;
    try {
        t->private_defines.push_back({name, value ? value : ""});
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_private_define: ") + e.what());
    }
    return t;
}

BAKE_API bake_target* bake_target_dependency_sources(
    bake_target* t, bake_dependency* dependency, const char* pattern) noexcept {
    if (!t || !dependency || !pattern) return t;
    try {
        if (auto path = dependency_relative_path(dependency, pattern, true))
            t->source_entries.push_back({std::move(*path), {}});
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_dependency_sources: ") + e.what());
    }
    return t;
}

BAKE_API bake_target* bake_target_dependency_include_dirs(
    bake_target* t, bake_dependency* dependency, const char* dirs) noexcept {
    if (!t || !dependency || !dirs) return t;
    try {
        if (auto path = dependency_relative_path(dependency, dirs, false))
            t->include_dirs.push_back(std::move(*path));
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_dependency_include_dirs: ") + e.what());
    }
    return t;
}

BAKE_API bake_target* bake_target_dependency_private_include_dirs(
    bake_target* t, bake_dependency* dependency, const char* dirs) noexcept {
    if (!t || !dependency || !dirs) return t;
    try {
        if (auto path = dependency_relative_path(dependency, dirs, false))
            t->private_include_dirs.push_back(std::move(*path));
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_dependency_private_include_dirs: ") + e.what());
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

BAKE_API bake_target* bake_target_link_framework(
    bake_target* t, const char* framework) noexcept {
    if (!t || !framework) return t;
    try {
        t->frameworks.push_back(framework);
    } catch (const std::exception& e) {
        set_error(std::string("bake_target_link_framework: ") + e.what());
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
// Dependency
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

// Import a Bake-native dependency's package-level usage requirements.
BAKE_API void bake_dep_link_to(bake_dependency* d, bake_target* t) noexcept {
    if (!d || !t) return;
    try {
        const char* src = bake_dep_src_dir(d);
        if (!src) {
            d->builder->configuration_errors.push_back(
                "dependency '" + d->name + "' is not declared or resolved");
            return;
        }

        auto dependency_manifest = bake::Manifest::load(bake::Path(src));
        if (!dependency_manifest || !dependency_manifest->package ||
            dependency_manifest->package->name.empty()) {
            d->builder->configuration_errors.push_back(
                "dependency '" + d->name +
                "' is not a named Bake package and cannot be linked");
            return;
        }
        bake::Path metadata_path = bake::Layout::for_dependency(
            bake::Path(src), d->builder->layout.out_dir,
            dependency_manifest->package->name).package_file;
        auto content = bake::read_file(metadata_path);
        if (!content) {
            d->builder->configuration_errors.push_back(
                "dependency '" + d->name + "' has no built package metadata at " +
                metadata_path.string());
            return;
        }

        auto metadata = nlohmann::json::parse(*content);
        if (metadata.value("schema", 0) != 1) {
            d->builder->configuration_errors.push_back(
                "dependency '" + d->name + "' has unsupported package metadata");
            return;
        }

        for (const auto& value : metadata.value("include_dirs", nlohmann::json::array()))
            append_unique(t->dependency_include_dirs, value.get<std::string>());
        for (const auto& define : metadata.value("defines", nlohmann::json::array())) {
            t->dependency_defines.push_back({
                define.value("name", std::string{}),
                define.value("value", std::string{})});
        }
        for (const auto& value : metadata.value("link_inputs", nlohmann::json::array()))
            append_unique(t->dependency_link_inputs, value.get<std::string>());
        for (const auto& value : metadata.value("system_libs", nlohmann::json::array()))
            append_unique(t->dependency_system_libs, value.get<std::string>());
        for (const auto& value : metadata.value("frameworks", nlohmann::json::array()))
            append_unique(t->dependency_frameworks, value.get<std::string>());
        t->dependency_uses_cxx =
            t->dependency_uses_cxx || metadata.value("uses_cxx", false);
    } catch (const std::exception& e) {
        set_error(std::string("bake_dep_link_to: ") + e.what());
        if (d && d->builder) {
            d->builder->configuration_errors.push_back(
                "failed to read dependency '" + d->name + "': " + e.what());
        }
    }
}
