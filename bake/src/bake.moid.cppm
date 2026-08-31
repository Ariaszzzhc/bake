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

export struct BinaryDeclaration {
    std::string name;
    std::vector<SourceGroup> sources;
    std::vector<std::string> include_dirs;
};

export struct TestRegistration {
    std::string name;
    std::string binary;
    bool is_default = false;
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
    // Extra executables sharing the main moid's exports and configuration
    std::vector<BinaryDeclaration> binaries;
    // Named test mappings onto declared binaries
    std::vector<TestRegistration> tests;
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

std::expected<std::vector<SourceGroup>, std::string> source_groups_from_json(
        const Json& value, std::string_view path) {
    if (!value.is_array()) {
        return std::unexpected(
            "moid declaration field '" + std::string(path) +
            "' must be an array");
    }

    std::vector<SourceGroup> result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const std::string entry_path =
            std::string(path) + "[" + std::to_string(i) + "]";
        if (!value[i].is_object()) {
            return std::unexpected(
                "moid declaration field '" + entry_path +
                "' must be an object");
        }

        SourceGroup group;
        auto pattern = required_string(value[i], "pattern", entry_path);
        if (!pattern) return std::unexpected(pattern.error());
        group.pattern = std::move(*pattern);

        auto visibility =
            required_string(value[i], "visibility", entry_path);
        if (!visibility) return std::unexpected(visibility.error());
        if (*visibility == "public") {
            group.is_public = true;
        } else if (*visibility != "private") {
            return std::unexpected(
                "unknown source visibility '" + *visibility + "'");
        }
        result.push_back(std::move(group));
    }
    return result;
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

    document["binaries"] = Json::array();
    for (const auto& binary : declaration.binaries) {
        Json entry;
        entry["name"] = binary.name;
        entry["sources"] = Json::array();
        for (const auto& source : binary.sources) {
            entry["sources"].push_back({
                {"pattern", source.pattern},
                {"visibility", source.is_public ? "public" : "private"},
            });
        }
        entry["include_dirs"] = binary.include_dirs;
        document["binaries"].push_back(std::move(entry));
    }

    document["tests"] = Json::array();
    for (const auto& test : declaration.tests) {
        document["tests"].push_back({
            {"name", test.name},
            {"binary", test.binary},
            {"is_default", test.is_default},
        });
    }

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
    auto source_groups =
        source_groups_from_json(**sources, "sources");
    if (!source_groups) return std::unexpected(source_groups.error());
    declaration.sources = std::move(*source_groups);

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
    // Optional: extra binaries and test registrations (build.cpp-declared
    // inputs). Cached declarations written before these fields existed parse
    // as empty.
    std::set<std::string> binary_names;
    auto binaries_field = document.find("binaries");
    if (binaries_field != document.end()) {
        if (!binaries_field->is_array()) {
            return std::unexpected(
                "moid declaration field 'binaries' must be an array");
        }
        for (std::size_t i = 0; i < binaries_field->size(); ++i) {
            const Json& entry = (*binaries_field)[i];
            const std::string path =
                "binaries[" + std::to_string(i) + "]";
            if (!entry.is_object()) {
                return std::unexpected(
                    "moid declaration field '" + path +
                    "' must be an object");
            }

            BinaryDeclaration binary;
            auto name = required_string(entry, "name", path);
            if (!name) return std::unexpected(name.error());
            binary.name = std::move(*name);
            if (!binary_names.insert(binary.name).second) {
                return std::unexpected(
                    "duplicate binary name '" + binary.name + "'");
            }

            auto sources = required_field(entry, "sources", path);
            if (!sources) return std::unexpected(sources.error());
            auto source_groups =
                source_groups_from_json(**sources, path + ".sources");
            if (!source_groups)
                return std::unexpected(source_groups.error());
            binary.sources = std::move(*source_groups);

            auto include_dirs =
                required_string_array(entry, "include_dirs", path);
            if (!include_dirs)
                return std::unexpected(include_dirs.error());
            binary.include_dirs = std::move(*include_dirs);

            declaration.binaries.push_back(std::move(binary));
        }
    }

    auto tests_field = document.find("tests");
    if (tests_field != document.end()) {
        if (!tests_field->is_array()) {
            return std::unexpected(
                "moid declaration field 'tests' must be an array");
        }
        std::set<std::string> test_names;
        for (std::size_t i = 0; i < tests_field->size(); ++i) {
            const Json& entry = (*tests_field)[i];
            const std::string path = "tests[" + std::to_string(i) + "]";
            if (!entry.is_object()) {
                return std::unexpected(
                    "moid declaration field '" + path +
                    "' must be an object");
            }

            TestRegistration test;
            auto name = required_string(entry, "name", path);
            if (!name) return std::unexpected(name.error());
            test.name = std::move(*name);
            if (!test_names.insert(test.name).second) {
                return std::unexpected(
                    "duplicate test name '" + test.name + "'");
            }

            auto binary_name = required_string(entry, "binary", path);
            if (!binary_name) return std::unexpected(binary_name.error());
            test.binary = std::move(*binary_name);
            if (!binary_names.contains(test.binary)) {
                return std::unexpected(
                    "test '" + test.name + "' references undeclared binary '" +
                    test.binary + "'");
            }

            auto is_default = required_field(entry, "is_default", path);
            if (!is_default) return std::unexpected(is_default.error());
            if (!(**is_default).is_boolean()) {
                return std::unexpected(
                    "moid declaration field '" + path +
                    ".is_default' must be a boolean");
            }
            test.is_default = (**is_default).get<bool>();

            declaration.tests.push_back(std::move(test));
        }
    }

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
