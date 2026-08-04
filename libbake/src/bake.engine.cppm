module;

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <filesystem>
#include <thread>
#include <mutex>
#include <mutex>
#include <atomic>
#include <cstdio>

#include <nlohmann/json.hpp>

export module bake.engine;

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
                    if (!name.empty() && name != "std") {
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
    std::string id;
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
    Path obj_dir;
    Path bmi_dir;

    // Apply build options to std version etc.
    // Returns nullopt if configuration is valid.
};

// Dependent source entry — populated by the CLI layer from the lockfile/cache.
export struct DepSourceEntry {
    std::string dep_name;
    Path source;
};

// Create a build plan using convention rules
export BuildPlan create_convention_plan(
        const Manifest& manifest,
        const Layout& layout,
        const Toolchain& tc,
        const std::map<std::string, BuildOption>& options,
        const std::vector<DepSourceEntry>& dep_sources = {},
        const std::vector<Path>& dep_include_dirs = {}) {

    BuildPlan plan;
    plan.project_root = layout.root;
    plan.obj_dir = layout.obj_dir;
    plan.bmi_dir = layout.bmi_dir;

    // Ensure build dirs exist
    plan.obj_dir.mkdir_recursive();
    plan.bmi_dir.mkdir_recursive();

    if (!manifest.has_package()) return plan;

    const auto& pkg = *manifest.package;
    std::string std_ver = pkg.std_version;

    // Discover sources
    auto sources = discover_sources(layout.source_dir, layout.public_dir);

    // Build include dirs
    std::vector<Path> include_dirs;
    if (layout.public_dir.is_directory()) {
        include_dirs.push_back(layout.public_dir);
    }
    // Resolve path dependencies — add their public/ as include dirs
    for (auto& [name, dep] : manifest.dependencies) {
        if (dep.is_path_dep) {
            Path dep_dir = manifest.project_dir / dep.path;
            Path dep_public = dep_dir / "public";
            if (dep_public.is_directory()) {
                include_dirs.push_back(dep_public);
            }
        }
    }

    // Add include dirs from locked bake-native dependencies (populated by CLI)
    for (auto& inc : dep_include_dirs) {
        include_dirs.push_back(inc);
    }

    // Build module graph
    ModuleGraph mod_graph;
    if (!sources.module_interfaces.empty()) {
        mod_graph = ModuleGraph::build(tc, sources, std_ver, include_dirs);
    }

    // Map module name → BMI path
    std::map<std::string, Path> module_bmi;

    // Phase 1: Compile module interfaces (in dependency order)
    for (auto& mod_name : mod_graph.sorted) {
        auto& info = mod_graph.modules[mod_name];
        Path src(info.source_path);
        Path bmi = plan.bmi_dir / (mod_name + ".pcm");
        Path obj = plan.obj_dir / (src.stem_string() + ".o");

        CompileConfig cc;
        cc.source = src;
        cc.output = obj;
        cc.std_ver = std_ver;
        cc.include_dirs = include_dirs;
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

        BuildAction action;
        action.type = BuildAction::Type::CompileModule;
        action.id = "module:" + mod_name;
        action.description = "Compiling module " + mod_name;
        action.inputs = {src};
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
        Path obj = plan.obj_dir / (src.stem_string() + ".o");

        CompileConfig cc;
        cc.source = src;
        cc.output = obj;
        cc.std_ver = std_ver;
        cc.include_dirs = include_dirs;
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

        BuildAction action;
        action.type = BuildAction::Type::Compile;
        action.id = "compile:" + src.string();
        action.description = "Compiling " + src.filename_string();
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

    // Phase 2b: Compile sources from bake-native locked dependencies
    for (auto& ds : dep_sources) {
        // Unique object name to avoid collisions with project sources
        std::string obj_name = "dep__" + ds.dep_name + "__" + ds.source.stem_string() + ".o";
        Path obj = plan.obj_dir / obj_name;

        CompileConfig cc;
        cc.source = ds.source;
        cc.output = obj;
        cc.std_ver = std_ver;
        cc.include_dirs = include_dirs;
        cc.use_pic = (pkg.type == PackageType::SharedLib);

        BuildAction action;
        action.type = BuildAction::Type::Compile;
        action.id = "dep-compile:" + ds.dep_name + ":" + ds.source.filename_string();
        action.description = "Compiling dep " + ds.dep_name + "/" + ds.source.filename_string();
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
    if (!all_sources.empty() || !mod_graph.sorted.empty() || !dep_sources.empty()) {
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
            link_action.id = "archive:" + pkg.name;
            link_action.description = "Creating " + out_name;

            LinkConfig lc;
            lc.inputs = obj_files;
            lc.output = output;
            lc.type = pkg.type;
            link_action.command = make_archive_command(tc, lc);
        } else {
            link_action.type = BuildAction::Type::Link;
            link_action.id = "link:" + pkg.name;
            link_action.description = "Linking " + out_name;

            LinkConfig lc;
            lc.inputs = obj_files;
            lc.output = output;
            lc.type = pkg.type;
            link_action.command = make_link_command(tc, lc);
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

// ===== Executor =====

export int execute_plan(BuildPlan& plan, int jobs) {
    if (plan.actions.empty()) {
        std::printf("bake: nothing to build\n");
        return 0;
    }

    if (jobs <= 0) {
        jobs = static_cast<int>(std::thread::hardware_concurrency());
        if (jobs == 0) jobs = 4;
    }

    int total = 0;
    int skipped = 0;
    int failed = 0;

    // Track completion
    std::vector<int> status(plan.actions.size(), 0);  // 0=pending, 1=done, -1=failed
    std::atomic<int> actions_done{0};

    // Execute module interfaces and compiles
    // For simplicity: module interfaces sequential, regular compiles parallel

    // First pass: compile module interfaces (sequential, in order)
    for (size_t i = 0; i < plan.actions.size(); ++i) {
        if (plan.actions[i].type == BuildAction::Type::CompileModule) {
            auto& action = plan.actions[i];
            if (!needs_rebuild(action)) {
                status[i] = 1;
                actions_done++;
                skipped++;
                continue;
            }

            std::printf("[%d/%zu] %s\n", actions_done.load() + 1, plan.actions.size(),
                        action.description.c_str());

            auto result = run_process(action.command, plan.project_root);
            if (!result.success()) {
                if (!result.stderr_output.empty()) {
                    std::fprintf(stderr, "%s", result.stderr_output.c_str());
                }
                std::fprintf(stderr, "bake: failed to compile module\n");
                status[i] = -1;
                failed++;
                return 1;
            }
            status[i] = 1;
            actions_done++;
            total++;
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
        std::mutex mtx;
        std::vector<std::thread> threads;
        std::atomic<int> compile_failed{0};

        int actual_jobs = std::min(jobs, static_cast<int>(compile_indices.size()));

        auto worker = [&](size_t start, size_t stride) {
            for (size_t k = start; k < compile_indices.size(); k += stride) {
                if (compile_failed.load() > 0) return;

                size_t i = compile_indices[k];
                auto& action = plan.actions[i];

                {
                    std::lock_guard<std::mutex> lock(mtx);
                    if (!needs_rebuild(action)) {
                        status[i] = 1;
                        actions_done++;
                        skipped++;
                        continue;
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(mtx);
                    std::printf("[%d/%zu] %s\n", actions_done.load() + 1,
                                plan.actions.size(), action.description.c_str());
                }

                auto result = run_process(action.command, plan.project_root);
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    if (!result.success()) {
                        if (!result.stderr_output.empty()) {
                            std::fprintf(stderr, "%s", result.stderr_output.c_str());
                        }
                        compile_failed++;
                        status[i] = -1;
                        return;
                    }
                    status[i] = 1;
                    actions_done++;
                    total++;
                }
            }
        };

        for (int t = 0; t < actual_jobs; ++t) {
            threads.emplace_back(worker, t, actual_jobs);
        }
        for (auto& th : threads) th.join();

        if (compile_failed.load() > 0) {
            std::fprintf(stderr, "bake: compilation failed\n");
            return 1;
        }
    }

    // Third pass: link/archive
    for (size_t i = 0; i < plan.actions.size(); ++i) {
        if (plan.actions[i].type == BuildAction::Type::Link ||
            plan.actions[i].type == BuildAction::Type::Archive) {
            auto& action = plan.actions[i];

            if (!needs_rebuild(action)) {
                status[i] = 1;
                skipped++;
                continue;
            }

            std::printf("[%d/%zu] %s\n", actions_done.load() + 1, plan.actions.size(),
                        action.description.c_str());

            auto result = run_process(action.command, plan.project_root);
            if (!result.success()) {
                if (!result.stderr_output.empty()) {
                    std::fprintf(stderr, "%s", result.stderr_output.c_str());
                }
                std::fprintf(stderr, "bake: %s failed\n",
                             action.type == BuildAction::Type::Archive ? "archive" : "link");
                return 1;
            }
            status[i] = 1;
            total++;
        }
    }

    if (total == 0 && skipped > 0) {
        std::printf("bake: up to date (%d actions skipped)\n", skipped);
    } else if (total > 0) {
        std::printf("bake: build complete (%d compiled, %d skipped)\n", total, skipped);
    }

    return 0;
}

// ===== build.json reader =====

export BuildPlan read_build_json(const Path& json_path, const Path& project_root) {
    BuildPlan plan;
    plan.project_root = project_root;

    auto content = read_file(json_path);
    if (!content) return plan;

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(*content);
    } catch (...) {
        return plan;
    }

    if (!doc.contains("actions")) return plan;

    // First pass: create all actions
    std::map<std::string, size_t> id_to_index;

    for (auto& jaction : doc["actions"]) {
        BuildAction action;
        action.id = jaction.value("id", "");
        action.description = action.id;

        std::string type_str = jaction.value("type", "compile");
        if (type_str == "compile") action.type = BuildAction::Type::Compile;
        else if (type_str == "link") action.type = BuildAction::Type::Link;
        else if (type_str == "archive") action.type = BuildAction::Type::Archive;
        else action.type = BuildAction::Type::Compile;

        for (auto& inp : jaction.value("inputs", std::vector<std::string>{})) {
            action.inputs.push_back(Path(inp));
        }
        for (auto& out : jaction.value("outputs", std::vector<std::string>{})) {
            action.outputs.push_back(Path(out));
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
