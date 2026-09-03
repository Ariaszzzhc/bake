module bake.toml;

// stage0 implementation — hand-written subset parser. Compiled only by
// the CMake bootstrap (and the unit tests); the self-hosted bake links
// toml.cpp instead.
//
// Accepted grammar (the bake.toml surface): [table] headers with dotted
// and quoted segments, bare/quoted keys, basic/literal strings,
// integers, floats, booleans, multiline arrays with trailing commas,
// inline tables. Everything else — dates, multiline strings, [[array
// tables]], dotted key assignments — is rejected with a position.

import std;

namespace bake::toml {
namespace {

class Parser {
  public:
    Parser(std::string_view text, std::string_view source)
        : text_(text), source_(source) {}

    Table run() {
        Table root;
        std::vector<std::string> prefix;
        std::set<std::vector<std::string>> headers;

        while (true) {
            skip_blank();
            if (at_end()) break;

            if (peek() == '[') {
                if (peek(1) == '[')
                    fail("arrays of tables ([[...]]) are not supported");
                advance();
                skip_spaces();
                std::vector<std::string> path = parse_dotted_key();
                skip_spaces();
                if (peek() != ']')
                    fail("expected ']' after table header");
                advance();
                end_of_line();
                if (!headers.insert(path).second)
                    fail("table [" + join(path) + "] defined twice");
                ensure_path(root, path);
                prefix = std::move(path);
            } else {
                std::string key = parse_key_part();
                skip_spaces();
                if (peek() != '.') {
                    if (peek() != '=')
                        fail("expected '=' after key");
                    advance();
                    skip_spaces();
                    Node value = parse_value();
                    Table& table = navigate(root, prefix);
                    if (table_find(table, key))
                        fail("duplicate key '" + key + "'");
                    table_insert(table, key, std::move(value));
                    end_of_line();
                } else {
                    fail("dotted keys are not supported");
                }
            }
        }
        return root;
    }

  private:
    [[noreturn]] void fail(const std::string& message) const {
        throw Error(std::string(source_) + ": line " +
                    std::to_string(line_) + ", column " +
                    std::to_string(column_) + ": " + message);
    }

    bool at_end() const { return pos_ >= text_.size(); }
    char peek() const { return at_end() ? '\0' : text_[pos_]; }
    char peek(std::size_t ahead) const {
        return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0';
    }

    void advance() {
        if (at_end()) return;
        if (text_[pos_] == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        ++pos_;
    }

    static bool is_space(char c) { return c == ' ' || c == '\t'; }

    void skip_spaces() {
        while (is_space(peek())) advance();
    }

    void skip_comment() {
        if (peek() != '#') return;
        while (!at_end() && peek() != '\n') advance();
    }

    // Spaces, comments and newlines (used inside arrays).
    void skip_blank() {
        while (true) {
            char c = peek();
            if (is_space(c) || c == '\n' || c == '\r')
                advance();
            else if (c == '#')
                skip_comment();
            else
                break;
        }
    }

    void end_of_line() {
        skip_spaces();
        skip_comment();
        if (!at_end() && peek() != '\n' && peek() != '\r')
            fail("unexpected content after value");
    }

    static std::string join(const std::vector<std::string>& path) {
        std::string out;
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (i > 0) out += '.';
            out += path[i];
        }
        return out;
    }

    Table& navigate(Table& root, const std::vector<std::string>& prefix) {
        Table* current = &root;
        for (const std::string& segment : prefix) {
            auto* entry = table_find(*current, segment);
            if (!entry)
                fail("internal: table [" + join(prefix) + "] vanished");
            Table* next = node_table(entry->second);
            if (!next)
                fail("key '" + segment + "' is not a table");
            current = next;
        }
        return *current;
    }

    void ensure_path(Table& root, const std::vector<std::string>& path) {
        Table* current = &root;
        for (const std::string& segment : path) {
            auto* entry = table_find(*current, segment);
            if (!entry) {
                table_insert(*current, segment, Node(Table{}));
                entry = table_find(*current, segment);
            } else if (!entry->second.is_table()) {
                fail("key '" + segment + "' conflicts with a non-table value");
            }
            current = node_table(entry->second);
        }
    }

    // ── keys ──

    static bool is_bare_key_char(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    }

    std::string parse_key_part() {
        char c = peek();
        if (c == '"') return parse_basic_string();
        if (c == '\'') return parse_literal_string();
        std::string out;
        while (is_bare_key_char(peek())) {
            out += peek();
            advance();
        }
        if (out.empty()) fail("expected a key");
        return out;
    }

    std::vector<std::string> parse_dotted_key() {
        std::vector<std::string> parts;
        while (true) {
            parts.push_back(parse_key_part());
            skip_spaces();
            if (peek() == '.') {
                advance();
                skip_spaces();
                continue;
            }
            return parts;
        }
    }

    // ── strings ──

    std::string parse_basic_string() {
        if (peek(1) == '"' && peek(2) == '"')
            fail("multiline strings are not supported");
        advance();  // '"'
        std::string out;
        while (true) {
            char c = peek();
            if (c == '"') {
                advance();
                return out;
            }
            if (c == '\n' || c == '\0')
                fail("unterminated string");
            if (c == '\\') {
                advance();
                parse_escape(out);
                continue;
            }
            out += c;
            advance();
        }
    }

    void parse_escape(std::string& out) {
        char c = peek();
        advance();
        switch (c) {
            case '"':  out += '"';  return;
            case '\\': out += '\\'; return;
            case 'b':  out += '\b'; return;
            case 'f':  out += '\f'; return;
            case 'n':  out += '\n'; return;
            case 'r':  out += '\r'; return;
            case 't':  out += '\t'; return;
            case 'u':  append_utf8(out, parse_hex(4));  return;
            case 'U':  append_utf8(out, parse_hex(8));  return;
            default: fail("invalid escape in string");
        }
    }

    std::uint32_t parse_hex(int digits) {
        std::uint32_t value = 0;
        for (int i = 0; i < digits; ++i) {
            char c = peek();
            advance();
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                fail("invalid hex digit in \\u escape");
        }
        return value;
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::string parse_literal_string() {
        if (peek(1) == '\'' && peek(2) == '\'')
            fail("multiline strings are not supported");
        advance();  // '\''
        std::string out;
        while (true) {
            char c = peek();
            if (c == '\'') {
                advance();
                return out;
            }
            if (c == '\n' || c == '\0')
                fail("unterminated string");
            out += c;
            advance();
        }
    }

    // ── values ──

    Node parse_value() {
        char c = peek();
        switch (c) {
            case '"': return Node(parse_basic_string());
            case '\'': return Node(parse_literal_string());
            case '[': return parse_array();
            case '{': return parse_inline_table();
            case 't':
                expect_word("true");
                return Node(true);
            case 'f':
                expect_word("false");
                return Node(false);
            default:
                if (c == '+' || c == '-' ||
                    (c >= '0' && c <= '9'))
                    return parse_number();
                fail("expected a value");
        }
    }

    void expect_word(std::string_view word) {
        if (!text_.substr(pos_).starts_with(word))
            fail("invalid value");
        for (std::size_t i = 0; i < word.size(); ++i) advance();
    }

    Node parse_array() {
        advance();  // '['
        Array items;
        while (true) {
            skip_blank();
            if (peek() == ']') {
                advance();
                return Node(std::move(items));
            }
            items.push_back(parse_value());
            skip_blank();
            if (peek() == ',') {
                advance();
                continue;
            }
            if (peek() == ']') {
                advance();
                return Node(std::move(items));
            }
            fail("expected ',' or ']' in array");
        }
    }

    Node parse_inline_table() {
        advance();  // '{'
        std::vector<std::pair<std::string, Node>> entries;
        skip_spaces();
        if (peek() == '}') {
            advance();
            return Node(make_table(std::move(entries)));
        }
        while (true) {
            skip_spaces();
            std::string key = parse_key_part();
            skip_spaces();
            if (peek() != '=') fail("expected '=' after key");
            advance();
            skip_spaces();
            Node value = parse_value();
            for (const auto& entry : entries)
                if (entry.first == key)
                    fail("duplicate key '" + key + "'");
            entries.emplace_back(std::move(key), std::move(value));
            skip_spaces();
            if (peek() == ',') {
                advance();
                continue;
            }
            if (peek() == '}') {
                advance();
                return Node(make_table(std::move(entries)));
            }
            fail("expected ',' or '}' in inline table");
        }
    }

    Node parse_number() {
        std::size_t start = pos_;
        std::string digits;
        if (peek() == '+' || peek() == '-') {
            digits += peek();
            advance();
        }
        while (true) {
            char c = peek();
            if (c >= '0' && c <= '9') {
                digits += c;
                advance();
            } else if (c == '_') {
                advance();
            } else {
                break;
            }
        }
        if (digits.empty() || digits == "+" || digits == "-")
            fail(pos_, "invalid number");

        // Date/time sniffing: 4+ digits followed by '-' or ':' — TOML
        // dates are outside bake's manifest subset.
        if (digits.size() >= 3 && peek() == '-')
            fail(start, "dates are not supported");
        if (digits.size() >= 2 && peek() == ':')
            fail(start, "times are not supported");

        bool is_float = false;
        if (peek() == '.') {
            is_float = true;
            digits += '.';
            advance();
            bool any = false;
            while (true) {
                char c = peek();
                if (c >= '0' && c <= '9') {
                    digits += c;
                    advance();
                    any = true;
                } else if (c == '_') {
                    advance();
                } else {
                    break;
                }
            }
            if (!any) fail(start, "invalid number");
        }
        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            digits += 'e';
            advance();
            if (peek() == '+' || peek() == '-') {
                digits += peek();
                advance();
            }
            bool any = false;
            while (true) {
                char c = peek();
                if (c >= '0' && c <= '9') {
                    digits += c;
                    advance();
                    any = true;
                } else if (c == '_') {
                    advance();
                } else {
                    break;
                }
            }
            if (!any) fail(start, "invalid number");
        }

        if (is_float) {
            double result = 0;
            std::from_chars(digits.data(), digits.data() + digits.size(),
                            result);
            return Node(result);
        }
        std::int64_t result = 0;
        auto [ptr, ec] = std::from_chars(
            digits.data(), digits.data() + digits.size(), result);
        if (ec == std::errc::result_out_of_range)
            fail(start, "integer out of range");
        (void)ptr;
        return Node(result);
    }

    void fail(std::size_t offset, const std::string& message) const {
        std::size_t line = 1;
        std::size_t line_start = 0;
        for (std::size_t i = 0; i < offset && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                line_start = i + 1;
            }
        }
        throw Error(std::string(source_) + ": line " + std::to_string(line) +
                    ", column " + std::to_string(offset - line_start + 1) +
                    ": " + message);
    }

    std::string_view text_;
    std::string_view source_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;
};

}  // namespace

Table parse(std::string_view text, std::string_view source) {
    Parser parser(text, source);
    return parser.run();
}

Table parse_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("failed to open '" + path + "'");
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse(buffer.str(), path);
}

}  // namespace bake::toml
