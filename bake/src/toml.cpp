// stage1 implementation — toml++-backed. Compiled only by the
// self-hosted bake; the module comes from the bake-pkgs "tomlplusplus"
// package (module/tomlplusplus.cppm wrapping upstream headers).

module bake.toml;

import std;
import tomlplusplus;

namespace bake::toml {
namespace {

namespace tt = ::toml;

Node from_toml(const tt::node& node) {
    switch (node.type()) {
        case tt::node_type::table: {
            std::vector<std::pair<std::string, Node>> entries;
            for (const auto& [key, value] : *node.as_table())
                entries.emplace_back(key.str(), from_toml(value));
            return Node(make_table(std::move(entries)));
        }
        case tt::node_type::array: {
            Array items;
            for (const auto& item : *node.as_array())
                items.push_back(from_toml(item));
            return Node(std::move(items));
        }
        case tt::node_type::string:
            return Node(node.value<std::string>().value_or(""));
        case tt::node_type::integer:
            return Node(node.value<std::int64_t>().value_or(0));
        case tt::node_type::floating_point:
            return Node(node.value<double>().value_or(0.0));
        case tt::node_type::boolean:
            return Node(node.value<bool>().value_or(false));
        default:
            return Node();
    }
}

Table table_from(const tt::table& parsed) {
    Node root = from_toml(parsed);
    Table table;
    if (auto* inner = node_table(root)) table = std::move(*inner);
    return table;
}

}  // namespace

Table parse(std::string_view text, std::string_view source) {
    try {
        tt::table parsed =
            tt::parse(text, source);
        return table_from(parsed);
    } catch (const tt::parse_error& error) {
        throw Error(error.what());
    }
}

Table parse_file(const std::string& path) {
    try {
        tt::table parsed = tt::parse_file(path);
        return table_from(parsed);
    } catch (const std::exception& error) {
        throw Error(error.what());
    }
}

}  // namespace bake::toml
