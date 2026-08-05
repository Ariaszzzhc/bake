module;

#include <nlohmann/json.hpp>

export module bake.engine;

import std;
import bake.util;
import bake.project;
import bake.compiler;

// ============================================================
// bake.engine — source discovery, module scanning, DAG, executor
// ============================================================

namespace bake {

// ===== Source discovery =====

export struct SourceSet {
    std::vector<Path> cpp_files;            // src/*.cpp, src/**/*.cpp
    std::vector<Path> c_files;              // src/*.c
    std::vector<Path> module_interfaces;    // src/*.cppm, public/*.cppm
    std::vector<Path> public_headers;       // public/**/*.hpp, *.h

    bool empty() const {
        return cpp_files.empty() && c_files.empty() &&
               module_interfaces.empty() && public_headers.empty();
    }
};

export SourceSet discover_sources(const Path& src_dir, const Path& public_dir) {
    SourceSet sources;

    // Source directory: *.cpp, *.c, *.cppm (recursive)
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

    // Public directory: *.cppm (module interfaces), *.hpp/*.h (headers)
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

    // Sort for deterministic ordering
    std::sort(sources.cpp_files.begin(), sources.cpp_files.end());
    std::sort(sources.c_files.begin(), sources.c_files.end());
    std::sort(sources.module_interfaces.begin(), sources.module_interfaces.end());
    std::sort(sources.public_headers.begin(), sources.public_headers.end());

    return sources;
}

// ===== Module scanning (P1689) =====

export struct ModuleInfo {
    std::string source_path;
    std::string module_name;            // provided module name (empty if consumer-only)
    bool is_interface = false;
    std::vector<std::string> imports;   // required modules
};

export std::optional<ModuleInfo> scan_module_file(
        const Toolchain& tc, const Path& source,
        const std::string& std_ver,
        const std::vector<Path>& include_dirs = {}) {

    if (!tc.has_scanner()) return std::nullopt;

    // Build the clang-scan-deps command
    std::vector<std::string> cmd;
    cmd.push_back(tc.scanner_path);
    cmd.push_back("-format=p1689");
    cmd.push_back("--");
    cmd.push_back(tc.cxx_path);
    cmd.push_back("-std=" + std_ver);
    for (auto& inc : include_dirs) {
        cmd.push_back("-I" + inc.string());
    }
    cmd.push_back("-c");
    cmd.push_back(source.string());

    auto result = run_process(cmd, source.parent(), true);
    if (!result.success() || result.stdout_output.empty()) {
        return std::nullopt;
    }

    ModuleInfo info;
    info.source_path = source.string();

    try {
        auto json = nlohmann::json::parse(result.stdout_output);
        if (json.contains("rules") && !json["rules"].empty()) {
            auto& rule = json["rules"][0];
            if (rule.contains("provides")) {
                for (auto& prov : rule["provides"]) {
                    info.module_name = prov.value("logical-name", "");
                    info.is_interface = prov.value("is-interface", true);
                }
            }
            if (rule.contains("requires")) {
                for (auto& req : rule["requires"]) {
                    std::string name = req.value("logical-name", "");
                    if (!name.empty()) {
                        info.imports.push_back(name);
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        return std::nullopt;
    }

    return info;
}

// ===== Module dependency graph =====

export struct ModuleGraph {
    // module_name → ModuleInfo
    std::map<std::string, ModuleInfo> modules;
    // source_path → ModuleInfo (for consumer files that import modules)
    std::map<std::string, ModuleInfo> consumers;
    // topologically sorted module names (interfaces first)
    std::vector<std::string> sorted;

    bool empty() const { return modules.empty(); }

    // Build graph from scanning all module interface and cpp files
    static ModuleGraph build(const Toolchain& tc, const SourceSet& sources,
                              const std::string& std_ver,
                              const std::vector<Path>& include_dirs = {}) {
        ModuleGraph graph;

        // Scan module interface files
        for (auto& src : sources.module_interfaces) {
            auto info = scan_module_file(tc, src, std_ver, include_dirs);
            if (info && !info->module_name.empty()) {
                graph.modules[info->module_name] = *info;
            }
        }

        // Scan cpp files for module imports
        for (auto& src : sources.cpp_files) {
            auto info = scan_module_file(tc, src, std_ver, include_dirs);
            if (info && !info->imports.empty()) {
                graph.consumers[src.string()] = *info;
            }
        }

        // Topological sort of module interfaces
        graph.topological_sort();

        return graph;
    }

private:
    void topological_sort() {
        // Kahn's algorithm
        std::map<std::string, int> in_degree;
        std::map<std::string, std::vector<std::string>> adj;  // module → modules it depends on

        for (auto& [name, info] : modules) {
            in_degree[name] = 0;
        }

        for (auto& [name, info] : modules) {
            for (auto& imp : info.imports) {
                if (modules.count(imp)) {
                    adj[imp].push_back(name);
                    in_degree[name]++;
                }
            }
        }

        std::vector<std::string> queue;
        for (auto& [name, deg] : in_degree) {
            if (deg == 0) queue.push_back(name);
        }
        std::sort(queue.begin(), queue.end());

        while (!queue.empty()) {
            std::string node = queue.front();
            queue.erase(queue.begin());
            sorted.push_back(node);

            for (auto& dependent : adj[node]) {
                if (--in_degree[dependent] == 0) {
                    queue.push_back(dependent);
                }
            }
            std::sort(queue.begin(), queue.end());
        }

        if (sorted.size() != modules.size()) {
            // Cycle detected — just append remaining modules
            for (auto& [name, info] : modules) {
                if (std::find(sorted.begin(), sorted.end(), name) == sorted.end()) {
                    sorted.push_back(name);
                }
            }
        }
    }
};

// ===== Build action =====

export struct BuildAction {
    enum class Type { CompileModule, Compile, Link, Archive };
    Type type;
    // Machine identity: deterministic and unique within a package build.
    // This is used by dependencies and incremental state, never as UI text.
    std::string id;
    // User-facing progress text. Cosmetic changes must not invalidate cache.
    std::string description;
    std::vector<Path> inputs;
    std::vector<Path> outputs;
    std::vector<std::string> command;
    // Index into the plan's action list
    std::vector<size_t> depends_on;

    bool is_compile() const { return type == Type::Compile || type == Type::CompileModule; }
};

// ===== Build plan =====

export struct BuildPlan {
    std::vector<BuildAction> actions;
    Path primary_output;      // final executable or library
    Path project_root;
    Path state_dir;
    Path obj_dir;
    Path bmi_dir;
    std::string package_name;
    std::string package_version;

    // Apply build options to std version etc.
    // Returns nullopt if configuration is valid.
};

// Dependent source entry — populated by the CLI layer from the lockfile/cache.
export struct DepSourceEntry {
    std::string dep_name;
    Path source;
};

// Public compile/link requirements exported by a built Bake-native package.
// The CLI reads these from out/.pkgs/<package>/package.json; the convention
// planner consumes them exactly as build.cpp's dependency(...).link_to() does.
export struct PackageUsageRequirements {
    std::string dependency_name;
    std::vector<Path> include_dirs;
    std::vector<std::pair<std::string, std::string>> defines;
    std::vector<Path> link_inputs;
    std::vector<std::string> system_libs;
    std::vector<std::string> frameworks;
    bool uses_cxx = false;
};

namespace {

std::string normalized_action_path(const Path& root, const Path& source) {
    std::error_code ec;
    auto relative = std::filesystem::relative(source.fs(), root.fs(), ec);
    if (!ec && !relative.empty())
        return relative.lexically_normal().generic_string();
    return source.fs().lexically_normal().generic_string();
}

std::string compile_action_id(std::string_view package,
                              std::string_view target,
                              std::string_view kind,
                              std::string_view source_identity) {
    return std::string(kind) + ":" + std::string(package) + ":" +
           std::string(target) + ":" + std::string(source_identity);
}

std::string object_name_for(const Path& source, std::string_view action_id) {
    return source.stem_string() + "_" +
           SHA256::hex(action_id).substr(0, 12) + ".o";
}

} // namespace

// Create a build plan using convention rules
export BuildPlan create_convention_plan(
        const Manifest& manifest,
        const Layout& layout,
        const Toolchain& tc,
        const std::map<std::string, BuildOption>& options,
        const std::vector<DepSourceEntry>& dep_sources = {},
        const std::vector<Path>& dep_include_dirs = {},
        bool compile_path_deps = true,
        const Path& std_module_pcm = {},
        const std::vector<PackageUsageRequirements>& package_requirements = {}) {

    BuildPlan plan;
    plan.project_root = layout.root;
    plan.state_dir = layout.bake_dir;
    plan.obj_dir = layout.obj_dir;
    plan.bmi_dir = layout.bmi_dir;

    // Ensure build dirs exist
    plan.obj_dir.mkdir_recursive();
    plan.bmi_dir.mkdir_recursive();

    if (!manifest.has_package()) return plan;

    const auto& pkg = *manifest.package;
    plan.package_name = pkg.name;
    plan.package_version = pkg.version;
    std::string std_ver = pkg.std_version;

    // Discover sources
    auto sources = discover_sources(layout.source_dir, layout.public_dir);

    // Build include dirs
    std::vector<Path> include_dirs;
    if (layout.public_dir.is_directory()) {
        include_dirs.push_back(layout.public_dir);
    }

    std::set<std::string> packaged_path_dependencies;
    std::vector<std::pair<std::string, std::string>> package_defines;
    std::vector<Path> package_link_inputs;
    std::vector<std::string> package_system_libs;
    std::vector<std::string> package_frameworks;
    bool package_uses_cxx = false;

    auto append_unique = []<typename T>(std::vector<T>& values, const T& value) {
        if (std::find(values.begin(), values.end(), value) == values.end())
            values.push_back(value);
    };

    for (const auto& requirements : package_requirements) {
        packaged_path_dependencies.insert(requirements.dependency_name);
        for (const auto& include_dir : requirements.include_dirs)
            append_unique(include_dirs, include_dir);
        for (const auto& define : requirements.defines)
            append_unique(package_defines, define);
        for (const auto& input : requirements.link_inputs)
            append_unique(package_link_inputs, input);
        for (const auto& library : requirements.system_libs)
            append_unique(package_system_libs, library);
        for (const auto& framework : requirements.frameworks)
            append_unique(package_frameworks, framework);
        package_uses_cxx = package_uses_cxx || requirements.uses_cxx;
    }

    // Resolve path dependencies — add their public/ as include dirs AND
    // collect their source files for compilation + linking.
    // When compile_path_deps is false (workspace builds), only include dirs
    // are added — the workspace build handles inter-member linking separately.
    std::vector<DepSourceEntry> path_dep_sources;
    for (auto& [name, dep] : manifest.dependencies) {
        if (dep.is_path_dep) {
            // A built Bake-native package is consumed through its exported
            // usage requirements. Its sources remain owned by that package.
            if (packaged_path_dependencies.contains(name)) continue;

            Path dep_dir = manifest.project_dir / dep.path;
            Path dep_public = dep_dir / "public";
            if (dep_public.is_directory()) {
                include_dirs.push_back(dep_public);
            }
            if (compile_path_deps) {
                // Discover path dep's source files for compilation + linking
                Path dep_src = dep_dir / "src";
                if (dep_src.is_directory()) {
                    auto pd = discover_sources(dep_src, dep_public);
                    for (auto& cpp : pd.cpp_files) {
                        path_dep_sources.push_back({"pathdep_" + name, cpp});
                    }
                    for (auto& c : pd.c_files) {
                        path_dep_sources.push_back({"pathdep_" + name, c});
                    }
                }
            }
        }
    }

    // Add include dirs from locked bake-native dependencies (populated by CLI)
    for (auto& inc : dep_include_dirs) {
        include_dirs.push_back(inc);
    }

    // Helper: inject std module PCM into a CompileConfig if available.
    // Ensures import std; works in ALL C++ sources, not just those
    // discovered through module graph scanning.
    auto ensure_std_dep = [&](CompileConfig& cc) {
        if (cc.source.is_c()) return;
        if (std_module_pcm.string().empty() || !std_module_pcm.is_regular_file())
            return;
        for (auto& [name, _] : cc.module_deps) {
            if (name == "std") return;  // already present
        }
        cc.module_deps.push_back({"std", std_module_pcm});
    };

    // Build module graph (scan even with no interfaces — .cpp files may
    // import std or other modules that need dependency resolution)
    ModuleGraph mod_graph;
    if (!sources.module_interfaces.empty() || !sources.cpp_files.empty()) {
        mod_graph = ModuleGraph::build(tc, sources, std_ver, include_dirs);
    }

    // Map module name → BMI path
    std::map<std::string, Path> module_bmi;

    // Register the pre-built libc++ std module so import std; resolves.
    if (!std_module_pcm.string().empty() && std_module_pcm.is_regular_file()) {
        module_bmi["std"] = std_module_pcm;
    }

    // Phase 1: Compile module interfaces (in dependency order)
    for (auto& mod_name : mod_graph.sorted) {
        auto& info = mod_graph.modules[mod_name];
        Path src(info.source_path);
        const std::string source_identity =
            normalized_action_path(manifest.project_dir, src);
        const std::string action_id = compile_action_id(
            pkg.name, mod_name, "module", source_identity);
        Path bmi = plan.bmi_dir / (mod_name + ".pcm");
        Path obj = plan.obj_dir / object_name_for(src, action_id);

        CompileConfig cc;
        cc.source = src;
        cc.output = obj;
        cc.std_ver = std_ver;
        cc.include_dirs = include_dirs;
        cc.defines = package_defines;
        cc.is_module_interface = true;
        cc.bmi_output = bmi;
        cc.use_pic = (pkg.type == PackageType::SharedLib);

        // Module dependencies (imports)
        for (auto& imp : info.imports) {
            auto it = module_bmi.find(imp);
            if (it != module_bmi.end()) {
                cc.module_deps.push_back({imp, it->second});
            }
        }
        ensure_std_dep(cc);

        BuildAction action;
        action.type = BuildAction::Type::CompileModule;
        action.id = action_id;
        action.description = pkg.name + ": module " + mod_name;
        action.inputs = {src};
        // Add imported BMI files as inputs so that when a dependency module
        // changes, this module is rebuilt. Without this, the old BMI gets
        // loaded alongside a new dependency BMI, crashing clang 22.
        for (auto& [name, bmi] : cc.module_deps) {
            action.inputs.push_back(bmi);
        }
        action.outputs = {obj, bmi};
        action.command = make_compile_command(tc, cc);

        plan.actions.push_back(std::move(action));
        module_bmi[mod_name] = bmi;
    }

    // Phase 2: Compile regular sources
    // Collect all .cpp and .c files
    std::vector<Path> all_sources = sources.cpp_files;
    for (auto& f : sources.c_files) all_sources.push_back(f);

    for (auto& src : all_sources) {
        const std::string source_identity =
            normalized_action_path(manifest.project_dir, src);
        const std::string action_id = compile_action_id(
            pkg.name, pkg.name, "compile", source_identity);
        Path obj = plan.obj_dir / object_name_for(src, action_id);

        CompileConfig cc;
        cc.source = src;
        cc.output = obj;
        cc.std_ver = std_ver;
        cc.include_dirs = include_dirs;
        cc.defines = package_defines;
        cc.use_pic = (pkg.type == PackageType::SharedLib);

        // Collect transitive module dependencies for this consumer
        auto consumer_it = mod_graph.consumers.find(src.string());
        if (consumer_it != mod_graph.consumers.end()) {
            std::set<std::string> visited;
            std::vector<std::string> queue(consumer_it->second.imports.begin(),
                                            consumer_it->second.imports.end());
            while (!queue.empty()) {
                std::string mod = queue.back();
                queue.pop_back();
                if (visited.count(mod)) continue;
                visited.insert(mod);

                auto bmi_it = module_bmi.find(mod);
                if (bmi_it != module_bmi.end()) {
                    cc.module_deps.push_back({mod, bmi_it->second});
                }
                // Add transitive deps from this module
                auto mod_it = mod_graph.modules.find(mod);
                if (mod_it != mod_graph.modules.end()) {
                    for (auto& imp : mod_it->second.imports) {
                        if (!visited.count(imp)) queue.push_back(imp);
                    }
                }
            }
        }

        // Ensure import std; resolves even without a module graph
        ensure_std_dep(cc);

        BuildAction action;
        action.type = BuildAction::Type::Compile;
        action.id = action_id;
        action.description = pkg.name + ": " +
                             normalized_action_path(layout.source_dir, src);
        action.inputs = {src};
        // Add BMI files as inputs so module changes trigger consumer recompile
        for (auto& [name, bmi] : cc.module_deps) {
            action.inputs.push_back(bmi);
        }
        action.outputs = {obj};
        action.command = make_compile_command(tc, cc);

        // Depend on all module compilations
        for (size_t i = 0; i < plan.actions.size(); ++i) {
            if (plan.actions[i].type == BuildAction::Type::CompileModule) {
                action.depends_on.push_back(i);
            }
        }

        plan.actions.push_back(std::move(action));
    }

    // Phase 2b: Compile sources from bake-native locked dependencies + path deps
    auto all_dep_sources = dep_sources;
    for (auto& pds : path_dep_sources) {
        all_dep_sources.push_back(pds);
    }
    for (auto& ds : all_dep_sources) {
        const std::string source_identity =
            normalized_action_path(manifest.project_dir, ds.source);
        const std::string action_id = compile_action_id(
            pkg.name, ds.dep_name, "dependency-compile", source_identity);
        Path obj = plan.obj_dir / object_name_for(ds.source, action_id);

        CompileConfig cc;
        cc.source = ds.source;
        cc.output = obj;
        cc.std_ver = std_ver;
        cc.include_dirs = include_dirs;
        cc.defines = package_defines;
        cc.use_pic = (pkg.type == PackageType::SharedLib);
        ensure_std_dep(cc);

        BuildAction action;
        action.type = BuildAction::Type::Compile;
        action.id = action_id;
        action.description = pkg.name + "/" + ds.dep_name + ": " +
                             ds.source.filename_string();
        action.inputs = {ds.source};
        action.outputs = {obj};
        action.command = make_compile_command(tc, cc);

        // Depend on all module compilations (dep sources may import modules)
        for (size_t i = 0; i < plan.actions.size(); ++i) {
            if (plan.actions[i].type == BuildAction::Type::CompileModule) {
                action.depends_on.push_back(i);
            }
        }

        plan.actions.push_back(std::move(action));
    }

    // Phase 3: Link/Archive
    if (!all_sources.empty() || !mod_graph.sorted.empty() ||
        !all_dep_sources.empty()) {
        std::vector<Path> obj_files;
        for (auto& a : plan.actions) {
            if (a.is_compile() && !a.outputs.empty()) {
                obj_files.push_back(a.outputs[0]);
            }
        }

        std::string out_name = library_name(pkg.name, pkg.type);
        Path output = layout.output_for(pkg.type) / out_name;
        output.parent().mkdir_recursive();

        BuildAction link_action;
        link_action.inputs = obj_files;
        link_action.outputs = {output};

        if (pkg.type == PackageType::StaticLib) {
            link_action.type = BuildAction::Type::Archive;
            link_action.id = "archive:" + pkg.name + ":" + pkg.name;
            link_action.description = pkg.name + ": archive " + out_name;

            LinkConfig lc;
            lc.inputs = obj_files;
            lc.output = output;
            lc.type = pkg.type;
            link_action.command = make_archive_command(tc, lc);
        } else {
            link_action.type = BuildAction::Type::Link;
            link_action.id = "link:" + pkg.name + ":" + pkg.name;
            link_action.description = pkg.name + ": link " + out_name;

            LinkConfig lc;
            lc.inputs = obj_files;
            for (const auto& input : package_link_inputs)
                lc.inputs.push_back(input);
            lc.output = output;
            lc.type = pkg.type;
            lc.use_cxx_linker = !sources.cpp_files.empty() ||
                                !sources.module_interfaces.empty() ||
                                package_uses_cxx;
            for (const auto& ds : all_dep_sources) {
                if (!ds.source.is_c()) {
                    lc.use_cxx_linker = true;
                    break;
                }
            }
            lc.link_libs = package_system_libs;
            lc.frameworks = package_frameworks;
            link_action.command = make_link_command(tc, lc);

            for (const auto& input : package_link_inputs)
                link_action.inputs.push_back(input);

            // Ensure libc++ at link time when import std is in use
            if (lc.use_cxx_linker && !std_module_pcm.string().empty() && tc.is_clang()) {
                link_action.command.insert(link_action.command.begin() + 1,
                                           "-stdlib=libc++");
            }
        }

        // Depends on all compile actions
        for (size_t i = 0; i < plan.actions.size(); ++i) {
            if (plan.actions[i].is_compile()) {
                link_action.depends_on.push_back(i);
            }
        }

        plan.actions.push_back(std::move(link_action));
        plan.primary_output = output;
    }

    return plan;
}

// ===== Incremental build =====

export bool needs_rebuild(const BuildAction& action) {
    // If any output doesn't exist → rebuild
    for (auto& out : action.outputs) {
        if (!out.exists()) return true;
    }

    // If any input is newer than any output → rebuild
    for (auto& in : action.inputs) {
        if (!in.exists()) continue;
        auto in_time = std::filesystem::last_write_time(in.fs());
        for (auto& out : action.outputs) {
            if (!out.exists()) return true;
            auto out_time = std::filesystem::last_write_time(out.fs());
            if (in_time > out_time) return true;
        }
    }

    return false;
}

namespace {

std::string action_fingerprint(const BuildAction& action) {
    nlohmann::json document;
    document["type"] = static_cast<int>(action.type);
    document["id"] = action.id;
    document["command"] = action.command;
    document["inputs"] = nlohmann::json::array();
    document["outputs"] = nlohmann::json::array();
    document["depends_on"] = action.depends_on;
    for (const auto& input : action.inputs)
        document["inputs"].push_back(input.string());
    for (const auto& output : action.outputs)
        document["outputs"].push_back(output.string());
    return SHA256::hex(document.dump());
}

std::map<std::string, std::string> load_action_fingerprints(const Path& path) {
    std::map<std::string, std::string> fingerprints;
    auto content = read_file(path);
    if (!content) return fingerprints;

    try {
        auto document = nlohmann::json::parse(*content);
        if (document.value("schema", 0) != 1 ||
            !document.contains("actions") ||
            !document["actions"].is_object()) {
            return fingerprints;
        }
        for (auto& [id, value] : document["actions"].items()) {
            if (value.is_string()) fingerprints[id] = value.get<std::string>();
        }
    } catch (...) {
        // A missing/corrupt state file is only a cache miss: rebuild safely.
    }
    return fingerprints;
}

} // namespace

// ===== Executor =====

export int execute_plan(BuildPlan& plan, int jobs) {
    const std::string package_label = [&] {
        if (plan.package_name.empty()) return std::string("build");
        if (plan.package_version.empty()) return plan.package_name;
        return plan.package_name + " v" + plan.package_version;
    }();

    if (plan.actions.empty()) {
        std::println("    Finished {} (nothing to build)", package_label);
        return 0;
    }

    if (jobs <= 0) {
        jobs = static_cast<int>(std::thread::hardware_concurrency());
        if (jobs == 0) jobs = 4;
    }

    const Path state_path = plan.state_dir / "action-state.json";
    const auto previous_fingerprints = load_action_fingerprints(state_path);
    std::vector<std::string> fingerprints;
    fingerprints.reserve(plan.actions.size());
    for (const auto& action : plan.actions)
        fingerprints.push_back(action_fingerprint(action));

    auto action_needs_rebuild = [&](size_t index) {
        const auto& action = plan.actions[index];
        auto previous = previous_fingerprints.find(action.id);
        return previous == previous_fingerprints.end() ||
               previous->second != fingerprints[index] ||
               needs_rebuild(action);
    };

    // Decide the complete dirty set before execution. Propagating dirtiness
    // through dependencies is important: a link action can look current until
    // one of its object inputs is rebuilt later in this invocation.
    std::vector<bool> rebuild(plan.actions.size(), false);
    for (size_t i = 0; i < plan.actions.size(); ++i)
        rebuild[i] = action_needs_rebuild(i);
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < plan.actions.size(); ++i) {
            if (rebuild[i]) continue;
            for (size_t dependency : plan.actions[i].depends_on) {
                if (dependency < rebuild.size() && rebuild[dependency]) {
                    rebuild[i] = true;
                    changed = true;
                    break;
                }
            }
        }
    }

    const int pending_total = static_cast<int>(
        std::count(rebuild.begin(), rebuild.end(), true));
    const int skipped = static_cast<int>(plan.actions.size()) - pending_total;
    std::atomic<int> completed_actions{0};

    // Track completion
    std::vector<int> status(plan.actions.size(), 0);  // 0=pending, 1=done, -1=failed
    std::mutex output_mutex;
    int actions_started = 0;
    auto announce = [&](const BuildAction& action) {
        std::lock_guard<std::mutex> lock(output_mutex);
        ++actions_started;
        std::println("    [{}/{}] {}", actions_started, pending_total,
                     action.description);
    };

    // Execute module interfaces and compiles
    // For simplicity: module interfaces sequential, regular compiles parallel

    // First pass: compile module interfaces (sequential, in order)
    for (size_t i = 0; i < plan.actions.size(); ++i) {
        if (plan.actions[i].type == BuildAction::Type::CompileModule) {
            auto& action = plan.actions[i];
            if (!rebuild[i]) {
                status[i] = 1;
                continue;
            }

            announce(action);

            auto result = run_process(action.command, plan.project_root);
            if (!result.success()) {
                std::lock_guard<std::mutex> lock(output_mutex);
                if (!result.stderr_output.empty()) {
                    std::print(std::cerr, "{}", result.stderr_output);
                }
                std::println(std::cerr, "bake: failed to compile module");
                status[i] = -1;
                return 1;
            }
            status[i] = 1;
            completed_actions++;
        }
    }

    // Second pass: compile regular sources (parallel with threads)
    std::vector<size_t> compile_indices;
    for (size_t i = 0; i < plan.actions.size(); ++i) {
        if (plan.actions[i].type == BuildAction::Type::Compile) {
            compile_indices.push_back(i);
        }
    }

    if (!compile_indices.empty()) {
        std::vector<std::thread> threads;
        std::atomic<int> compile_failed{0};

        int actual_jobs = std::min(jobs, static_cast<int>(compile_indices.size()));

        auto worker = [&](size_t start, size_t stride) {
            for (size_t k = start; k < compile_indices.size(); k += stride) {
                if (compile_failed.load() > 0) return;

                size_t i = compile_indices[k];
                auto& action = plan.actions[i];

                if (!rebuild[i]) {
                    status[i] = 1;
                    continue;
                }

                announce(action);

                auto result = run_process(action.command, plan.project_root);
                if (!result.success()) {
                    {
                        std::lock_guard<std::mutex> lock(output_mutex);
                        if (!result.stderr_output.empty()) {
                            std::print(std::cerr, "{}", result.stderr_output);
                        }
                    }
                    compile_failed++;
                    status[i] = -1;
                    return;
                }
                status[i] = 1;
                completed_actions++;
            }
        };

        for (int t = 0; t < actual_jobs; ++t) {
            threads.emplace_back(worker, t, actual_jobs);
        }
        for (auto& th : threads) th.join();

        if (compile_failed.load() > 0) {
            std::println(std::cerr, "bake: compilation failed");
            return 1;
        }
    }

    // Third pass: link/archive
    for (size_t i = 0; i < plan.actions.size(); ++i) {
        if (plan.actions[i].type == BuildAction::Type::Link ||
            plan.actions[i].type == BuildAction::Type::Archive) {
            auto& action = plan.actions[i];

            if (!rebuild[i]) {
                status[i] = 1;
                continue;
            }

            announce(action);

            auto result = run_process(action.command, plan.project_root);
            if (!result.success()) {
                std::lock_guard<std::mutex> lock(output_mutex);
                if (!result.stderr_output.empty()) {
                    std::print(std::cerr, "{}", result.stderr_output);
                }
                std::println(std::cerr, "bake: {} failed",
                             action.type == BuildAction::Type::Archive ? "archive" : "link");
                return 1;
            }
            status[i] = 1;
            completed_actions++;
        }
    }

    const int completed = completed_actions.load();
    if (completed == 0 && skipped > 0) {
        std::println("    Finished {} (up to date, {} actions cached)",
                     package_label, skipped);
    } else if (completed > 0) {
        std::println("    Finished {} ({} actions, {} cached)",
                     package_label, completed, skipped);
    }

    nlohmann::json state;
    state["schema"] = 1;
    state["actions"] = nlohmann::json::object();
    for (size_t i = 0; i < plan.actions.size(); ++i) {
        state["actions"][plan.actions[i].id] = fingerprints[i];
    }
    if (!atomic_write_file(state_path, state.dump(2))) {
        std::println(std::cerr,
                     "bake: warning: failed to write incremental state at {}",
                     state_path.string());
    }

    return 0;
}

// ===== build.json reader =====

export BuildPlan read_build_json(const Path& json_path, const Path& project_root) {
    BuildPlan plan;
    plan.project_root = project_root;
    plan.state_dir = json_path.parent();

    auto content = read_file(json_path);
    if (!content) return plan;

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(*content);
    } catch (...) {
        return plan;
    }

    if (doc.contains("package") && doc["package"].is_object()) {
        plan.package_name = doc["package"].value("name", "");
        plan.package_version = doc["package"].value("version", "");
    }

    if (!doc.contains("actions")) return plan;

    auto action_path = [&](const std::string& value) {
        Path path(value);
        if (path.fs().is_absolute()) return path;
        return (project_root / path).lexically_normal();
    };

    // First pass: create all actions
    std::map<std::string, size_t> id_to_index;

    for (auto& jaction : doc["actions"]) {
        BuildAction action;
        action.id = jaction.value("id", "");
        action.description = jaction.value("description", action.id);

        std::string type_str = jaction.value("type", "compile");
        if (type_str == "compile") action.type = BuildAction::Type::Compile;
        else if (type_str == "link") action.type = BuildAction::Type::Link;
        else if (type_str == "archive") action.type = BuildAction::Type::Archive;
        else action.type = BuildAction::Type::Compile;

        for (auto& inp : jaction.value("inputs", std::vector<std::string>{})) {
            action.inputs.push_back(action_path(inp));
        }
        for (auto& out : jaction.value("outputs", std::vector<std::string>{})) {
            action.outputs.push_back(action_path(out));
        }
        for (auto& cmd : jaction.value("command", std::vector<std::string>{})) {
            action.command.push_back(cmd);
        }

        size_t idx = plan.actions.size();
        id_to_index[action.id] = idx;
        plan.actions.push_back(std::move(action));
    }

    // Second pass: resolve depends_on (string IDs → indices)
    for (auto& jaction : doc["actions"]) {
        std::string id = jaction.value("id", "");
        auto it = id_to_index.find(id);
        if (it == id_to_index.end()) continue;

        for (auto& dep : jaction.value("depends_on", std::vector<std::string>{})) {
            auto dep_it = id_to_index.find(dep);
            if (dep_it != id_to_index.end()) {
                plan.actions[it->second].depends_on.push_back(dep_it->second);
            }
        }
    }

    // Find primary output (first link/archive action's output)
    for (auto& a : plan.actions) {
        if ((a.type == BuildAction::Type::Link || a.type == BuildAction::Type::Archive) &&
            !a.outputs.empty()) {
            plan.primary_output = a.outputs[0];
            break;
        }
    }

    return plan;
}

// ===== compile_commands.json output =====

export void write_compile_commands(const BuildPlan& plan, const Path& output_path) {
    nlohmann::json commands = nlohmann::json::array();

    for (auto& action : plan.actions) {
        if (!action.is_compile()) continue;
        if (action.inputs.empty() || action.outputs.empty()) continue;

        nlohmann::json entry;
        entry["directory"] = plan.project_root.absolute().string();

        // Join command args into a single string
        std::string cmd_str;
        for (size_t i = 0; i < action.command.size(); ++i) {
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
