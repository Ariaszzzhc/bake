export module bake.buildsystem.query;

import std;
import bake.util;
import bake.json;
import bake.buildsystem.project;
import bake.buildsystem.moid;
import bake.buildsystem.graph;
import bake.buildsystem.engine;
import bake.buildsystem.package;
import bake.toolchain.target;

// ============================================================
// bake.buildsystem.query — the file→command surface
//
// The read-only view over the configure pipeline: resolve →
// configure → build_graph, without executing anything. This is
// what `bake compdb` prints and what the LSP consumes to answer
// "which compile command owns this file?" — the build graph is
// the single source of truth; there is no separate compilation
// database.
// ============================================================

namespace bake {

// One compilable file with its full driver argv (exe, "c++"/"cc",
// flags..., source) — the same command shape the executor runs.
export struct CompileEntry {
    Path file;                         // absolute source path
    Path output;                       // object (or BMI) path
    std::vector<std::string> command;
    std::string moid_id;
    bool is_module_interface = false;
};

export struct CompileDb {
    Path project_root;
    Path out_dir;
    std::string profile;
    std::string target_triple;
    std::vector<CompileEntry> entries;

    // Exact-match lookup by path. Both sides are canonicalized:
    // editors hand unresolved spellings (/var on macOS) while the
    // build graph stores the resolved root (/private/var).
    const CompileEntry* find(const Path& file) const {
        std::error_code ec;
        std::filesystem::path key = std::filesystem::weakly_canonical(
            file.absolute().lexically_normal().fs(), ec);
        if (ec) key = file.absolute().lexically_normal().fs();
        for (const auto& entry : entries) {
            std::error_code ec2;
            std::filesystem::path candidate =
                std::filesystem::weakly_canonical(entry.file.fs(), ec2);
            if (ec2) candidate = entry.file.fs();
            if (candidate == key) return &entry;
        }
        return nullptr;
    }
};

// Lockfile + content cache → the SourceIndex consumed by graph
// resolution. The bridge between the dependency domain (package)
// and the graph; shared by the build, compdb and LSP paths.
export SourceIndex source_index_from_lock(const Path& root) {
    SourceIndex index;
    auto lockfile = Lockfile::load(root / "bake.lock");
    if (!lockfile) return index;
    const Path cache_dir = get_cache_dir();
    for (auto& [key, dep] : lockfile->deps) {
        if (!dep.is_remote()) continue;
        LockedSource source;
        source.url = normalize_dependency_url(dep.url);
        source.ref = dep.is_archive() ? "" : dep.ref;
        source.ref_type = dep.is_archive() ? "archive" : dep.ref_type;
        source.commit = dep.commit;
        source.integrity = dep.integrity;
        source.cache_hash = dep.cache_hash();
        source.cache_dir = cache_dir / source.cache_hash;
        index.entries.push_back(std::move(source));
    }
    return index;
}

// Resolve → configure → build_graph without executing. Mirrors the
// build command's pipeline exactly (same selection defaults, same
// profile resolution) so commands and diagnostics always agree with
// what a build would do. `offline` refuses to touch the network —
// unresolved dependencies surface as errors telling the user to run
// `bake update`.
export std::expected<CompileDb, std::string> build_compile_db(
        const Path& root,
        const TargetSpec& target,
        const std::string& profile = "dev",
        bool offline = true) {
    auto manifest = Manifest::load(root);
    if (!manifest)
        return std::unexpected("failed to load bake.toml at " +
                               root.string());

    if (profile != "dev" && profile != "release" &&
        !manifest->profiles.contains(profile)) {
        return std::unexpected("unknown profile '" + profile +
                               "' (declare it in bake.toml or use "
                               "'dev'/'release')");
    }

    BuildSelection selection;
    auto member = resolve_workspace_member_selection(*manifest,
                                                      std::nullopt);
    if (!member) return std::unexpected(member.error());
    selection.workspace_member = *member;
    selection.target_triple =
        target.is_native() ? detect_host_target().triple_ : target.triple_;

    ResolvePolicy policy;
    policy.offline = offline;
    auto outer = resolve_moid_graph(*manifest, selection, policy,
                                    source_index_from_lock(root));
    if (!outer) return std::unexpected(outer.error());

    std::string triple_dir =
        target.is_native() ? detect_host_target().triple_ : target.triple_;
    Path out_dir = root / "out" / triple_dir;

    auto configured =
        configure_moid_graph(*outer, target, out_dir, root, profile);
    if (!configured) return std::unexpected(configured.error());

    auto graph = build_graph(*outer, target, out_dir, root);
    if (!graph) return std::unexpected(graph.error());

    CompileDb db;
    db.project_root = root;
    db.out_dir = out_dir;
    db.profile = profile;
    db.target_triple = triple_dir;
    db.entries.reserve(graph->actions.size());
    for (auto& action : graph->actions) {
        if (!action.is_compile()) continue;
        if (action.inputs.empty()) continue;
        CompileEntry entry;
        entry.file = action.inputs.front().absolute().lexically_normal();
        entry.output = action.outputs.empty()
                           ? Path()
                           : action.outputs.front();
        entry.command = action.command;
        entry.moid_id = action.moid_id;
        entry.is_module_interface =
            action.type == BuildAction::Type::CompileModule;
        db.entries.push_back(std::move(entry));
    }
    return db;
}

// Project a built graph into the same entry shape (used by the build
// command to refresh compile_commands.json after a build).
export CompileDb compile_db_from_graph(const BuildGraph& graph,
                                       const Path& project_root) {
    CompileDb db;
    db.project_root = project_root;
    for (auto& action : graph.actions) {
        if (!action.is_compile()) continue;
        if (action.inputs.empty()) continue;
        CompileEntry entry;
        entry.file = action.inputs.front().absolute().lexically_normal();
        entry.output =
            action.outputs.empty() ? Path() : action.outputs.front();
        entry.command = action.command;
        entry.moid_id = action.moid_id;
        entry.is_module_interface =
            action.type == BuildAction::Type::CompileModule;
        db.entries.push_back(std::move(entry));
    }
    return db;
}

// compile_commands.json (JSON Compilation Database) — one writer for
// the build command's post-build refresh and `bake compdb`.
export void write_compdb_json(const CompileDb& db, const Path& output) {
    json::Value commands = json::Value::array();
    for (const auto& entry : db.entries) {
        json::Value item;
        item["directory"] =
            db.project_root.absolute().fs().generic_string();
        item["file"] = entry.file.fs().generic_string();
        if (!entry.output.string().empty())
            item["output"] = entry.output.absolute().fs().generic_string();

        std::string cmd_str;
        for (std::size_t i = 0; i < entry.command.size(); ++i) {
            if (i > 0) cmd_str += " ";
            cmd_str += entry.command[i];
        }
        item["command"] = cmd_str;
        commands.push_back(std::move(item));
    }
    write_file(output, commands.dump(2) + "\n");
}

} // namespace bake
