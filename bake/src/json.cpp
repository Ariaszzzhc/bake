// stage1 implementation — nlohmann/json-backed. Compiled only by the
// self-hosted bake; the module comes from the bake-pkgs "json"
// package (module/nlohmann.json.cppm wrapping upstream headers).

module bake.json;

import std;
import nlohmann.json;

namespace bake::json {
namespace {

using Lib = nlohmann::json;

Value from_lib(const Lib& node) {
    switch (node.type()) {
        case Lib::value_t::object: {
            Value out = Value::object();
            for (auto item = node.begin(); item != node.end(); ++item)
                out[item.key()] = from_lib(item.value());
            return out;
        }
        case Lib::value_t::array: {
            Value out = Value::array();
            for (const Lib& item : node) out.push_back(from_lib(item));
            return out;
        }
        case Lib::value_t::string:
            return Value(node.get<std::string>());
        case Lib::value_t::boolean:
            return Value(node.get<bool>());
        case Lib::value_t::number_integer:
            return Value(node.get<std::int64_t>());
        case Lib::value_t::number_unsigned:
            return Value(static_cast<std::int64_t>(
                node.get<std::uint64_t>()));
        case Lib::value_t::number_float:
            return Value(node.get<double>());
        default:
            return Value();
    }
}

Lib to_lib(const Value& value) {
    if (value.is_null()) return Lib();
    if (value.is_boolean()) return Lib(value.get<bool>());
    if (value.is_integer()) return Lib(value.get<std::int64_t>());
    if (value.is_number()) return Lib(value.get<double>());
    if (value.is_string()) return Lib(value.get<std::string>());
    if (value.is_array()) {
        Lib out = Lib::array();
        for (const Value& item : value) out.push_back(to_lib(item));
        return out;
    }
    Lib out = Lib::object();
    for (const auto& member : value.items())
        out[member.key()] = to_lib(member.value());
    return out;
}

}  // namespace

Value Value::parse(std::string_view text) {
    try {
        return from_lib(Lib::parse(text));
    } catch (const Lib::exception& error) {
        throw Error(std::string("bake.json: ") + error.what());
    }
}

std::string Value::dump(int indent) const {
    return to_lib(*this).dump(indent);
}

}  // namespace bake::json
