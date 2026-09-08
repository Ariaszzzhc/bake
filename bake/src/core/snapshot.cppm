export module bake.core.snapshot;

import std;
import bake.json;
import bake.util;
import bake.toolchain.target;
import bake.buildsystem.project;
import bake.buildsystem.moid;
import bake.buildsystem.graph;
import bake.buildsystem.plan;
import bake.core.model;

namespace bake {

export enum class SnapshotErrorCode {
    malformed,
    unsupported_schema,
    context_mismatch,
    identity_mismatch,
    unknown_owner,
    invalid_dependency,
};

export struct SnapshotError {
    SnapshotErrorCode code;
    std::string field;
    std::string message;
};

export struct ObservedDeclarationInput {
    Path canonical_path;
    bool exists = false;
    std::int64_t mtime = 0;

    bool operator==(const ObservedDeclarationInput&) const = default;
};

export struct InputDeclarationRecord {
    ProjectContextKey context;
    std::vector<ObservedDeclarationInput> inputs;

    bool operator==(const InputDeclarationRecord&) const = default;
};

export struct ManifestRecord {
    Path path;
    std::optional<std::string> moid_id;
    std::vector<std::string> workspace_members;
    std::optional<SourceExtConfig> source_extensions;
};

export struct SourceUnitRecord {
    CompilationUnitId id;
    std::string owner_moid;
    InputIdentity source;
    std::vector<InputIdentity> headers;
    std::optional<std::string> provides_module;
    std::vector<std::string> imports;
};

export struct SourceGraphSnapshot {
    std::vector<SourceUnitRecord> units;
};

export struct ProjectSnapshot {
    ProjectContext context;
    InputDeclarationRecord declaration_inputs;
    SourceIndex source_index;
    std::vector<ManifestRecord> manifests;
    MoidGraph moid_graph;
    SourceGraphSnapshot source_graph;
    BuildGraph build_graph;
    std::vector<CompilationDescription> compilations;
};

namespace detail {

using Json = json::Value;

SnapshotError error(SnapshotErrorCode code, std::string field,
                    std::string message) {
    return SnapshotError{code, std::move(field), std::move(message)};
}

bool exact_fields(const Json& object,
                  std::initializer_list<std::string_view> fields) {
    if (!object.is_object() || object.size() != fields.size()) return false;
    for (auto field : fields)
        if (!object.contains(field)) return false;
    return true;
}

std::expected<const Json*, SnapshotError> field(
        const Json& object, std::string_view name, std::string path) {
    const Json* value = object.find(name);
    if (!value)
        return std::unexpected(error(
            SnapshotErrorCode::malformed, std::move(path),
            "missing required field '" + std::string(name) + "'"));
    return value;
}

std::expected<std::string, SnapshotError> string_field(
        const Json& object, std::string_view name, std::string path,
        bool allow_empty = false) {
    auto value = field(object, name, path);
    if (!value) return std::unexpected(value.error());
    if (!(*value)->is_string())
        return std::unexpected(error(
            SnapshotErrorCode::malformed, path + "." + std::string(name),
            "expected a string"));
    std::string result = (*value)->get<std::string>();
    if (!allow_empty && result.empty())
        return std::unexpected(error(
            SnapshotErrorCode::malformed, path + "." + std::string(name),
            "must not be empty"));
    return result;
}

std::expected<bool, SnapshotError> bool_field(
        const Json& object, std::string_view name, std::string path) {
    auto value = field(object, name, path);
    if (!value) return std::unexpected(value.error());
    if (!(*value)->is_boolean())
        return std::unexpected(error(
            SnapshotErrorCode::malformed, path + "." + std::string(name),
            "expected a boolean"));
    return (*value)->get<bool>();
}

std::expected<std::int64_t, SnapshotError> integer_field(
        const Json& object, std::string_view name, std::string path) {
    auto value = field(object, name, path);
    if (!value) return std::unexpected(value.error());
    if (!(*value)->is_integer())
        return std::unexpected(error(
            SnapshotErrorCode::malformed, path + "." + std::string(name),
            "expected an integer"));
    return (*value)->get<std::int64_t>();
}

std::expected<std::vector<std::string>, SnapshotError> string_array(
        const Json& value, std::string path) {
    if (!value.is_array())
        return std::unexpected(error(
            SnapshotErrorCode::malformed, std::move(path),
            "expected an array"));
    std::vector<std::string> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (!value[index].is_string())
            return std::unexpected(error(
                SnapshotErrorCode::malformed,
                path + "[" + std::to_string(index) + "]",
                "expected a string"));
        result.push_back(value[index].get<std::string>());
    }
    return result;
}

std::expected<Path, SnapshotError> canonical_record_path(
        const Path& path, std::string field_path) {
    std::error_code filesystem_error;
    auto absolute = std::filesystem::absolute(path.fs(), filesystem_error);
    if (filesystem_error)
        return std::unexpected(error(
            SnapshotErrorCode::malformed, std::move(field_path),
            "failed to make path absolute"));
    auto canonical = std::filesystem::weakly_canonical(
        absolute, filesystem_error);
    if (filesystem_error)
        return std::unexpected(error(
            SnapshotErrorCode::malformed, std::move(field_path),
            "failed to canonicalize path"));
    return Path(canonical);
}

Json context_to_json(const ProjectContext& context) {
    Json document = Json::object();
    document["workspace_root"] = context.workspace_root.string();
    document["member_root"] = context.member_root
        ? Json(context.member_root->string()) : Json(nullptr);
    document["features"] = context.features;
    document["profile"] = context.profile;
    Json target = Json::object();
    target["triple"] = context.target.triple_;
    target["native"] = context.target.native_;
    target["glibc_major"] = context.target.glibc_major_;
    target["glibc_minor"] = context.target.glibc_minor_;
    target["macos_min_major"] = context.target.macos_min_major_;
    target["macos_min_minor"] = context.target.macos_min_minor_;
    document["target"] = std::move(target);
    Json compiler = Json::object();
    compiler["executable"] = context.compiler.executable.string();
    compiler["version"] = context.compiler.version;
    compiler["content_digest"] = context.compiler.content_digest;
    document["compiler"] = std::move(compiler);
    return document;
}

std::expected<ProjectContext, SnapshotError> context_from_json(
        const Json& document) {
    if (!exact_fields(document, {"workspace_root", "member_root", "features",
                                 "profile", "target", "compiler"}))
        return std::unexpected(error(
            SnapshotErrorCode::malformed, "context",
            "context has missing or unknown fields"));
    auto workspace = string_field(document, "workspace_root", "context");
    auto profile = string_field(document, "profile", "context");
    auto features_value = field(document, "features", "context");
    auto member_value = field(document, "member_root", "context");
    auto target_value = field(document, "target", "context");
    auto compiler_value = field(document, "compiler", "context");
    if (!workspace) return std::unexpected(workspace.error());
    if (!profile) return std::unexpected(profile.error());
    if (!features_value) return std::unexpected(features_value.error());
    if (!member_value) return std::unexpected(member_value.error());
    if (!target_value) return std::unexpected(target_value.error());
    if (!compiler_value) return std::unexpected(compiler_value.error());
    auto features = string_array(**features_value, "context.features");
    if (!features) return std::unexpected(features.error());
    std::optional<Path> member;
    if (!(*member_value)->is_null()) {
        if (!(*member_value)->is_string())
            return std::unexpected(error(
                SnapshotErrorCode::malformed, "context.member_root",
                "expected a string or null"));
        member = Path((*member_value)->get<std::string>());
    }
    if (!exact_fields(**target_value,
                      {"triple", "native", "glibc_major", "glibc_minor",
                       "macos_min_major", "macos_min_minor"}))
        return std::unexpected(error(
            SnapshotErrorCode::malformed, "context.target",
            "target has missing or unknown fields"));
    auto triple = string_field(**target_value, "triple", "context.target");
    auto native = bool_field(**target_value, "native", "context.target");
    auto glibc_major = integer_field(**target_value, "glibc_major", "context.target");
    auto glibc_minor = integer_field(**target_value, "glibc_minor", "context.target");
    auto macos_major = integer_field(**target_value, "macos_min_major", "context.target");
    auto macos_minor = integer_field(**target_value, "macos_min_minor", "context.target");
    if (!triple) return std::unexpected(triple.error());
    if (!native) return std::unexpected(native.error());
    if (!glibc_major) return std::unexpected(glibc_major.error());
    if (!glibc_minor) return std::unexpected(glibc_minor.error());
    if (!macos_major) return std::unexpected(macos_major.error());
    if (!macos_minor) return std::unexpected(macos_minor.error());
    TargetSpec target;
    target.triple_ = std::move(*triple);
    target.native_ = *native;
    target.glibc_major_ = static_cast<int>(*glibc_major);
    target.glibc_minor_ = static_cast<int>(*glibc_minor);
    target.macos_min_major_ = static_cast<int>(*macos_major);
    target.macos_min_minor_ = static_cast<int>(*macos_minor);
    if (!exact_fields(**compiler_value,
                      {"executable", "version", "content_digest"}))
        return std::unexpected(error(
            SnapshotErrorCode::malformed, "context.compiler",
            "compiler has missing or unknown fields"));
    auto executable = string_field(**compiler_value, "executable", "context.compiler");
    auto version = string_field(**compiler_value, "version", "context.compiler");
    auto digest = string_field(**compiler_value, "content_digest", "context.compiler");
    if (!executable) return std::unexpected(executable.error());
    if (!version) return std::unexpected(version.error());
    if (!digest) return std::unexpected(digest.error());
    ProjectContext context{
        Path(*workspace), std::move(member), std::move(*features),
        std::move(*profile), std::move(target),
        CompilerIdentity{Path(*executable), std::move(*version),
                         std::move(*digest)},
    };
    auto key = context_key(context);
    if (!key)
        return std::unexpected(error(
            SnapshotErrorCode::identity_mismatch, "context",
            key.error().message));
    return context;
}

Json input_identity_to_json(const InputIdentity& identity) {
    Json value = Json::object();
    value["path"] = identity.path.string();
    value["digest"] = identity.digest;
    value["size"] = static_cast<std::int64_t>(identity.size);
    return value;
}

std::expected<InputIdentity, SnapshotError> input_identity_from_json(
        const Json& value, std::string path) {
    if (!exact_fields(value, {"path", "digest", "size"}))
        return std::unexpected(error(
            SnapshotErrorCode::malformed, path,
            "input identity has missing or unknown fields"));
    auto file = string_field(value, "path", path);
    auto digest = string_field(value, "digest", path);
    auto size = integer_field(value, "size", path);
    if (!file) return std::unexpected(file.error());
    if (!digest) return std::unexpected(digest.error());
    if (!size) return std::unexpected(size.error());
    if (*size < 0 || digest->size() != 64 ||
        !std::ranges::all_of(*digest, [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        return std::unexpected(error(
            SnapshotErrorCode::malformed, path,
            "input identity has an invalid size or digest"));
    Path input_path(*file);
    auto canonical = canonical_record_path(input_path, path + ".path");
    if (!canonical || canonical->string() != input_path.string())
        return std::unexpected(error(
            SnapshotErrorCode::identity_mismatch, path + ".path",
            "input identity path is not canonical"));
    return InputIdentity{std::move(*canonical), std::move(*digest),
                         static_cast<std::uintmax_t>(*size)};
}

Json source_index_to_json(const SourceIndex& index) {
    Json entries = Json::array();
    for (const auto& source : index.entries) {
        Json entry = Json::object();
        entry["url"] = source.url;
        entry["ref"] = source.ref;
        entry["ref_type"] = source.ref_type;
        entry["commit"] = source.commit;
        entry["integrity"] = source.integrity;
        entry["cache_hash"] = source.cache_hash;
        entry["cache_dir"] = source.cache_dir.string();
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::expected<SourceIndex, SnapshotError> source_index_from_json(
        const Json& value) {
    if (!value.is_array())
        return std::unexpected(error(
            SnapshotErrorCode::malformed, "source_index",
            "source index must be an array"));
    SourceIndex index;
    std::set<std::string> identities;
    for (std::size_t item = 0; item < value.size(); ++item) {
        const auto& entry = value[item];
        const std::string path = "source_index[" + std::to_string(item) + "]";
        if (!exact_fields(entry, {"url", "ref", "ref_type", "commit",
                                  "integrity", "cache_hash", "cache_dir"}))
            return std::unexpected(error(
                SnapshotErrorCode::malformed, path,
                "locked source has missing or unknown fields"));
        auto url = string_field(entry, "url", path);
        auto ref = string_field(entry, "ref", path, true);
        auto ref_type = string_field(entry, "ref_type", path);
        auto commit = string_field(entry, "commit", path, true);
        auto integrity = string_field(entry, "integrity", path);
        auto cache_hash = string_field(entry, "cache_hash", path);
        auto cache_dir = string_field(entry, "cache_dir", path);
        if (!url) return std::unexpected(url.error());
        if (!ref) return std::unexpected(ref.error());
        if (!ref_type) return std::unexpected(ref_type.error());
        if (!commit) return std::unexpected(commit.error());
        if (!integrity) return std::unexpected(integrity.error());
        if (!cache_hash) return std::unexpected(cache_hash.error());
        if (!cache_dir) return std::unexpected(cache_dir.error());
        if (*ref_type != "archive" && *ref_type != "tag" &&
            *ref_type != "branch" && *ref_type != "rev" &&
            *ref_type != "head")
            return std::unexpected(error(
                SnapshotErrorCode::malformed, path + ".ref_type",
                "unknown locked source reference type"));
        LockedSource source{
            std::move(*url), std::move(*ref), std::move(*ref_type),
            std::move(*commit), std::move(*integrity),
            std::move(*cache_hash), Path(*cache_dir),
        };
        if (!identities.insert(source.identity()).second)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, path,
                "duplicate locked source identity"));
        index.entries.push_back(std::move(source));
    }
    return index;
}

Json moid_graph_to_json(const MoidGraph& graph) {
    Json document = Json::object();
    document["nodes"] = Json::array();
    for (const auto& [id, node] : graph.nodes) {
        Json entry = Json::object();
        entry["id"] = id.value;
        entry["declaration"] = moid_declaration_to_json(node.declaration);
        entry["dependencies"] = Json::array();
        for (const auto& dependency : node.dependencies) {
            Json edge = Json::object();
            edge["alias"] = dependency.alias;
            edge["target"] = dependency.target.value;
            edge["features"] = dependency.features;
            edge["default_features"] = dependency.default_features;
            entry["dependencies"].push_back(std::move(edge));
        }
        entry["source_deps"] = Json::array();
        for (const auto& [alias, source] : node.source_deps) {
            Json dep = Json::object();
            dep["alias"] = alias;
            dep["path"] = source;
            entry["source_deps"].push_back(std::move(dep));
        }
        document["nodes"].push_back(std::move(entry));
    }
    document["roots"] = Json::array();
    for (const auto& root : graph.roots)
        document["roots"].push_back(root.value);
    return document;
}

std::expected<MoidGraph, SnapshotError> moid_graph_from_json(
        const Json& document) {
    if (!exact_fields(document, {"nodes", "roots"}))
        return std::unexpected(error(
            SnapshotErrorCode::malformed, "moid_graph",
            "moid graph has missing or unknown fields"));
    const auto* nodes = document.find("nodes");
    const auto* roots = document.find("roots");
    if (!nodes || !nodes->is_array() || nodes->size() == 0)
        return std::unexpected(error(
            SnapshotErrorCode::malformed, "moid_graph.nodes",
            "moid graph requires at least one node"));
    if (!roots || !roots->is_array() || roots->size() == 0)
        return std::unexpected(error(
            SnapshotErrorCode::malformed, "moid_graph.roots",
            "moid graph requires at least one root"));
    MoidGraph graph;
    for (std::size_t item = 0; item < nodes->size(); ++item) {
        const auto& entry = (*nodes)[item];
        const std::string path = "moid_graph.nodes[" + std::to_string(item) + "]";
        if (!exact_fields(entry,
                          {"id", "declaration", "dependencies", "source_deps"}))
            return std::unexpected(error(
                SnapshotErrorCode::malformed, path,
                "moid node has missing or unknown fields"));
        auto id = string_field(entry, "id", path);
        auto declaration_value = field(entry, "declaration", path);
        auto dependencies = field(entry, "dependencies", path);
        auto source_deps = field(entry, "source_deps", path);
        if (!id) return std::unexpected(id.error());
        if (!declaration_value) return std::unexpected(declaration_value.error());
        if (!dependencies) return std::unexpected(dependencies.error());
        if (!source_deps) return std::unexpected(source_deps.error());
        auto declaration = moid_declaration_from_json(**declaration_value);
        if (!declaration)
            return std::unexpected(error(
                SnapshotErrorCode::malformed, path + ".declaration",
                declaration.error()));
        if (declaration->id != *id)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, path + ".declaration.id",
                "node and declaration identities differ"));
        if (!(*dependencies)->is_array() || !(*source_deps)->is_array())
            return std::unexpected(error(
                SnapshotErrorCode::malformed, path,
                "node dependencies must be arrays"));
        MoidNode node;
        node.id = MoidId{*id};
        node.declaration = std::move(*declaration);
        std::set<std::string> aliases;
        for (std::size_t edge_index = 0;
             edge_index < (*dependencies)->size(); ++edge_index) {
            const auto& edge = (**dependencies)[edge_index];
            const std::string edge_path = path + ".dependencies[" +
                                          std::to_string(edge_index) + "]";
            if (!exact_fields(edge, {"alias", "target", "features",
                                     "default_features"}))
                return std::unexpected(error(
                    SnapshotErrorCode::malformed, edge_path,
                    "moid edge has missing or unknown fields"));
            auto alias = string_field(edge, "alias", edge_path);
            auto target = string_field(edge, "target", edge_path);
            auto features_value = field(edge, "features", edge_path);
            auto defaults = bool_field(edge, "default_features", edge_path);
            if (!alias) return std::unexpected(alias.error());
            if (!target) return std::unexpected(target.error());
            if (!features_value) return std::unexpected(features_value.error());
            if (!defaults) return std::unexpected(defaults.error());
            auto features = string_array(**features_value, edge_path + ".features");
            if (!features) return std::unexpected(features.error());
            if (!aliases.insert(*alias).second)
                return std::unexpected(error(
                    SnapshotErrorCode::identity_mismatch, edge_path + ".alias",
                    "duplicate dependency alias"));
            node.dependencies.push_back(MoidEdge{
                std::move(*alias), MoidId{std::move(*target)},
                std::move(*features), *defaults,
            });
        }
        for (std::size_t dep_index = 0;
             dep_index < (*source_deps)->size(); ++dep_index) {
            const auto& source_dep = (**source_deps)[dep_index];
            const std::string dep_path = path + ".source_deps[" +
                                         std::to_string(dep_index) + "]";
            if (!exact_fields(source_dep, {"alias", "path"}))
                return std::unexpected(error(
                    SnapshotErrorCode::malformed, dep_path,
                    "source dependency has missing or unknown fields"));
            auto alias = string_field(source_dep, "alias", dep_path);
            auto source = string_field(source_dep, "path", dep_path);
            if (!alias) return std::unexpected(alias.error());
            if (!source) return std::unexpected(source.error());
            if (!node.source_deps.emplace(*alias, *source).second)
                return std::unexpected(error(
                    SnapshotErrorCode::identity_mismatch, dep_path + ".alias",
                    "duplicate source dependency alias"));
        }
        if (!graph.nodes.emplace(node.id, std::move(node)).second)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, path + ".id",
                "duplicate moid identity"));
    }
    std::set<MoidId> root_ids;
    for (std::size_t index = 0; index < roots->size(); ++index) {
        if (!(*roots)[index].is_string())
            return std::unexpected(error(
                SnapshotErrorCode::malformed,
                "moid_graph.roots[" + std::to_string(index) + "]",
                "expected a string"));
        MoidId id{(*roots)[index].get<std::string>()};
        if (!graph.nodes.contains(id))
            return std::unexpected(error(
                SnapshotErrorCode::invalid_dependency,
                "moid_graph.roots[" + std::to_string(index) + "]",
                "root references an unknown moid"));
        if (!root_ids.insert(id).second)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch,
                "moid_graph.roots[" + std::to_string(index) + "]",
                "duplicate graph root"));
        graph.roots.push_back(std::move(id));
    }
    for (const auto& [id, node] : graph.nodes) {
        for (const auto& dependency : node.dependencies) {
            if (!graph.nodes.contains(dependency.target))
                return std::unexpected(error(
                    SnapshotErrorCode::invalid_dependency,
                    "moid_graph.nodes." + id.value + ".dependencies",
                    "edge references an unknown moid"));
            auto declared = std::ranges::find_if(
                node.declaration.dependencies, [&](const MoidDependency& value) {
                    return value.alias == dependency.alias &&
                           value.id == dependency.target.value &&
                           value.features == dependency.features;
                });
            if (declared == node.declaration.dependencies.end())
                return std::unexpected(error(
                    SnapshotErrorCode::invalid_dependency,
                    "moid_graph.nodes." + id.value + ".dependencies",
                    "edge is inconsistent with the unified declaration"));
        }
    }
    return graph;
}

Json declaration_inputs_to_json(const InputDeclarationRecord& record) {
    Json document = Json::object();
    document["context"] = record.context.value;
    document["inputs"] = Json::array();
    for (const auto& input : record.inputs) {
        Json entry = Json::object();
        entry["path"] = input.canonical_path.string();
        entry["exists"] = input.exists;
        entry["mtime"] = input.mtime;
        document["inputs"].push_back(std::move(entry));
    }
    return document;
}

std::expected<InputDeclarationRecord, SnapshotError>
declaration_inputs_from_json(const Json& document) {
    if (!exact_fields(document, {"context", "inputs"}))
        return std::unexpected(error(
            SnapshotErrorCode::malformed, "declaration_inputs",
            "declaration input record has missing or unknown fields"));
    auto context = string_field(document, "context", "declaration_inputs");
    auto inputs = field(document, "inputs", "declaration_inputs");
    if (!context) return std::unexpected(context.error());
    if (!inputs) return std::unexpected(inputs.error());
    if (!(*inputs)->is_array() || (*inputs)->size() == 0)
        return std::unexpected(error(
            SnapshotErrorCode::malformed, "declaration_inputs.inputs",
            "declaration input record requires inputs"));
    InputDeclarationRecord record{ProjectContextKey{*context}, {}};
    std::set<std::string> paths;
    for (std::size_t index = 0; index < (*inputs)->size(); ++index) {
        const auto& entry = (**inputs)[index];
        const std::string path = "declaration_inputs.inputs[" +
                                 std::to_string(index) + "]";
        if (!exact_fields(entry, {"path", "exists", "mtime"}))
            return std::unexpected(error(
                SnapshotErrorCode::malformed, path,
                "declaration input has missing or unknown fields"));
        auto file = string_field(entry, "path", path);
        auto exists = bool_field(entry, "exists", path);
        auto mtime = integer_field(entry, "mtime", path);
        if (!file) return std::unexpected(file.error());
        if (!exists) return std::unexpected(exists.error());
        if (!mtime) return std::unexpected(mtime.error());
        Path input_path(*file);
        auto canonical = canonical_record_path(input_path, path + ".path");
        if (!canonical || canonical->string() != input_path.string())
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, path + ".path",
                "declaration input path is not canonical"));
        if (!paths.insert(file.value()).second)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, path + ".path",
                "duplicate declaration input path"));
        if (!*exists && *mtime != 0)
            return std::unexpected(error(
                SnapshotErrorCode::malformed, path + ".mtime",
                "missing declaration input must have mtime zero"));
        record.inputs.push_back(ObservedDeclarationInput{
            std::move(*canonical), *exists, *mtime,
        });
    }
    return record;
}

Json source_graph_to_json(const SourceGraphSnapshot& graph) {
    Json units = Json::array();
    for (const auto& unit : graph.units) {
        Json entry = Json::object();
        entry["id"] = unit.id.value;
        entry["owner_moid"] = unit.owner_moid;
        entry["source"] = input_identity_to_json(unit.source);
        entry["headers"] = Json::array();
        for (const auto& header : unit.headers)
            entry["headers"].push_back(input_identity_to_json(header));
        entry["provides_module"] = unit.provides_module
            ? Json(*unit.provides_module) : Json(nullptr);
        entry["imports"] = unit.imports;
        units.push_back(std::move(entry));
    }
    return units;
}

std::expected<SourceGraphSnapshot, SnapshotError> source_graph_from_json(
        const Json& value) {
    if (!value.is_array())
        return std::unexpected(error(
            SnapshotErrorCode::malformed, "source_graph",
            "source graph must be an array"));
    SourceGraphSnapshot graph;
    std::set<std::string> ids;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto& entry = value[index];
        const std::string path = "source_graph[" + std::to_string(index) + "]";
        if (!exact_fields(entry, {"id", "owner_moid", "source", "headers",
                                  "provides_module", "imports"}))
            return std::unexpected(error(
                SnapshotErrorCode::malformed, path,
                "source unit has missing or unknown fields"));
        auto id = string_field(entry, "id", path);
        auto owner = string_field(entry, "owner_moid", path);
        auto source_value = field(entry, "source", path);
        auto headers = field(entry, "headers", path);
        auto provides = field(entry, "provides_module", path);
        auto imports_value = field(entry, "imports", path);
        if (!id) return std::unexpected(id.error());
        if (!owner) return std::unexpected(owner.error());
        if (!source_value) return std::unexpected(source_value.error());
        if (!headers) return std::unexpected(headers.error());
        if (!provides) return std::unexpected(provides.error());
        if (!imports_value) return std::unexpected(imports_value.error());
        if (!ids.insert(*id).second)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, path + ".id",
                "duplicate compilation unit identity"));
        auto source = input_identity_from_json(**source_value, path + ".source");
        if (!source) return std::unexpected(source.error());
        if (!(*headers)->is_array())
            return std::unexpected(error(
                SnapshotErrorCode::malformed, path + ".headers",
                "headers must be an array"));
        std::vector<InputIdentity> header_values;
        for (std::size_t header_index = 0;
             header_index < (*headers)->size(); ++header_index) {
            auto header = input_identity_from_json(
                (**headers)[header_index], path + ".headers[" +
                                              std::to_string(header_index) + "]");
            if (!header) return std::unexpected(header.error());
            header_values.push_back(std::move(*header));
        }
        std::optional<std::string> provided;
        if (!(*provides)->is_null()) {
            if (!(*provides)->is_string() ||
                (*provides)->get<std::string>().empty())
                return std::unexpected(error(
                    SnapshotErrorCode::malformed, path + ".provides_module",
                    "expected a nonempty string or null"));
            provided = (*provides)->get<std::string>();
        }
        auto imports = string_array(**imports_value, path + ".imports");
        if (!imports) return std::unexpected(imports.error());
        graph.units.push_back(SourceUnitRecord{
            CompilationUnitId{std::move(*id)}, std::move(*owner),
            std::move(*source), std::move(header_values), std::move(provided),
            std::move(*imports),
        });
    }
    return graph;
}

std::string language_name(SourceLanguage language) {
    switch (language) {
        case SourceLanguage::c: return "c";
        case SourceLanguage::cxx: return "cxx";
        case SourceLanguage::module_interface: return "module_interface";
    }
    std::unreachable();
}

std::expected<SourceLanguage, SnapshotError> parse_language(
        std::string_view value, std::string path) {
    if (value == "c") return SourceLanguage::c;
    if (value == "cxx") return SourceLanguage::cxx;
    if (value == "module_interface") return SourceLanguage::module_interface;
    return std::unexpected(error(
        SnapshotErrorCode::malformed, std::move(path),
        "unknown source language"));
}

Json compilations_to_json(
        const std::vector<CompilationDescription>& compilations) {
    Json values = Json::array();
    for (const auto& compilation : compilations) {
        Json entry = Json::object();
        entry["id"] = compilation.id.value;
        entry["context"] = compilation.context.value;
        entry["owner_moid"] = compilation.owner_moid;
        entry["language"] = language_name(compilation.language);
        entry["working_directory"] = compilation.working_directory.string();
        entry["source"] = compilation.source.string();
        entry["planned_output"] = compilation.planned_output.string();
        entry["driver_arguments"] = compilation.driver_arguments;
        entry["module_references"] = Json::array();
        for (const auto& reference : compilation.module_references) {
            Json module = Json::object();
            module["name"] = reference.name;
            module["bmi"] = reference.bmi.string();
            entry["module_references"].push_back(std::move(module));
        }
        values.push_back(std::move(entry));
    }
    return values;
}

std::expected<std::vector<CompilationDescription>, SnapshotError>
compilations_from_json(const Json& value) {
    if (!value.is_array())
        return std::unexpected(error(
            SnapshotErrorCode::malformed, "compilations",
            "compilations must be an array"));
    std::vector<CompilationDescription> result;
    std::set<std::string> ids;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto& entry = value[index];
        const std::string path = "compilations[" + std::to_string(index) + "]";
        if (!exact_fields(entry,
                          {"id", "context", "owner_moid", "language",
                           "working_directory", "source", "planned_output",
                           "driver_arguments", "module_references"}))
            return std::unexpected(error(
                SnapshotErrorCode::malformed, path,
                "compilation has missing or unknown fields"));
        auto id = string_field(entry, "id", path);
        auto context = string_field(entry, "context", path);
        auto owner = string_field(entry, "owner_moid", path);
        auto language_text = string_field(entry, "language", path);
        auto working_directory = string_field(entry, "working_directory", path);
        auto source = string_field(entry, "source", path);
        auto output = string_field(entry, "planned_output", path);
        auto arguments_value = field(entry, "driver_arguments", path);
        auto modules = field(entry, "module_references", path);
        if (!id) return std::unexpected(id.error());
        if (!context) return std::unexpected(context.error());
        if (!owner) return std::unexpected(owner.error());
        if (!language_text) return std::unexpected(language_text.error());
        if (!working_directory) return std::unexpected(working_directory.error());
        if (!source) return std::unexpected(source.error());
        if (!output) return std::unexpected(output.error());
        if (!arguments_value) return std::unexpected(arguments_value.error());
        if (!modules) return std::unexpected(modules.error());
        if (!ids.insert(*id).second)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, path + ".id",
                "duplicate compilation identity"));
        auto language = parse_language(*language_text, path + ".language");
        auto arguments = string_array(**arguments_value,
                                      path + ".driver_arguments");
        if (!language) return std::unexpected(language.error());
        if (!arguments) return std::unexpected(arguments.error());
        if (arguments->empty() ||
            (arguments->front() != "cc" && arguments->front() != "c++"))
            return std::unexpected(error(
                SnapshotErrorCode::malformed, path + ".driver_arguments",
                "driver arguments must start with cc or c++ and omit argv[0]"));
        if (!(*modules)->is_array())
            return std::unexpected(error(
                SnapshotErrorCode::malformed, path + ".module_references",
                "module references must be an array"));
        std::vector<ModuleReference> references;
        std::set<std::string> module_names;
        for (std::size_t module_index = 0;
             module_index < (*modules)->size(); ++module_index) {
            const auto& module = (**modules)[module_index];
            const std::string module_path = path + ".module_references[" +
                                            std::to_string(module_index) + "]";
            if (!exact_fields(module, {"name", "bmi"}))
                return std::unexpected(error(
                    SnapshotErrorCode::malformed, module_path,
                    "module reference has missing or unknown fields"));
            auto name = string_field(module, "name", module_path);
            auto bmi = string_field(module, "bmi", module_path);
            if (!name) return std::unexpected(name.error());
            if (!bmi) return std::unexpected(bmi.error());
            if (!module_names.insert(*name).second)
                return std::unexpected(error(
                    SnapshotErrorCode::identity_mismatch, module_path + ".name",
                    "duplicate module reference"));
            references.push_back(ModuleReference{std::move(*name), Path(*bmi)});
        }
        result.push_back(CompilationDescription{
            CompilationUnitId{std::move(*id)}, ProjectContextKey{std::move(*context)},
            std::move(*owner), *language, Path(*working_directory), Path(*source),
            Path(*output), std::move(*arguments), std::move(references),
        });
    }
    return result;
}

std::expected<void, SnapshotError> validate_snapshot(
        const ProjectSnapshot& snapshot,
        const ProjectContextKey& expected_context) {
    auto actual_context = context_key(snapshot.context);
    if (!actual_context)
        return std::unexpected(error(
            SnapshotErrorCode::identity_mismatch, "context",
            actual_context.error().message));
    if (*actual_context != expected_context)
        return std::unexpected(error(
            SnapshotErrorCode::context_mismatch, "context",
            "snapshot context does not match the requested context"));
    if (snapshot.declaration_inputs.context != expected_context)
        return std::unexpected(error(
            SnapshotErrorCode::context_mismatch, "declaration_inputs.context",
            "declaration record belongs to another context"));
    auto paths = context_paths(snapshot.context);
    if (!paths)
        return std::unexpected(error(
            SnapshotErrorCode::identity_mismatch, "context",
            paths.error().message));
    if (snapshot.build_graph.project_root != snapshot.context.workspace_root ||
        snapshot.build_graph.state_dir != paths->bake)
        return std::unexpected(error(
            SnapshotErrorCode::context_mismatch, "build_graph",
            "build graph paths do not belong to the snapshot context"));
    std::set<std::string> owners;
    for (const auto& [id, node] : snapshot.moid_graph.nodes) {
        if (id != node.id || node.id.value != node.declaration.id)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, "moid_graph.nodes",
                "moid node identity is inconsistent"));
        owners.insert(id.value);
        for (const auto& binary : node.declaration.binaries)
            owners.insert(binary_moid_id(id.value, binary.name));
    }
    if (owners.empty())
        return std::unexpected(error(
            SnapshotErrorCode::malformed, "moid_graph.nodes",
            "ready snapshot cannot contain an empty moid graph"));
    std::set<std::string> manifest_paths;
    if (snapshot.manifests.empty())
        return std::unexpected(error(
            SnapshotErrorCode::malformed, "manifests",
            "snapshot requires manifest facts"));
    for (const auto& manifest : snapshot.manifests) {
        auto canonical = canonical_record_path(manifest.path, "manifests.path");
        if (!canonical || canonical->string() != manifest.path.string())
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, "manifests.path",
                "manifest path is not canonical"));
        if (!manifest_paths.insert(manifest.path.string()).second)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, "manifests.path",
                "duplicate manifest path"));
        if (manifest.moid_id && !owners.contains(*manifest.moid_id))
            return std::unexpected(error(
                SnapshotErrorCode::unknown_owner, "manifests.moid_id",
                "manifest references an unknown moid"));
    }
    std::set<std::string> action_ids;
    for (std::size_t index = 0; index < snapshot.build_graph.actions.size();
         ++index) {
        const auto& action = snapshot.build_graph.actions[index];
        if (!owners.contains(action.moid_id))
            return std::unexpected(error(
                SnapshotErrorCode::unknown_owner,
                "build_graph.actions[" + std::to_string(index) + "].moid_id",
                "build action references an unknown moid"));
        if (!action_ids.insert(action.id).second)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch,
                "build_graph.actions[" + std::to_string(index) + "].id",
                "duplicate build action identity"));
        for (const auto& output : action.outputs) {
            auto relative = output.fs().lexically_normal().lexically_relative(
                paths->root.fs().lexically_normal());
            if (relative.empty() || *relative.begin() == "..")
                return std::unexpected(error(
                    SnapshotErrorCode::context_mismatch,
                    "build_graph.actions[" + std::to_string(index) +
                        "].outputs",
                    "build output is outside the context output root"));
        }
        for (std::size_t dependency : action.depends_on) {
            if (dependency >= index)
                return std::unexpected(error(
                    SnapshotErrorCode::invalid_dependency,
                    "build_graph.actions[" + std::to_string(index) +
                        "].depends_on",
                    "build action dependency is not topological"));
        }
    }
    std::map<std::string, const CompilationDescription*> compilations;
    for (const auto& compilation : snapshot.compilations) {
        if (compilation.context != expected_context)
            return std::unexpected(error(
                SnapshotErrorCode::context_mismatch, "compilations.context",
                "compilation belongs to another context"));
        if (!owners.contains(compilation.owner_moid))
            return std::unexpected(error(
                SnapshotErrorCode::unknown_owner, "compilations.owner_moid",
                "compilation references an unknown moid"));
        if (!compilations.emplace(compilation.id.value, &compilation).second)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, "compilations.id",
                "duplicate compilation identity"));
    }
    std::size_t compile_action_count = 0;
    for (const auto& action : snapshot.build_graph.actions) {
        if (!action.is_compile()) continue;
        ++compile_action_count;
        auto compilation = compilations.find(action.id);
        if (compilation == compilations.end())
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, "compilations",
                "compile action has no compilation description"));
        if (action.inputs.empty() || action.outputs.empty() ||
            compilation->second->source != action.inputs.front() ||
            compilation->second->planned_output != action.outputs.front() ||
            compilation->second->owner_moid != action.moid_id)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, "compilations",
                "compilation description differs from its build action"));
    }
    if (compile_action_count != snapshot.compilations.size())
        return std::unexpected(error(
            SnapshotErrorCode::identity_mismatch, "compilations",
            "compilation descriptions include an unknown action"));
    std::set<std::string> source_units;
    for (const auto& unit : snapshot.source_graph.units) {
        if (!owners.contains(unit.owner_moid))
            return std::unexpected(error(
                SnapshotErrorCode::unknown_owner, "source_graph.owner_moid",
                "source unit references an unknown moid"));
        if (!compilations.contains(unit.id.value))
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, "source_graph.id",
                "source unit has no compilation description"));
        if (!source_units.insert(unit.id.value).second)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, "source_graph.id",
                "duplicate source unit identity"));
        if (unit.source.path != compilations.at(unit.id.value)->source)
            return std::unexpected(error(
                SnapshotErrorCode::identity_mismatch, "source_graph.source",
                "source identity path differs from compilation source"));
    }
    if (source_units.size() != compilations.size())
        return std::unexpected(error(
            SnapshotErrorCode::identity_mismatch, "source_graph",
            "source graph does not cover every compilation"));
    return {};
}

}

export std::expected<InputDeclarationRecord, SnapshotError>
observe_declaration_inputs(
        const ProjectContextKey& context,
        std::vector<Path> manifest_paths,
        const Path& workspace_lock,
        std::vector<Path> build_script_paths) {
    auto canonicalize = [](std::vector<Path>& paths)
            -> std::expected<void, SnapshotError> {
        for (auto& path : paths) {
            auto canonical = detail::canonical_record_path(
                path, "declaration_inputs.path");
            if (!canonical) return std::unexpected(canonical.error());
            path = std::move(*canonical);
        }
        std::sort(paths.begin(), paths.end());
        return {};
    };
    auto manifests_ok = canonicalize(manifest_paths);
    if (!manifests_ok) return std::unexpected(manifests_ok.error());
    auto scripts_ok = canonicalize(build_script_paths);
    if (!scripts_ok) return std::unexpected(scripts_ok.error());
    auto lock = detail::canonical_record_path(
        workspace_lock, "declaration_inputs.workspace_lock");
    if (!lock) return std::unexpected(lock.error());

    InputDeclarationRecord record{context, {}};
    auto observe = [&](const Path& path) {
        std::error_code filesystem_error;
        bool exists = std::filesystem::is_regular_file(
            path.fs(), filesystem_error);
        if (filesystem_error) {
            exists = false;
            filesystem_error.clear();
        }
        std::int64_t mtime = 0;
        if (exists) {
            mtime = static_cast<std::int64_t>(
                std::filesystem::last_write_time(path.fs(), filesystem_error)
                    .time_since_epoch().count());
            if (filesystem_error) {
                exists = false;
                mtime = 0;
            }
        }
        record.inputs.push_back(ObservedDeclarationInput{
            path, exists, mtime,
        });
    };
    for (const auto& path : manifest_paths) observe(path);
    observe(*lock);
    for (const auto& path : build_script_paths) observe(path);
    std::set<std::string> unique;
    for (const auto& input : record.inputs) {
        if (!unique.insert(input.canonical_path.string()).second)
            return std::unexpected(detail::error(
                SnapshotErrorCode::identity_mismatch,
                "declaration_inputs.path",
                "declaration input categories contain a duplicate path"));
    }
    return record;
}

export bool declaration_inputs_match(const InputDeclarationRecord& stored,
                                     const InputDeclarationRecord& current) {
    return stored == current;
}

export json::Value project_snapshot_to_json(const ProjectSnapshot& snapshot) {
    json::Value document = json::Value::object();
    document["schema"] = "bake-core-project-snapshot-v1";
    document["context"] = detail::context_to_json(snapshot.context);
    document["declaration_inputs"] =
        detail::declaration_inputs_to_json(snapshot.declaration_inputs);
    document["source_index"] =
        detail::source_index_to_json(snapshot.source_index);
    document["manifests"] = json::Value::array();
    for (const auto& manifest : snapshot.manifests) {
        json::Value entry = json::Value::object();
        entry["path"] = manifest.path.string();
        entry["moid_id"] = manifest.moid_id
            ? json::Value(*manifest.moid_id) : json::Value(nullptr);
        entry["workspace_members"] = manifest.workspace_members;
        if (manifest.source_extensions) {
            json::Value extensions = json::Value::object();
            extensions["module"] = manifest.source_extensions->module_ext;
            extensions["source"] = manifest.source_extensions->source_ext;
            extensions["header"] = manifest.source_extensions->header_ext;
            entry["source_extensions"] = std::move(extensions);
        } else {
            entry["source_extensions"] = json::Value(nullptr);
        }
        document["manifests"].push_back(std::move(entry));
    }
    document["moid_graph"] = detail::moid_graph_to_json(snapshot.moid_graph);
    document["source_graph"] =
        detail::source_graph_to_json(snapshot.source_graph);
    document["build_graph"] = build_graph_to_json(snapshot.build_graph);
    document["compilations"] =
        detail::compilations_to_json(snapshot.compilations);
    return document;
}

export std::expected<ProjectSnapshot, SnapshotError>
project_snapshot_from_json(const json::Value& document,
                           const ProjectContextKey& expected_context) {
    if (!detail::exact_fields(
            document, {"schema", "context", "declaration_inputs",
                       "source_index", "manifests", "moid_graph",
                       "source_graph", "build_graph", "compilations"}))
        return std::unexpected(detail::error(
            SnapshotErrorCode::malformed, "snapshot",
            "snapshot has missing or unknown fields"));
    auto schema = detail::string_field(document, "schema", "snapshot");
    if (!schema) return std::unexpected(schema.error());
    if (*schema != "bake-core-project-snapshot-v1")
        return std::unexpected(detail::error(
            SnapshotErrorCode::unsupported_schema, "snapshot.schema",
            "unsupported project snapshot schema '" + *schema + "'"));
    auto context_value = detail::field(document, "context", "snapshot");
    auto declaration_value = detail::field(document, "declaration_inputs", "snapshot");
    auto source_index_value = detail::field(document, "source_index", "snapshot");
    auto manifests_value = detail::field(document, "manifests", "snapshot");
    auto moid_graph_value = detail::field(document, "moid_graph", "snapshot");
    auto source_graph_value = detail::field(document, "source_graph", "snapshot");
    auto build_graph_value = detail::field(document, "build_graph", "snapshot");
    auto compilations_value = detail::field(document, "compilations", "snapshot");
    if (!context_value) return std::unexpected(context_value.error());
    if (!declaration_value) return std::unexpected(declaration_value.error());
    if (!source_index_value) return std::unexpected(source_index_value.error());
    if (!manifests_value) return std::unexpected(manifests_value.error());
    if (!moid_graph_value) return std::unexpected(moid_graph_value.error());
    if (!source_graph_value) return std::unexpected(source_graph_value.error());
    if (!build_graph_value) return std::unexpected(build_graph_value.error());
    if (!compilations_value) return std::unexpected(compilations_value.error());
    auto context = detail::context_from_json(**context_value);
    auto declarations = detail::declaration_inputs_from_json(**declaration_value);
    auto sources = detail::source_index_from_json(**source_index_value);
    auto graph = detail::moid_graph_from_json(**moid_graph_value);
    auto source_graph = detail::source_graph_from_json(**source_graph_value);
    auto build_graph = build_graph_from_json(**build_graph_value);
    auto compilations = detail::compilations_from_json(**compilations_value);
    if (!context) return std::unexpected(context.error());
    if (!declarations) return std::unexpected(declarations.error());
    if (!sources) return std::unexpected(sources.error());
    if (!graph) return std::unexpected(graph.error());
    if (!source_graph) return std::unexpected(source_graph.error());
    if (!build_graph) {
        SnapshotErrorCode code = SnapshotErrorCode::malformed;
        if (build_graph.error().find("dependency") != std::string::npos)
            code = SnapshotErrorCode::invalid_dependency;
        else if (build_graph.error().find("duplicate build action id") !=
                 std::string::npos)
            code = SnapshotErrorCode::identity_mismatch;
        return std::unexpected(detail::error(
            code, "build_graph",
            build_graph.error()));
    }
    if (!compilations) return std::unexpected(compilations.error());
    if (!(*manifests_value)->is_array() || (*manifests_value)->size() == 0)
        return std::unexpected(detail::error(
            SnapshotErrorCode::malformed, "manifests",
            "snapshot requires manifest facts"));
    std::vector<ManifestRecord> manifests;
    for (std::size_t index = 0; index < (*manifests_value)->size(); ++index) {
        const auto& entry = (**manifests_value)[index];
        const std::string path = "manifests[" + std::to_string(index) + "]";
        if (!detail::exact_fields(
                entry, {"path", "moid_id", "workspace_members",
                        "source_extensions"}))
            return std::unexpected(detail::error(
                SnapshotErrorCode::malformed, path,
                "manifest record has missing or unknown fields"));
        auto file = detail::string_field(entry, "path", path);
        auto owner = detail::field(entry, "moid_id", path);
        auto members = detail::field(entry, "workspace_members", path);
        auto extensions = detail::field(entry, "source_extensions", path);
        if (!file) return std::unexpected(file.error());
        if (!owner) return std::unexpected(owner.error());
        if (!members) return std::unexpected(members.error());
        if (!extensions) return std::unexpected(extensions.error());
        std::optional<std::string> moid_id;
        if (!(*owner)->is_null()) {
            if (!(*owner)->is_string() || (*owner)->get<std::string>().empty())
                return std::unexpected(detail::error(
                    SnapshotErrorCode::malformed, path + ".moid_id",
                    "expected a nonempty string or null"));
            moid_id = (*owner)->get<std::string>();
        }
        auto workspace_members =
            detail::string_array(**members, path + ".workspace_members");
        if (!workspace_members)
            return std::unexpected(workspace_members.error());
        std::optional<SourceExtConfig> source_extensions;
        if (!(*extensions)->is_null()) {
            if (!detail::exact_fields(**extensions,
                                      {"module", "source", "header"}))
                return std::unexpected(detail::error(
                    SnapshotErrorCode::malformed, path + ".source_extensions",
                    "source extension config has missing or unknown fields"));
            auto module = detail::field(**extensions, "module",
                                        path + ".source_extensions");
            auto source = detail::field(**extensions, "source",
                                        path + ".source_extensions");
            auto header = detail::field(**extensions, "header",
                                        path + ".source_extensions");
            if (!module) return std::unexpected(module.error());
            if (!source) return std::unexpected(source.error());
            if (!header) return std::unexpected(header.error());
            auto module_values = detail::string_array(
                **module, path + ".source_extensions.module");
            auto source_values = detail::string_array(
                **source, path + ".source_extensions.source");
            auto header_values = detail::string_array(
                **header, path + ".source_extensions.header");
            if (!module_values) return std::unexpected(module_values.error());
            if (!source_values) return std::unexpected(source_values.error());
            if (!header_values) return std::unexpected(header_values.error());
            source_extensions = SourceExtConfig{
                std::move(*module_values), std::move(*source_values),
                std::move(*header_values),
            };
        }
        manifests.push_back(ManifestRecord{
            Path(*file), std::move(moid_id), std::move(*workspace_members),
            std::move(source_extensions),
        });
    }
    ProjectSnapshot snapshot{
        std::move(*context), std::move(*declarations), std::move(*sources),
        std::move(manifests), std::move(*graph), std::move(*source_graph),
        std::move(*build_graph), std::move(*compilations),
    };
    auto valid = detail::validate_snapshot(snapshot, expected_context);
    if (!valid) return std::unexpected(valid.error());
    return snapshot;
}

export std::string encode_project_snapshot(const ProjectSnapshot& snapshot) {
    return project_snapshot_to_json(snapshot).dump();
}

export std::expected<ProjectSnapshot, SnapshotError> decode_project_snapshot(
        std::string_view content,
        const ProjectContextKey& expected_context) {
    try {
        return project_snapshot_from_json(json::Value::parse(content),
                                          expected_context);
    } catch (const std::exception& exception) {
        return std::unexpected(detail::error(
            SnapshotErrorCode::malformed, "snapshot",
            "invalid snapshot JSON: " + std::string(exception.what())));
    }
}

}
