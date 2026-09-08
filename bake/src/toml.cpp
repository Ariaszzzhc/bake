// stage1 implementation — toml++-backed. Compiled only by the
// self-hosted bake; the module comes from the bake-pkgs "tomlplusplus"
// package (module/tomlplusplus.cppm wrapping upstream headers).

module bake.toml;

import std;
import tomlplusplus;

namespace bake::toml {
namespace {

namespace tt = ::toml;

[[noreturn]] void fail(std::string_view source,
                       std::size_t line,
                       std::size_t column,
                       std::string_view message) {
    throw Error(std::string(source) + ": line " + std::to_string(line) +
                ", column " + std::to_string(column) + ": " +
                std::string(message));
}

std::string node_source(const tt::node& node, std::string_view fallback) {
    const auto& region = node.source();
    return region.path ? *region.path : std::string(fallback);
}

[[noreturn]] void unsupported_date(const tt::node& node,
                                   std::string_view source) {
    const auto& begin = node.source().begin;
    fail(node_source(node, source), begin.line, begin.column,
         "dates are not supported");
}

std::size_t source_offset(std::string_view text,
                          const tt::source_position& position) {
    std::size_t offset = 0;
    std::size_t line = 1;
    std::size_t column = 1;
    while (offset < text.size() &&
           (line < position.line || column < position.column)) {
        unsigned char c = static_cast<unsigned char>(text[offset]);
        if (c == '\n') {
            ++offset;
            ++line;
            column = 1;
            continue;
        }
        if ((c & 0x80u) == 0u)
            ++offset;
        else if ((c & 0xe0u) == 0xc0u)
            offset += 2;
        else if ((c & 0xf0u) == 0xe0u)
            offset += 3;
        else
            offset += 4;
        ++column;
    }
    return offset;
}

bool same_region(const tt::source_region& lhs,
                 const tt::source_region& rhs) {
    return lhs.begin == rhs.begin && lhs.end == rhs.end;
}

Node from_toml(const tt::node& node,
               std::string_view text,
               std::string_view source) {
    switch (node.type()) {
        case tt::node_type::table: {
            std::vector<std::pair<std::string, Node>> entries;
            for (const auto& [key, value] : *node.as_table()) {
                if (value.is_table() &&
                    same_region(key.source(), value.source())) {
                    fail(node_source(value, source), key.source().begin.line,
                         key.source().begin.column,
                         "dotted keys are not supported");
                }
                entries.emplace_back(
                    key.str(), from_toml(value, text, source));
            }
            return Node(make_table(std::move(entries)));
        }
        case tt::node_type::array: {
            std::size_t offset = source_offset(text, node.source().begin);
            if (node.as_array()->is_array_of_tables() &&
                offset + 1 < text.size() &&
                text.substr(offset, 2) == "[[") {
                const auto& begin = node.source().begin;
                fail(node_source(node, source), begin.line, begin.column,
                     "arrays of tables ([[...]]) are not supported");
            }
            Array items;
            for (const auto& item : *node.as_array())
                items.push_back(from_toml(item, text, source));
            return Node(std::move(items));
        }
        case tt::node_type::string: {
            std::size_t offset = source_offset(text, node.source().begin);
            if (offset + 2 < text.size() &&
                (text.substr(offset, 3) == "\"\"\"" ||
                 text.substr(offset, 3) == "'''")) {
                fail(node_source(node, source), node.source().begin.line,
                     node.source().begin.column,
                     "multiline strings are not supported");
            }
            return Node(node.value<std::string>().value_or(""));
        }
        case tt::node_type::integer:
            return Node(node.value<std::int64_t>().value_or(0));
        case tt::node_type::floating_point:
            return Node(node.value<double>().value_or(0.0));
        case tt::node_type::boolean:
            return Node(node.value<bool>().value_or(false));
        case tt::node_type::date:
        case tt::node_type::time:
        case tt::node_type::date_time:
            unsupported_date(node, source);
        default:
            throw Error("unsupported TOML node type");
    }
}

Table table_from(const tt::table& parsed,
                 std::string_view text,
                 std::string_view source) {
    Node root = from_toml(parsed, text, source);
    Table table;
    if (auto* inner = node_table(root)) table = std::move(*inner);
    return table;
}

std::string quoted_name(std::string_view description) {
    std::size_t end = description.rfind('\'');
    if (end == std::string_view::npos) return {};
    std::size_t begin = description.rfind('\'', end - 1);
    if (begin == std::string_view::npos) return {};
    return std::string(description.substr(begin + 1, end - begin - 1));
}

[[noreturn]] void translate(const tt::parse_error& error,
                            std::string_view fallback_source) {
    std::string_view description = error.description();
    std::string message(description);
    if (description.find("cannot redefine existing table") !=
        std::string_view::npos) {
        std::string name = quoted_name(description);
        message = "table [" + name + "] defined twice";
    } else if (description.find("cannot redefine existing") !=
               std::string_view::npos) {
        std::string name = quoted_name(description);
        message = "duplicate key '" + name + "'";
    } else if (description.find("expected value") !=
               std::string_view::npos) {
        message = "expected a value";
    }

    const auto& region = error.source();
    std::string source = region.path ? *region.path
                                     : std::string(fallback_source);
    fail(source, region.begin.line, region.begin.column, message);
}

}  // namespace

Table parse(std::string_view text, std::string_view source) {
    try {
        tt::table parsed =
            tt::parse(text, source);
        return table_from(parsed, text, source);
    } catch (const tt::parse_error& error) {
        translate(error, source);
    }
}

Table parse_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("failed to open '" + path + "'");
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse(buffer.str(), path);
}

}  // namespace bake::toml
