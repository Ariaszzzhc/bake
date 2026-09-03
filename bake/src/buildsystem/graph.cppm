export module bake.buildsystem.graph;

import std;
import bake.util;
import bake.buildsystem.project;
import bake.buildsystem.moid;

namespace bake {

export struct MoidId {
    std::string value;
    auto operator<=>(const MoidId&) const = default;
};

export struct MoidEdge {
    std::string alias;
    MoidId target;
    std::vector<std::string> features;
    bool default_features = true;  // edge contributes target's defaults
};

export struct MoidNode {
    MoidId id;
    MoidDeclaration declaration;
    std::vector<MoidEdge> dependencies;
    // Non-moid dependency source directories (alias → absolute path).
    // Populated for remote/path deps that have no bake.toml — their
    // source is referenced by build.cpp via dep_src_dir().
    std::map<std::string, std::string> source_deps;
};

export struct MoidGraph {
    std::map<MoidId, MoidNode> nodes;
    std::vector<MoidId> roots;
};


export struct BuildSelection {
    std::optional<std::string> workspace_member;
    std::vector<std::string> root_features;
    // Build triple used to filter target-scoped dependencies. Native
    // builds carry the detected host triple.
    std::string target_triple;
};

export struct ResolvePolicy {
    bool offline = false;
};

// A remote source pre-resolved from the lockfile and content cache.
// graph does not read bake.lock — the caller resolves lock entries into
// concrete source directories and injects them here.
export struct LockedSource {
    std::string url;        // normalized URL
    std::string ref;        // git ref value ("" for archive deps)
    std::string ref_type;   // "tag" | "branch" | "rev" | "head" | "archive"
    std::string commit;     // resolved commit (git deps only)
    std::string integrity;  // "sha256-<tree-hash>"
    std::string cache_hash; // bare hex tree hash
    Path cache_dir;         // content-addressed source directory

    // Canonical moid identity for a native source from this location.
    std::string identity() const {
        if (ref_type == "archive")
            return "archive:" + url + ":" + cache_hash;
        return "git:" + url + "#" + commit + ":" + cache_hash;
    }
};

export struct SourceIndex {
    std::vector<LockedSource> entries;

    const LockedSource* find(const std::string& normalized_url,
                             const std::string& ref_type,
                             const std::string& ref) const {
        for (const auto& entry : entries)
            if (entry.url == normalized_url && entry.ref_type == ref_type &&
                entry.ref == ref)
                return &entry;
        return nullptr;
    }

    const LockedSource* find(const Dependency& dep) const {
        if (dep.is_archive())
            return find(normalize_dependency_url(dep.url), "archive", "");
        auto [type, value] = dep.git_ref();
        return find(normalize_dependency_url(dep.url), type, value);
    }
};

namespace {

std::expected<Path, std::string> canonical_source_root(const Path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path.fs(), error);
    if (error) {
        return std::unexpected(
            "failed to make source root absolute: " + path.string());
    }

    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        return std::unexpected(
            "failed to canonicalize source root: " + path.string());
    }
    return Path(canonical);
}

std::string canonical_path_string(const Path& path) {
    return path.fs().generic_string();
}

MoidDeclaration manifest_declaration(const Manifest& manifest,
                                     const MoidId& id,
                                     const Path& source_root) {
    MoidDeclaration declaration;
    declaration.id = id.value;
    declaration.name = manifest.moid->name;
    declaration.version = manifest.moid->version;
    declaration.type = manifest.moid->type;
    declaration.root = source_root.string();
    declaration.cxx_std = manifest.moid->cxx_std;
    declaration.c_std = manifest.moid->c_std;
    return declaration;
}

std::string alias_path_string(const std::vector<std::string>& path) {
    return join(path, " -> ");
}

} // namespace

export std::expected<std::optional<std::string>, std::string>
resolve_workspace_member_selection(
        const Manifest& root,
        const std::optional<std::string>& selector) {
    if (!selector || !root.is_workspace()) return selector;

    auto root_path = canonical_source_root(root.project_dir);
    if (!root_path) return std::unexpected(root_path.error());

    struct Candidate {
        std::string canonical_member;
        std::string display_name;
        std::string source_key;
    };
    std::vector<Candidate> candidates;

    for (const auto& member : root.workspace->members) {
        auto member_root = canonical_source_root(*root_path / member.c_str());
        if (!member_root) return std::unexpected(member_root.error());

        auto manifest = Manifest::load(*member_root);
        if (!manifest || !manifest->has_moid()) {
            return std::unexpected(
                "workspace member '" + member + "' does not declare a [package]");
        }

        std::error_code error;
        auto relative = std::filesystem::relative(
            member_root->fs(), root_path->fs(), error);
        if (error) {
            relative = member_root->fs().lexically_relative(root_path->fs());
        }
        std::string canonical_member =
            relative.lexically_normal().generic_string();
        if (canonical_member.empty()) canonical_member = ".";
        candidates.push_back(Candidate{
            std::move(canonical_member), manifest->moid->name,
            canonical_path_string(*member_root)});
    }

    auto selector_root =
        canonical_source_root(*root_path / selector->c_str());
    if (!selector_root) return std::unexpected(selector_root.error());
    const std::string selector_key = canonical_path_string(*selector_root);
    for (const auto& candidate : candidates) {
        if (candidate.source_key == selector_key)
            return candidate.canonical_member;
    }

    std::vector<std::string> display_matches;
    for (const auto& candidate : candidates) {
        if (candidate.display_name == *selector) {
            display_matches.push_back(candidate.canonical_member);
        }
    }
    std::ranges::sort(display_matches);

    if (display_matches.size() == 1) return display_matches.front();
    if (display_matches.empty()) {
        return std::unexpected(
            "workspace member selector '" + *selector + "' not found");
    }

    std::string names;
    for (const auto& member : display_matches) {
        if (!names.empty()) names += ", ";
        names += "'" + member + "'";
    }
    return std::unexpected(
        "workspace member selector '" + *selector +
        "' is ambiguous: " + names);
}

export std::expected<std::vector<MoidId>, std::string>
topological_moids(const MoidGraph& graph) {
    std::set<MoidId> root_ids;
    for (const auto& root : graph.roots) {
        if (!root_ids.insert(root).second) {
            return std::unexpected(
                "duplicate root moid id '" + root.value + "'");
        }
        if (!graph.nodes.contains(root)) {
            return std::unexpected(
                "missing root moid '" + root.value + "'");
        }
    }

    std::set<MoidId> declared_ids;
    for (const auto& [id, node] : graph.nodes) {
        if (node.id != id || node.declaration.id != id.value) {
            return std::unexpected(
                "duplicate or inconsistent moid id '" + id.value + "'");
        }
        if (!declared_ids.insert(node.id).second) {
            return std::unexpected(
                "duplicate moid id '" + node.id.value + "'");
        }

        std::set<std::string> aliases;
        for (const auto& edge : node.dependencies) {
            if (!aliases.insert(edge.alias).second) {
                return std::unexpected(
                    "duplicate dependency alias '" + edge.alias +
                    "' in moid '" + node.declaration.name + "'");
            }
            auto target = graph.nodes.find(edge.target);
            if (target == graph.nodes.end()) {
                return std::unexpected(
                    "moid '" + node.declaration.name + "' dependency '" +
                    edge.alias + "' targets missing moid '" +
                    edge.target.value + "'");
            }
            if (target->second.declaration.type == MoidType::Executable) {
                return std::unexpected(
                    "moid '" + node.declaration.name +
                    "' cannot use executable moid '" +
                    target->second.declaration.name +
                    "' as a normal dependency");
            }
        }
    }

    std::map<MoidId, int> color;
    std::vector<MoidId> node_stack;
    std::vector<std::string> edge_stack;
    std::vector<MoidId> sorted;

    std::function<std::expected<void, std::string>(const MoidId&)> visit;
    visit = [&](const MoidId& id) -> std::expected<void, std::string> {
        color[id] = 1;
        node_stack.push_back(id);

        const auto& node = graph.nodes.at(id);
        for (const auto& edge : node.dependencies) {
            const int target_color = color[edge.target];
            if (target_color == 0) {
                edge_stack.push_back(edge.alias);
                auto result = visit(edge.target);
                edge_stack.pop_back();
                if (!result) return result;
            } else if (target_color == 1) {
                auto cycle_start = std::find(
                    node_stack.begin(), node_stack.end(), edge.target);
                const std::size_t index = static_cast<std::size_t>(
                    std::distance(node_stack.begin(), cycle_start));

                std::vector<std::string> path;
                path.push_back(
                    graph.nodes.at(node_stack[index]).declaration.name);
                for (std::size_t i = index; i < edge_stack.size(); ++i)
                    path.push_back(edge_stack[i]);
                path.push_back(edge.alias);
                return std::unexpected(
                    "moid dependency cycle: " + alias_path_string(path));
            }
        }

        node_stack.pop_back();
        color[id] = 2;
        sorted.push_back(id);
        return {};
    };

    for (const auto& root : graph.roots) {
        if (color[root] != 0) continue;
        auto result = visit(root);
        if (!result) return std::unexpected(result.error());
    }
    for (const auto& [id, _] : graph.nodes) {
        if (color[id] == 0) {
            return std::unexpected(
                "orphan moid '" + id.value +
                "' is not reachable from any graph root");
        }
    }

    return sorted;
}

export std::expected<std::vector<MoidId>, std::string>
configuration_topological_moids(const MoidGraph& graph) {
    MoidGraph provisional = graph;
    for (auto& [_, node] : provisional.nodes) {
        const Path moid_dir(node.declaration.root);
        if ((moid_dir / "build.cpp").is_regular_file()) {
            node.declaration.type = MoidType::Lib;
        }
    }
    return topological_moids(provisional);
}

// Build a resolved MoidGraph from a root manifest.
//
// Walks the dependency closure recursively:
//   workspace members → path deps → remote deps (via SourceIndex)
// Each node gets a canonical identity (workspace:path, path:<hash>,
// git:<url>#<commit>, archive:<url>:<hash>). Non-moid sources (no bake.toml)
// are recorded as source_deps for build.cpp access but don't become graph
// nodes.
//
// Build options are resolved after the graph structure is complete:
// each option request is validated against the target package's declared
// [options], and conflicts between multiple requesters are detected.
export std::expected<MoidGraph, std::string>
resolve_moid_graph(const Manifest& root,
                   const BuildSelection& selection,
                   const ResolvePolicy& policy,
                   const SourceIndex& sources) {
    auto root_path = canonical_source_root(root.project_dir);
    if (!root_path) return std::unexpected(root_path.error());

    struct SourceRecord {
        std::string source_key;
        Manifest manifest;
    };

    MoidGraph graph;
    std::map<MoidId, SourceRecord> source_records;
    std::vector<MoidId> worklist;
    std::map<std::string, MoidId> workspace_ids;
    std::map<std::string, std::string> workspace_member_spellings;
    std::map<std::string, Manifest> workspace_manifests;

    auto register_source = [&](const MoidId& id,
                               std::string source_key,
                               Manifest manifest)
            -> std::expected<bool, std::string> {
        auto existing = source_records.find(id);
        if (existing != source_records.end()) {
            if (existing->second.source_key != source_key) {
                return std::unexpected(
                    "duplicate moid id '" + id.value + "' for '" +
                    existing->second.source_key + "' and '" + source_key + "'");
            }
            return false;
        }

        if (!manifest.has_moid()) {
            return std::unexpected(
                "source '" + manifest.project_dir.string() +
                "' does not declare a [package]");
        }

        const Path source_root = manifest.project_dir;
        MoidNode node;
        node.id = id;
        node.declaration = manifest_declaration(manifest, id, source_root);
        graph.nodes.emplace(id, std::move(node));
        source_records.emplace(
            id, SourceRecord{std::move(source_key), std::move(manifest)});
        worklist.push_back(id);
        return true;
    };

    if (root.is_workspace()) {
        for (const auto& member : root.workspace->members) {
            auto member_root = canonical_source_root(*root_path / member.c_str());
            if (!member_root) return std::unexpected(member_root.error());

            auto manifest = Manifest::load(*member_root);
            if (!manifest || !manifest->has_moid()) {
                return std::unexpected(
                    "workspace member '" + member + "' does not declare a [package]");
            }
            manifest->project_dir = *member_root;

            std::error_code error;
            auto relative = std::filesystem::relative(
                member_root->fs(), root_path->fs(), error);
            if (error) relative = member_root->fs().lexically_relative(root_path->fs());
            std::string canonical_member = relative.lexically_normal().generic_string();
            if (canonical_member.empty()) canonical_member = ".";

            MoidId id{"workspace:" + canonical_member};
            const std::string path_key = canonical_path_string(*member_root);
            auto [existing, inserted] = workspace_ids.emplace(path_key, id);
            if (!inserted) {
                return std::unexpected(
                    "duplicate canonical workspace member: '" +
                    workspace_member_spellings.at(path_key) + "' and '" +
                    member + "' both resolve to '" + existing->second.value + "'");
            }
            workspace_member_spellings.emplace(path_key, member);
            workspace_manifests[path_key] = *manifest;
        }

        bool selected_member_found = !selection.workspace_member.has_value();
        for (const auto& member : root.workspace->members) {
            auto member_root = canonical_source_root(*root_path / member.c_str());
            if (!member_root) return std::unexpected(member_root.error());
            const std::string path_key = canonical_path_string(*member_root);
            const Manifest& manifest = workspace_manifests.at(path_key);
            const MoidId id = workspace_ids.at(path_key);

            if (selection.workspace_member &&
                id.value != "workspace:" + *selection.workspace_member) {
                continue;
            }
            selected_member_found = true;

            auto added = register_source(
                id, "workspace:" + path_key, manifest);
            if (!added) return std::unexpected(added.error());
            if (std::find(graph.roots.begin(), graph.roots.end(), id) ==
                graph.roots.end()) {
                graph.roots.push_back(id);
            }
        }

        if (!selected_member_found) {
            return std::unexpected(
                "workspace member '" + *selection.workspace_member + "' not found");
        }
    } else if (root.has_moid()) {
        Manifest manifest = root;
        manifest.project_dir = *root_path;
        const std::string path_key = canonical_path_string(*root_path);
        MoidId id{"path:" + SHA256::hex(path_key)};
        auto added = register_source(
            id, path_key, std::move(manifest));
        if (!added) return std::unexpected(added.error());
        graph.roots.push_back(id);
    } else {
        return std::unexpected("project does not declare a [package] or [workspace]");
    }

    // ── Feature activation + conditional dependency expansion ──
    //
    // A feature's dependencies are conditional edges: they exist only when
    // the feature is active, and activation arrives on edges from
    // dependents that may themselves be feature-gated. Activation can also
    // shrink — conflict demotion removes defaults — so expansion and
    // unification iterate to a fixpoint over the changing sets.
    std::map<MoidId, std::set<std::string>> activated;
    std::map<MoidId, std::set<std::string>> expanded_with;
    std::map<MoidId, std::set<std::string>> demoted_notes;

    auto is_root = [&](const MoidId& id) {
        return std::ranges::any_of(
            graph.roots, [&](const MoidId& root) { return root == id; });
    };

    // Unify every node's activation set (defaults ∪ incoming edges ∪ CLI,
    // platform-filtered), validate declarations and platforms, and apply
    // explicit-beats-default conflict demotion. Returns whether any
    // activation changed.
    auto unify = [&]() -> std::expected<bool, std::string> {
        bool changed = false;
        for (auto& [id, node] : graph.nodes) {
            const Manifest& manifest = source_records.at(id).manifest;
            std::set<std::string> effective;
            std::map<std::string, std::string> origin;
            std::map<std::string, bool> hard;
            // Incoming edges vote on this package's default feature set:
            // an edge may opt out (default-features = false) so only its
            // explicit `features` activate here. Union semantics — one
            // edge keeping defaults is enough to keep them on, and a
            // package built as its own root has no incoming edges and
            // keeps its defaults.
            bool saw_defaults_on = false, saw_defaults_off = false;
            for (const auto& [_, source] : graph.nodes) {
                for (const auto& edge : source.dependencies) {
                    if (edge.target != id) continue;
                    if (edge.default_features)
                        saw_defaults_on = true;
                    else
                        saw_defaults_off = true;
                    for (const auto& name : edge.features) {
                        auto it = manifest.features.find(name);
                        if (it == manifest.features.end()) {
                            return std::unexpected(
                                "feature '" + name + "' is not declared by "
                                "package '" + node.declaration.name +
                                "' (requested by dependency '" + edge.alias +
                                "' of '" + source.declaration.name + "')");
                        }
                        if (!feature_supports_target(
                                it->second, selection.target_triple)) {
                            std::string supported =
                                it->second.platforms.empty()
                                    ? "*"
                                    : join(it->second.platforms, ", ");
                            return std::unexpected(
                                "feature '" + name + "' of package '" +
                                node.declaration.name +
                                "' does not support target '" +
                                selection.target_triple +
                                "' (activated by dependency '" + edge.alias +
                                "' of '" + source.declaration.name +
                                "'; supported: " + supported + ")");
                        }
                        effective.insert(name);
                        origin.emplace(name,
                                       "dependency '" + edge.alias + "' of '" +
                                           source.declaration.name + "'");
                        hard[name] = true;
                    }
                }
            }
            if (saw_defaults_off && saw_defaults_on)
                std::println(std::cerr,
                             "bake: package '{}' keeps default features: "
                             "another edge still contributes them",
                             node.declaration.name);

            if (!saw_defaults_off || saw_defaults_on) {
                for (const auto& name : manifest.default_features) {
                    auto it = manifest.features.find(name);
                    if (it == manifest.features.end()) {
                        return std::unexpected(
                            "default feature '" + name + "' of package '" +
                            node.declaration.name + "' is not declared");
                    }
                    if (!feature_supports_target(it->second,
                                                 selection.target_triple))
                        continue;
                    effective.insert(name);
                    origin.emplace(name, "default");
                    hard.emplace(name, false);
                }
            }

            if (is_root(id)) {
                for (const auto& name : selection.root_features) {
                    auto it = manifest.features.find(name);
                    if (it == manifest.features.end()) {
                        return std::unexpected(
                            "feature '" + name + "' is not declared by "
                            "package '" + node.declaration.name +
                            "' (requested on the command line)");
                    }
                    if (!feature_supports_target(
                            it->second, selection.target_triple)) {
                        return std::unexpected(
                            "feature '" + name + "' of package '" +
                            node.declaration.name +
                            "' does not support target '" +
                            selection.target_triple +
                            "' (requested on the command line)");
                    }
                    effective.insert(name);
                    origin.emplace(name, "command line");
                    hard[name] = true;
                }
            }

            // Conflicts follow explicit-beats-default (see
            // demote_conflicting_defaults in project.cppm): a default-only
            // feature yields to an explicitly activated one; two explicit
            // or two default features that clash are an error.
            std::set<std::string> demote;
            for (const auto& [name, name_hard] : hard) {
                if (!name_hard) continue;
                for (const auto& other :
                     manifest.features.at(name).conflicts)
                    if (effective.count(other) && !hard.at(other))
                        demote.insert(other);
            }
            for (const auto& name : demote) {
                if (demoted_notes[id].insert(name).second)
                    std::println(std::cerr,
                                 "bake: feature '{}' of package '{}' is "
                                 "disabled: it conflicts with an "
                                 "explicitly activated feature",
                                 name, node.declaration.name);
                effective.erase(name);
            }
            for (const auto& name : effective) {
                for (const auto& other :
                     manifest.features.at(name).conflicts) {
                    if (!effective.count(other)) continue;
                    return std::unexpected(
                        "features '" + name + "' and '" + other +
                        "' of package '" + node.declaration.name +
                        "' are mutually exclusive (activated by " +
                        origin.at(name) + " and " + origin.at(other) + ")");
                }
            }

            auto& known = activated[id];
            if (known != effective) {
                known = std::move(effective);
                changed = true;
            }
            node.declaration.active_features.assign(known.begin(),
                                                     known.end());
        }
        return changed;
    };

    // Expand dependency edges from the current activation sets. Nodes are
    // revisited whenever their activation changes — including shrinkage
    // from conflict demotion, which removes feature-conditional edges.
    auto expand = [&]() -> std::expected<void, std::string> {
        for (std::size_t index = 0; index < worklist.size(); ++index) {
            const MoidId source_id = worklist[index];
            const std::set<std::string> features = activated[source_id];
            auto [marker_entry, marker_fresh] =
                expanded_with.try_emplace(source_id);
            std::set<std::string>& marker = marker_entry->second;
            if (!marker_fresh && marker == features) continue;
            marker = features;
            const SourceRecord source = source_records.at(source_id);
            auto& source_node = graph.nodes.at(source_id);
            // Edges are a pure function of the activation set — rebuild
            // them from scratch so demoted features lose theirs.
            source_node.dependencies.clear();
            source_node.source_deps.clear();

            auto effective = effective_dependencies(
                source.manifest, selection.target_triple, features);
            if (!effective) return std::unexpected(effective.error());
            for (const auto& [alias, dependency] : *effective) {
                std::optional<MoidId> target_id;
                std::optional<Manifest> target_manifest;
                std::string source_key;

                if (dependency.is_path_dep) {
                    auto dependency_root = canonical_source_root(
                        source.manifest.project_dir / dependency.path.c_str());
                    if (!dependency_root)
                        return std::unexpected(dependency_root.error());
                    if (!dependency_root->is_directory()) {
                        return std::unexpected(
                            "path dependency '" + alias + "' at '" +
                            dependency.path +
                            "' does not exist or is not a directory");
                    }

                    if (!(*dependency_root / "bake.toml").is_regular_file()) {
                        // Non-moid path dep — record source dir for build.cpp.
                        source_node.source_deps[alias] =
                            dependency_root->string();
                        continue;
                    }

                    auto manifest = Manifest::load(*dependency_root);
                    if (!manifest || !manifest->has_moid()) {
                        return std::unexpected(
                            "dependency '" + alias + "' at '" +
                            dependency_root->string() +
                            "' does not declare a [package]");
                    }
                    manifest->project_dir = *dependency_root;
                    target_manifest = std::move(*manifest);

                    const std::string path_key =
                        canonical_path_string(*dependency_root);
                    auto workspace = workspace_ids.find(path_key);
                    if (workspace != workspace_ids.end()) {
                        target_id = workspace->second;
                        source_key = "workspace:" + path_key;
                    } else {
                        target_id = MoidId{"path:" + SHA256::hex(path_key)};
                        source_key = path_key;
                    }
                } else {
                    // Remote dep: pre-resolved by the caller into SourceIndex.
                    const LockedSource* locked = sources.find(dependency);
                    if (!locked) {
                        return std::unexpected(
                            "remote dependency '" + alias +
                            "' is not resolved (no lock entry matching "
                            "URL/ref) — run 'bake update'");
                    }
                    if (locked->integrity.empty() ||
                        (locked->ref_type != "archive" &&
                         locked->commit.empty())) {
                        return std::unexpected(
                            "locked dependency '" + alias +
                            "' is missing canonical hashes");
                    }
                    if (!locked->cache_dir.is_directory()) {
                        return std::unexpected(
                            std::string(policy.offline ? "offline cache"
                                                       : "cache") +
                            " for dependency '" + alias +
                            "' is missing at " + locked->cache_dir.string());
                    }

                    auto manifest = Manifest::load_moid(locked->cache_dir);
                    if (!manifest) {
                        // Non-native remote dep — source-only, no graph node.
                        // Record its source dir for build.cpp access.
                        source_node.source_deps[alias] =
                            locked->cache_dir.string();
                        continue;
                    }
                    manifest->project_dir = locked->cache_dir;
                    target_manifest = std::move(*manifest);

                    source_key = locked->identity();
                    target_id = MoidId{source_key};
                }

                auto added = register_source(
                    *target_id, source_key, std::move(*target_manifest));
                if (!added) return std::unexpected(added.error());

                // Edges were cleared above and `effective` holds one entry
                // per alias — a plain append rebuilds the outgoing set.
                source_node.dependencies.push_back(
                    MoidEdge{alias, *target_id, dependency.features,
                             dependency.default_features});
            }
        }
        return {};
    };

    auto seeded = unify();
    if (!seeded) return std::unexpected(seeded.error());
    std::size_t remaining_iterations = graph.nodes.size() + 8;
    for (;;) {
        if (remaining_iterations-- == 0) {
            return std::unexpected(
                "feature activation did not converge");
        }
        auto expanded = expand();
        if (!expanded) return std::unexpected(expanded.error());
        auto result = unify();
        if (!result) return std::unexpected(result.error());
        if (!*result) break;
    }

    for (auto& [_, node] : graph.nodes) {
        node.declaration.dependencies.clear();
        for (const auto& edge : node.dependencies) {
            node.declaration.dependencies.push_back(
                MoidDependency{edge.alias, edge.target.value, edge.features});
        }
    }

    auto topology = configuration_topological_moids(graph);
    if (!topology) return std::unexpected(topology.error());

    return graph;
}

} // namespace bake
