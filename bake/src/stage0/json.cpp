module bake.json;

// stage0 implementation — hand-written recursive-descent parser and
// nlohmann-compatible serializer. Compiled only by the CMake bootstrap
// (and the unit tests); the self-hosted bake links json.cpp instead.

import std;

namespace bake::json {
namespace {

[[noreturn]] void fail(std::size_t offset, const std::string& message) {
    throw Error("bake.json: " + message + " (offset " +
                std::to_string(offset) + ")");
}

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xC0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
        out += static_cast<char>(0xE0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code_point >> 18));
        out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    }
}

class Parser {
  public:
    explicit Parser(std::string_view text) : text_(text) {}

    Value run() {
        Value result = parse_value();
        skip_ws();
        if (pos_ != text_.size())
            fail(pos_, "unexpected trailing content");
        return result;
    }

  private:
    void skip_ws() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    char peek() const {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    void expect(std::string_view literal) {
        if (!text_.substr(pos_).starts_with(literal))
            fail(pos_, "invalid literal");
        pos_ += literal.size();
    }

    Value parse_value() {
        switch (peek()) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': return Value(parse_string());
            case 't': expect("true");  return Value(true);
            case 'f': expect("false"); return Value(false);
            case 'n': expect("null");  return Value(nullptr);
            default:  return parse_number();
        }
    }

    Value parse_object() {
        Value obj = Value::object();
        ++pos_;  // '{'
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            return obj;
        }
        while (true) {
            skip_ws();
            if (peek() != '"') fail(pos_, "expected a string object key");
            std::string key = parse_string();
            skip_ws();
            if (peek() != ':') fail(pos_, "expected ':' after object key");
            ++pos_;
            skip_ws();
            obj[key] = parse_value();  // duplicate keys: last wins
            skip_ws();
            char c = peek();
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == '}') {
                ++pos_;
                return obj;
            }
            fail(pos_, "expected ',' or '}' in object");
        }
    }

    Value parse_array() {
        Value arr = Value::array();
        ++pos_;  // '['
        skip_ws();
        if (peek() == ']') {
            ++pos_;
            return arr;
        }
        while (true) {
            skip_ws();
            arr.push_back(parse_value());
            skip_ws();
            char c = peek();
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == ']') {
                ++pos_;
                return arr;
            }
            fail(pos_, "expected ',' or ']' in array");
        }
    }

    std::string parse_string() {
        ++pos_;  // '"'
        std::string out;
        while (true) {
            if (pos_ >= text_.size()) fail(pos_, "unterminated string");
            char c = text_[pos_];
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c == '\\') {
                ++pos_;
                parse_escape(out);
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20)
                fail(pos_, "unescaped control character in string");
            out += c;
            ++pos_;
        }
    }

    void parse_escape(std::string& out) {
        if (pos_ >= text_.size()) fail(pos_, "unterminated escape");
        char c = text_[pos_++];
        switch (c) {
            case '"':  out += '"';  return;
            case '\\': out += '\\'; return;
            case '/':  out += '/';  return;
            case 'b':  out += '\b'; return;
            case 'f':  out += '\f'; return;
            case 'n':  out += '\n'; return;
            case 'r':  out += '\r'; return;
            case 't':  out += '\t'; return;
            case 'u':  break;
            default: fail(pos_ - 1, "invalid string escape");
        }
        std::uint32_t unit = parse_hex4();
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' ||
                text_[pos_ + 1] != 'u')
                fail(pos_, "unpaired high surrogate");
            pos_ += 2;
            std::uint32_t low = parse_hex4();
            if (low < 0xDC00 || low > 0xDFFF)
                fail(pos_, "invalid low surrogate");
            append_utf8(out, 0x10000 + ((unit - 0xD800) << 10) +
                                     (low - 0xDC00));
            return;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF)
            fail(pos_, "unpaired low surrogate");
        append_utf8(out, unit);
    }

    std::uint32_t parse_hex4() {
        if (pos_ + 4 > text_.size()) fail(pos_, "truncated \\u escape");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                fail(pos_ - 1, "invalid hex digit in \\u escape");
        }
        return value;
    }

    Value parse_number() {
        std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (!scan_digits()) fail(start, "invalid number");
        bool is_double = false;
        if (peek() == '.') {
            is_double = true;
            ++pos_;
            if (!scan_digits()) fail(start, "invalid number");
        }
        if (peek() == 'e' || peek() == 'E') {
            is_double = true;
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!scan_digits()) fail(start, "invalid number");
        }
        std::string_view span = text_.substr(start, pos_ - start);
        if (is_double) {
            double result = 0;
            std::from_chars(span.data(), span.data() + span.size(), result);
            return Value(result);
        }
        std::int64_t result = 0;
        auto [ptr, ec] = std::from_chars(
            span.data(), span.data() + span.size(), result);
        if (ec == std::errc::result_out_of_range) {
            double big = 0;
            std::from_chars(span.data(), span.data() + span.size(), big);
            return Value(big);
        }
        return Value(result);
    }

    bool scan_digits() {
        std::size_t start = pos_;
        while (peek() >= '0' && peek() <= '9') ++pos_;
        return pos_ > start;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

std::string escape_string(std::string_view value) {
    std::string out = "\"";
    for (char c : value) {
        switch (c) {
            case '"':  out += "\\\""; continue;
            case '\\': out += "\\\\"; continue;
            case '\b': out += "\\b";  continue;
            case '\f': out += "\\f";  continue;
            case '\n': out += "\\n";  continue;
            case '\r': out += "\\r";  continue;
            case '\t': out += "\\t";  continue;
            default: break;
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            out += "\\u00";
            constexpr char digits[] = "0123456789abcdef";
            out += digits[(static_cast<unsigned char>(c) >> 4) & 0xF];
            out += digits[static_cast<unsigned char>(c) & 0xF];
            continue;
        }
        out += c;
    }
    out += '"';
    return out;
}

void write_value(const Value& value, std::string& out, int indent, int depth);

void write_padded(std::string& out, int indent, int depth) {
    out.append(static_cast<std::size_t>(indent) * depth, ' ');
}

void write_value(const Value& value, std::string& out, int indent, int depth) {
    if (value.is_null()) {
        out += "null";
    } else if (value.is_boolean()) {
        out += value.get<bool>() ? "true" : "false";
    } else if (value.is_integer()) {
        out += std::to_string(value.get<std::int64_t>());
    } else if (value.is_number()) {
        double d = value.get<double>();
        if (std::isnan(d) || std::isinf(d)) {
            out += "null";
        } else if (d == std::floor(d) && std::fabs(d) < 1e15) {
            out += std::format("{:.1f}", d);
        } else {
            out += std::format("{}", d);
        }
    } else if (value.is_string()) {
        out += escape_string(value.get<std::string>());
    } else if (value.is_array()) {
        if (value.size() == 0) {
            out += "[]";
            return;
        }
        out += '[';
        bool first = true;
        for (const Value& item : value) {
            if (!first) out += ',';
            first = false;
            if (indent >= 0) {
                out += '\n';
                write_padded(out, indent, depth + 1);
            }
            write_value(item, out, indent, depth + 1);
        }
        if (indent >= 0) {
            out += '\n';
            write_padded(out, indent, depth);
        }
        out += ']';
    } else {
        if (value.size() == 0) {
            out += "{}";
            return;
        }
        out += '{';
        bool first = true;
        for (const auto& member : value.items()) {
            if (!first) out += ',';
            first = false;
            if (indent >= 0) {
                out += '\n';
                write_padded(out, indent, depth + 1);
            }
            out += escape_string(member.key());
            out += ':';
            if (indent >= 0) out += ' ';
            write_value(member.value(), out, indent, depth + 1);
        }
        if (indent >= 0) {
            out += '\n';
            write_padded(out, indent, depth);
        }
        out += '}';
    }
}

}  // namespace

Value Value::parse(std::string_view text) {
    return Parser(text).run();
}

std::string Value::dump(int indent) const {
    std::string out;
    write_value(*this, out, indent, 0);
    return out;
}

}  // namespace bake::json
