export module bake.graph;

import std;
import bake.util;
import bake.project;
import bake.moid;

namespace bake {

export struct MoidId {
    std::string value;
    auto operator<=>(const MoidId&) const = default;
};

export struct MoidEdge {
    std::string alias;
    MoidId target;
    std::map<std::string, BuildOption> options;
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

export enum class ArtifactKind {
    Object,
    Module,
    StaticLibrary,
    SharedLibrary,
    Executable,
};

export struct ArtifactRef {
    ArtifactKind kind;
    Path path;
    std::string producer_action;

    auto operator<=>(const ArtifactRef& other) const {
        if (kind != other.kind) return kind <=> other.kind;
        const std::string normalized =
            path.fs().lexically_normal().generic_string();
        const std::string other_normalized =
            other.path.fs().lexically_normal().generic_string();
        if (normalized != other_normalized)
            return normalized <=> other_normalized;
        return producer_action <=> other.producer_action;
    }

    bool operator==(const ArtifactRef& other) const {
        return (*this <=> other) == 0;
    }
};

export struct CompileUsage {
    std::vector<Path> include_dirs;
    std::vector<std::string> defines;
    std::map<std::string, ArtifactRef> modules;
};

export struct LinkInterface {
    std::vector<ArtifactRef> objects;
    std::vector<ArtifactRef> libraries;
    std::vector<std::string> system_libraries;
    std::vector<std::string> frameworks;
};

export struct MoidExports {
    CompileUsage compile;
    LinkInterface link;
    std::optional<ArtifactRef> terminal;
};

export struct BuildSelection {
    std::optional<std::string> workspace_member;
    std::map<std::string, BuildOption> root_options;
};

export struct ResolvePolicy {
    bool locked = false;
    bool offline = false;
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
    declaration.std_version = manifest.moid->std_version;
    declaration.options = manifest.options;
    return declaration;
}

std::string alias_path_string(const std::vector<std::string>& path) {
    return join(path, " -> ");
}

struct OptionRequest {
    BuildOption value;
    std::string path;
};

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
                "workspace member '" + member + "' does not declare a [moid]");
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

export std::expected<MoidGraph, std::string>
resolve_moid_graph(const Manifest& root,
                   const BuildSelection& selection,
                   const ResolvePolicy& policy) {
    auto root_path = canonical_source_root(root.project_dir);
    if (!root_path) return std::unexpected(root_path.error());

    struct SourceRecord {
        std::string source_key;
        Manifest manifest;
    };

    MoidGraph graph;
    std::map<MoidId, SourceRecord> sources;
    std::vector<MoidId> worklist;
    std::map<std::string, MoidId> workspace_ids;
    std::map<std::string, std::string> workspace_member_spellings;
    std::map<std::string, Manifest> workspace_manifests;

    auto register_source = [&](const MoidId& id,
                               std::string source_key,
                               Manifest manifest)
            -> std::expected<bool, std::string> {
        auto existing = sources.find(id);
        if (existing != sources.end()) {
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
                "' does not declare a [moid]");
        }

        const Path source_root = manifest.project_dir;
        MoidNode node;
        node.id = id;
        node.declaration = manifest_declaration(manifest, id, source_root);
        graph.nodes.emplace(id, std::move(node));
        sources.emplace(
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
                    "workspace member '" + member + "' does not declare a [moid]");
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
        return std::unexpected("project does not declare a [moid] or [workspace]");
    }

    auto lockfile = Lockfile::load(*root_path / "bake.lock");
    const Path cache_dir = get_cache_dir();

    for (std::size_t index = 0; index < worklist.size(); ++index) {
        const MoidId source_id = worklist[index];
        const SourceRecord source = sources.at(source_id);
        auto& source_node = graph.nodes.at(source_id);

        for (const auto& [alias, dependency] : source.manifest.dependencies) {
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
                        dependency.path + "' does not exist or is not a directory");
                }

                if (!(*dependency_root / "bake.toml").is_regular_file()) {
                    // Non-moid path dep — record source dir for build.cpp.
                    source_node.source_deps[alias] = dependency_root->string();
                    continue;
                }

                auto manifest = Manifest::load(*dependency_root);
                if (!manifest || !manifest->has_moid()) {
                    return std::unexpected(
                        "dependency '" + alias + "' at '" +
                        dependency_root->string() + "' does not declare a [moid]");
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
                // Remote dep: flat lock search by url + ref
                if (!lockfile) {
                    return std::unexpected(
                        "remote dependency '" + alias +
                        "' is not resolved: lockfile is missing");
                }
                const LockDep* locked =
                    lockfile->find_remote(dependency.url, dependency.tag);
                if (!locked) {
                    return std::unexpected(
                        "locked dependency '" + alias +
                        "' is not resolved (no lock entry matching URL/tag)");
                }
                if (locked->commit.empty() || locked->integrity.empty()) {
                    return std::unexpected(
                        "locked dependency '" + alias +
                        "' is missing canonical hashes");
                }

                std::string hash = locked->cache_hash();
                const Path dependency_root = cache_dir / hash;
                if (!dependency_root.is_directory()) {
                    return std::unexpected(
                        std::string(policy.offline ? "offline cache" : "cache") +
                        " for dependency '" + alias + "' is missing at " +
                        dependency_root.string());
                }

                auto manifest = Manifest::load(dependency_root);
                if (!manifest || !manifest->has_moid()) {
                    // Non-native remote dep — source-only, no graph node.
                    // Record its source dir for build.cpp access.
                    source_node.source_deps[alias] = dependency_root.string();
                    continue;
                }
                manifest->project_dir = dependency_root;
                target_manifest = std::move(*manifest);

                source_key = "git:" + normalize_dependency_url(locked->url) +
                    "#" + locked->commit + ":" + hash;
                target_id = MoidId{source_key};
            }

            auto added = register_source(
                *target_id, source_key, std::move(*target_manifest));
            if (!added) return std::unexpected(added.error());

            source_node.dependencies.push_back(
                MoidEdge{alias, *target_id, dependency.options});
        }
    }

    for (auto& [_, node] : graph.nodes) {
        node.declaration.dependencies.clear();
        for (const auto& edge : node.dependencies) {
            node.declaration.dependencies.push_back(
                MoidDependency{edge.alias, edge.target.value, edge.options});
        }
    }

    auto topology = configuration_topological_moids(graph);
    if (!topology) return std::unexpected(topology.error());

    std::map<MoidId, std::vector<std::string>> paths;
    std::deque<MoidId> path_queue;
    for (const auto& id : graph.roots) {
        if (paths.contains(id)) continue;
        paths[id] = {graph.nodes.at(id).declaration.name};
        path_queue.push_back(id);
    }
    while (!path_queue.empty()) {
        const MoidId source_id = path_queue.front();
        path_queue.pop_front();
        for (const auto& edge : graph.nodes.at(source_id).dependencies) {
            if (paths.contains(edge.target)) continue;
            auto target_path = paths.at(source_id);
            target_path.push_back(edge.alias);
            paths.emplace(edge.target, std::move(target_path));
            path_queue.push_back(edge.target);
        }
    }

    std::map<MoidId, std::map<std::string, OptionRequest>> requests;
    auto request_option = [&](const MoidId& target_id,
                              const std::string& option_name,
                              const BuildOption& value,
                              std::string request_path,
                              bool dependency_option)
            -> std::expected<void, std::string> {
        const auto& target = sources.at(target_id).manifest;
        auto declared = target.options.find(option_name);
        const std::string package_name = target.moid->name;
        const std::string kind = dependency_option
            ? "dependency option" : "build option";

        if (declared == target.options.end()) {
            return std::unexpected(
                kind + " '" + option_name +
                "' is not declared by package '" + package_name + "'");
        }
        if (declared->second.type != value.type) {
            return std::unexpected(
                kind + " '" + option_name + "' for package '" +
                package_name + "' expects " +
                std::string(build_option_type_name(declared->second.type)) +
                ", got " +
                std::string(build_option_type_name(value.type)));
        }

        auto& package_requests = requests[target_id];
        auto existing = package_requests.find(option_name);
        if (existing != package_requests.end() &&
            existing->second.value != value) {
            return std::unexpected(
                "option conflict for package '" + package_name +
                "': option '" + option_name + "' requested as " +
                format_build_option(existing->second.value) + " by " +
                existing->second.path + " and " +
                format_build_option(value) + " by " + request_path);
        }
        if (existing == package_requests.end()) {
            package_requests.emplace(
                option_name, OptionRequest{value, std::move(request_path)});
        }
        return {};
    };

    for (const auto& root_id : graph.roots) {
        const std::string root_path_label =
            alias_path_string(paths.at(root_id));
        for (const auto& [name, option] : selection.root_options) {
            auto requested = request_option(
                root_id, name, option, root_path_label, false);
            if (!requested) return std::unexpected(requested.error());
        }
    }

    for (const auto& [source_id, node] : graph.nodes) {
        const auto source_path = paths.contains(source_id)
            ? paths.at(source_id)
            : std::vector<std::string>{node.declaration.name};
        for (const auto& edge : node.dependencies) {
            auto request_path = source_path;
            request_path.push_back(edge.alias);
            const std::string path_label = alias_path_string(request_path);
            for (const auto& [name, option] : edge.options) {
                auto requested = request_option(
                    edge.target, name, option, path_label, true);
                if (!requested) return std::unexpected(requested.error());
            }
        }
    }

    for (auto& [id, node] : graph.nodes) {
        auto effective = sources.at(id).manifest.options;
        auto configured = requests.find(id);
        if (configured != requests.end()) {
            for (const auto& [name, request] : configured->second)
                effective[name] = request.value;
        }
        node.declaration.options = std::move(effective);
    }

    return graph;
}

} // namespace bake
