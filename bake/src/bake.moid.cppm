export module bake.moid;

import std;
import bake.util;
import bake.project;
import nlohmann.json;

namespace bake {

export struct SourceGroup {
    std::string pattern;
    bool is_public = false;
};

export struct MoidDependency {
    std::string alias;
    std::string id;
    std::vector<std::string> options;
};

export struct MoidDeclaration {
    std::string id;
    std::string name;
    std::string version;
    MoidType type = MoidType::Executable;
    std::string root;
    std::string cxx_std = "c++17";
    std::string c_std = "c17";
    std::map<std::string, BuildOption> options;
    std::vector<SourceGroup> sources;
    std::vector<std::string> public_include_dirs;
    std::vector<std::string> libraries;
    std::vector<std::string> frameworks;
    std::vector<MoidDependency> dependencies;
    // Resolved at configure time (profile/target analysis)
    std::vector<std::string> compile_flags;
    std::vector<std::pair<std::string, std::string>> compile_defines;
    std::vector<std::string> extra_include_dirs;
    std::vector<std::string> link_flags;
    // Prebuilt libraries (.a/.so paths for linking)
    std::vector<std::string> prebuilt_libs;
};

namespace {

using Json = nlohmann::json;

std::string field_path(std::string_view parent, std::string_view field) {
    if (parent.empty()) return std::string(field);
    return std::string(parent) + "." + std::string(field);
}

std::expected<const Json*, std::string> required_field(
        const Json& object, std::string_view name, std::string_view parent = {}) {
    if (!object.is_object()) {
        return std::unexpected(
            "moid declaration field '" + std::string(parent) + "' must be an object");
    }
    auto it = object.find(std::string(name));
    if (it == object.end()) {
        return std::unexpected(
            "missing required moid declaration field '" +
            field_path(parent, name) + "'");
    }
    return &*it;
}

std::expected<std::string, std::string> required_string(
        const Json& object, std::string_view name, std::string_view parent = {}) {
    auto value = required_field(object, name, parent);
    if (!value) return std::unexpected(value.error());
    if (!(*value)->is_string()) {
        return std::unexpected(
            "moid declaration field '" + field_path(parent, name) +
            "' must be a string");
    }
    return (*value)->get<std::string>();
}

std::expected<std::vector<std::string>, std::string> string_array(
        const Json& value, std::string_view path) {
    if (!value.is_array()) {
        return std::unexpected(
            "moid declaration field '" + std::string(path) + "' must be an array");
    }

    std::vector<std::string> result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (!value[i].is_string()) {
            return std::unexpected(
                "moid declaration field '" + std::string(path) + "[" +
                std::to_string(i) + "]' must be a string");
        }
        result.push_back(value[i].get<std::string>());
    }
    return result;
}

std::expected<std::vector<std::string>, std::string> required_string_array(
        const Json& object, std::string_view name, std::string_view parent = {}) {
    auto value = required_field(object, name, parent);
    if (!value) return std::unexpected(value.error());
    return string_array(**value, field_path(parent, name));
}

std::expected<std::map<std::string, BuildOption>, std::string> options_from_json(
        const Json& value, std::string_view path) {
    if (!value.is_object()) {
        return std::unexpected(
            "moid declaration field '" + std::string(path) + "' must be an object");
    }

    std::map<std::string, BuildOption> result;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!it.value().is_boolean()) {
            return std::unexpected(
                "moid declaration field '" +
                field_path(path, it.key()) + "' must be a boolean");
        }
        result.emplace(it.key(), BuildOption{it.value().get<bool>()});
    }
    return result;
}

std::expected<std::map<std::string, BuildOption>, std::string> required_options(
        const Json& object, std::string_view name, std::string_view parent = {}) {
    auto value = required_field(object, name, parent);
    if (!value) return std::unexpected(value.error());
    return options_from_json(**value, field_path(parent, name));
}

Json options_to_json(const std::map<std::string, BuildOption>& options) {
    Json result = Json::object();
    for (const auto& [name, option] : options) {
        result[name] = option.value;
    }
    return result;
}

Json compile_defines_to_json(
        const std::vector<std::pair<std::string, std::string>>& defines) {
    Json result = Json::array();
    for (const auto& [name, value] : defines) {
        result.push_back({{"macro", name}, {"value", value}});
    }
    return result;
}

std::expected<std::vector<std::pair<std::string, std::string>>, std::string>
compile_defines_from_json(const Json& value, std::string_view path) {
    if (!value.is_array()) {
        return std::unexpected(
            "moid declaration field '" + std::string(path) + "' must be an array");
    }

    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const std::string elem_path =
            std::string(path) + "[" + std::to_string(i) + "]";
        if (!value[i].is_object()) {
            return std::unexpected(
                "moid declaration field '" + elem_path + "' must be an object");
        }
        auto name = required_string(value[i], "macro", elem_path);
        if (!name) return std::unexpected(name.error());
        auto def_value = required_string(value[i], "value", elem_path);
        if (!def_value) return std::unexpected(def_value.error());
        result.emplace_back(std::move(*name), std::move(*def_value));
    }
    return result;
}

Json declaration_to_json(const MoidDeclaration& declaration) {
    Json document;
    document["id"] = declaration.id;
    document["name"] = declaration.name;
    document["version"] = declaration.version;
    document["type"] = moid_type_str(declaration.type);
    document["root"] = declaration.root;
    document["cxx_std"] = declaration.cxx_std;
    document["c_std"] = declaration.c_std;
    document["options"] = options_to_json(declaration.options);

    document["sources"] = Json::array();
    for (const auto& source : declaration.sources) {
        Json entry;
        entry["pattern"] = source.pattern;
        entry["visibility"] = source.is_public ? "public" : "private";
        document["sources"].push_back(std::move(entry));
    }

    document["public_include_dirs"] = declaration.public_include_dirs;
    document["link"] = {
        {"libraries", declaration.libraries},
        {"frameworks", declaration.frameworks},
    };

    document["dependencies"] = Json::array();
    for (const auto& dependency : declaration.dependencies) {
        document["dependencies"].push_back({
            {"alias", dependency.alias},
            {"id", dependency.id},
            {"options", dependency.options},
        });
    }

    document["flags"] = declaration.compile_flags;
    document["defines"] = compile_defines_to_json(declaration.compile_defines);
    document["include_dirs"] = declaration.extra_include_dirs;
    document["link_flags"] = declaration.link_flags;
    document["prebuilt_libs"] = declaration.prebuilt_libs;

    return document;
}

std::expected<MoidDeclaration, std::string> declaration_from_json(
        const Json& document) {
    if (!document.is_object())
        return std::unexpected("moid declaration must be a JSON object");

    MoidDeclaration declaration;

    auto id = required_string(document, "id");
    if (!id) return std::unexpected(id.error());
    declaration.id = std::move(*id);

    auto name = required_string(document, "name");
    if (!name) return std::unexpected(name.error());
    declaration.name = std::move(*name);

    auto version = required_string(document, "version");
    if (!version) return std::unexpected(version.error());
    declaration.version = std::move(*version);

    auto type = document.find("type");
    if (type != document.end()) {
        if (!type->is_string()) {
            return std::unexpected(
                "moid declaration field 'type' must be a string");
        }
        auto parsed_type = parse_moid_type(type->get<std::string>());
        if (!parsed_type) return std::unexpected(parsed_type.error());
        declaration.type = *parsed_type;
    }

    auto root = required_string(document, "root");
    if (!root) return std::unexpected(root.error());
    declaration.root = std::move(*root);

    auto cxx_standard = required_string(document, "cxx_std");
    if (!cxx_standard) return std::unexpected(cxx_standard.error());
    declaration.cxx_std = std::move(*cxx_standard);

    auto c_standard = required_string(document, "c_std");
    if (!c_standard) return std::unexpected(c_standard.error());
    declaration.c_std = std::move(*c_standard);

    auto options = required_options(document, "options");
    if (!options) return std::unexpected(options.error());
    declaration.options = std::move(*options);

    auto sources = required_field(document, "sources");
    if (!sources) return std::unexpected(sources.error());
    if (!(**sources).is_array()) {
        return std::unexpected(
            "moid declaration field 'sources' must be an array");
    }
    for (std::size_t i = 0; i < (**sources).size(); ++i) {
        const Json& source = (**sources)[i];
        const std::string path = "sources[" + std::to_string(i) + "]";
        if (!source.is_object()) {
            return std::unexpected(
                "moid declaration field '" + path + "' must be an object");
        }

        SourceGroup group;
        auto pattern = required_string(source, "pattern", path);
        if (!pattern) return std::unexpected(pattern.error());
        group.pattern = std::move(*pattern);

        auto visibility = required_string(source, "visibility", path);
        if (!visibility) return std::unexpected(visibility.error());
        if (*visibility == "public") {
            group.is_public = true;
        } else if (*visibility != "private") {
            return std::unexpected(
                "unknown source visibility '" + *visibility + "'");
        }

        declaration.sources.push_back(std::move(group));
    }

    auto public_include_dirs =
        required_string_array(document, "public_include_dirs");
    if (!public_include_dirs)
        return std::unexpected(public_include_dirs.error());
    declaration.public_include_dirs = std::move(*public_include_dirs);

    auto link = required_field(document, "link");
    if (!link) return std::unexpected(link.error());
    if (!(**link).is_object()) {
        return std::unexpected(
            "moid declaration field 'link' must be an object");
    }
    auto libraries = required_string_array(**link, "libraries", "link");
    if (!libraries) return std::unexpected(libraries.error());
    declaration.libraries = std::move(*libraries);
    auto frameworks = required_string_array(**link, "frameworks", "link");
    if (!frameworks) return std::unexpected(frameworks.error());
    declaration.frameworks = std::move(*frameworks);

    auto dependencies = required_field(document, "dependencies");
    if (!dependencies) return std::unexpected(dependencies.error());
    if (!(**dependencies).is_array()) {
        return std::unexpected(
            "moid declaration field 'dependencies' must be an array");
    }

    std::set<std::string> aliases;
    for (std::size_t i = 0; i < (**dependencies).size(); ++i) {
        const Json& dependency = (**dependencies)[i];
        const std::string path = "dependencies[" + std::to_string(i) + "]";
        if (!dependency.is_object()) {
            return std::unexpected(
                "moid declaration field '" + path + "' must be an object");
        }

        MoidDependency value;
        auto alias = required_string(dependency, "alias", path);
        if (!alias) return std::unexpected(alias.error());
        value.alias = std::move(*alias);
        if (!aliases.insert(value.alias).second) {
            return std::unexpected(
                "duplicate moid dependency alias '" + value.alias + "'");
        }

        auto dependency_id = required_string(dependency, "id", path);
        if (!dependency_id) return std::unexpected(dependency_id.error());
        value.id = std::move(*dependency_id);

        auto dependency_options =
            required_string_array(dependency, "options", path);
        if (!dependency_options)
            return std::unexpected(dependency_options.error());
        value.options = std::move(*dependency_options);

        declaration.dependencies.push_back(std::move(value));
    }

    auto compile_flags = required_string_array(document, "flags");
    if (!compile_flags) return std::unexpected(compile_flags.error());
    declaration.compile_flags = std::move(*compile_flags);

    auto compile_defines_field = required_field(document, "defines");
    if (!compile_defines_field)
        return std::unexpected(compile_defines_field.error());
    auto compile_defines =
        compile_defines_from_json(**compile_defines_field, "defines");
    if (!compile_defines) return std::unexpected(compile_defines.error());
    declaration.compile_defines = std::move(*compile_defines);

    auto extra_include_dirs =
        required_string_array(document, "include_dirs");
    if (!extra_include_dirs)
        return std::unexpected(extra_include_dirs.error());
    declaration.extra_include_dirs = std::move(*extra_include_dirs);

    auto link_flags = required_string_array(document, "link_flags");
    if (!link_flags) return std::unexpected(link_flags.error());
    declaration.link_flags = std::move(*link_flags);

    auto prebuilt_libs = required_string_array(document, "prebuilt_libs");
    if (!prebuilt_libs) return std::unexpected(prebuilt_libs.error());
    declaration.prebuilt_libs = std::move(*prebuilt_libs);

    return declaration;
}

} // namespace

export std::expected<MoidDeclaration, std::string>
read_moid_declaration(const Path& path) {
    auto content = read_file(path);
    if (!content) {
        return std::unexpected(
            "failed to read moid declaration '" + path.string() + "'");
    }

    try {
        return declaration_from_json(Json::parse(*content));
    } catch (const std::exception& error) {
        return std::unexpected(
            "invalid moid declaration JSON: " + std::string(error.what()));
    }
}

export std::expected<void, std::string>
write_moid_declaration(const Path& path, const MoidDeclaration& declaration) {
    Json document = declaration_to_json(declaration);
    auto validated = declaration_from_json(document);
    if (!validated) return std::unexpected(validated.error());
    if (!atomic_write_file(path, document.dump(2))) {
        return std::unexpected(
            "failed to write moid declaration '" + path.string() + "'");
    }
    return {};
}

} // namespace bake
