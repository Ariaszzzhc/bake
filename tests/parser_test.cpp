// parser_test — contract tests for bake.json / bake.toml (stage0
// implementation units). The self-hosted bake links the nlohmann/
// toml++-backed units instead; these tests pin the interface
// semantics both implementations must satisfy.
import std;
import bake.json;
import bake.toml;

namespace {

int failures = 0;

void check(bool ok, std::string_view what) {
    if (!ok) {
        ++failures;
        std::println(std::cerr, "FAIL: {}", what);
    }
}

template <typename Fn>
bool throws_with(Fn&& fn, std::string_view needle) {
    try {
        fn();
    } catch (const std::exception& error) {
        return std::string_view(error.what()).find(needle) !=
               std::string_view::npos;
    }
    return false;
}

// ===== bake.json =====

bool json_parse_and_query() {
    auto doc = bake::json::Value::parse(
        R"({"name": "bake", "count": 42, "ratio": 2.5, "ok": true,)"
        R"( "tags": ["a", "b"], "nested": {"x": [1, 2, 3]}})");

    check(doc.is_object(), "json: root is object");
    check(doc.contains("name"), "json: contains");
    check(!doc.contains("missing"), "json: !contains");
    check(doc.value("name", "") == "bake", "json: value(string)");
    check(doc.value("count", 0) == 42, "json: value(int)");
    check(doc.value("ok", false), "json: value(bool)");
    check(doc.value("missing", "dflt") == "dflt", "json: value fallback");
    check(doc.find("tags") != nullptr, "json: find hit");
    check(doc.find("missing") == nullptr, "json: find miss");
    check(doc["tags"].size() == 2, "json: array size");
    const bake::json::Value& const_doc = doc;
    check(const_doc["missing_key"].is_null(), "json: absent sentinel is null");
    check(doc.find("missing_key") == nullptr,
          "json: const lookup does not insert");

    std::size_t members = 0;
    for (const auto& member : const_doc.items()) ++members;
    check(members == 6, "json: items() count");
    return true;
}

bool json_build_and_dump() {
    bake::json::Value doc;
    doc["name"] = "bake";
    doc["count"] = 42;
    doc["flags"] = std::vector<std::string>{"x", "y"};

    bake::json::Value arr = bake::json::Value::array();
    arr.push_back("one");
    arr.push_back(2);
    doc["list"] = std::move(arr);

    check(doc.dump() ==
          R"({"name":"bake","count":42,"flags":["x","y"],"list":["one",2]})",
          "json: compact dump (insertion order, nlohmann format)");
    check(doc.dump(2) ==
          "{\n"
          "  \"name\": \"bake\",\n"
          "  \"count\": 42,\n"
          "  \"flags\": [\n"
          "    \"x\",\n"
          "    \"y\"\n"
          "  ],\n"
          "  \"list\": [\n"
          "    \"one\",\n"
          "    2\n"
          "  ]\n"
          "}",
          "json: pretty dump(2)");
    // Brace literals.
    bake::json::Value entry = {
        {"alias", bake::json::Value("fmt")},
        {"id", bake::json::Value(7)},
    };
    check(entry.is_object() && entry["alias"].get<std::string>() == "fmt",
          "json: initializer-list object");
    bake::json::Value list = {bake::json::Value(1), bake::json::Value(2)};
    check(list.is_array() && list.size() == 2, "json: initializer-list array");

    // Round-trip: dump → parse → dump is a fixed point.
    auto reparsed = bake::json::Value::parse(doc.dump(2));
    check(reparsed.dump(2) == doc.dump(2), "json: dump/parse round-trip");
    return true;
}

bool json_escapes_and_errors() {
    auto doc = bake::json::Value::parse(
        "{\"s\": \"a\\\"b\\\\c\\nd\\u00e9\\ud83d\\ude00\"}");
    std::string s = doc["s"].get<std::string>();
    check(s == "a\"b\\c\nd\u00e9\U0001F600", "json: escape decoding");

    bake::json::Value with;
    with["s"] = "tab\t\"q\"\x01";
    auto back = bake::json::Value::parse(with.dump());
    check(back["s"].get<std::string>() == "tab\t\"q\"\x01",
          "json: escape round-trip");

    check(throws_with(
              [] { bake::json::Value::parse("{bad"); },
              "bake.json"),
          "json: parse error throws");
    check(throws_with(
              [] { bake::json::Value::parse("[1] trailing"); },
              "trailing"),
          "json: trailing content throws");
    check(throws_with(
              [] {
                  auto v = bake::json::Value::parse("3");
                  v.get<std::string>();
              },
              "expected a string"),
          "json: get type mismatch throws");

    // Duplicate keys: last wins (nlohmann semantics).
    auto dup = bake::json::Value::parse("{\"k\": 1, \"k\": 2}");
    check(dup["k"].get<std::int64_t>() == 2, "json: duplicate key last-wins");
    check(dup.size() == 1, "json: duplicate key single entry");

    // Deep copy: mutating the copy leaves the original alone.
    bake::json::Value original;
    original["a"] = 1;
    bake::json::Value copy = original;
    copy["a"] = 2;
    copy["b"] = 3;
    check(original["a"].get<std::int64_t>() == 1 && !original.contains("b"),
          "json: deep copy independence");
    return true;
}

// ===== bake.toml =====

bool toml_subset_parse() {
    auto tbl = bake::toml::parse(R"toml(
# leading comment
[package]
name = "demo"          # trailing comment
version = '1.2.3'
type = "lib"

[language]
cxx = "c++23"

[target."*-apple-darwin"]
frameworks = [
    "Foundation",   # first
    "AppKit",
]

[dependencies]
fmt = { url = "https://github.com/fmtlib/fmt", tag = "11.1.4" }

[numbers]
opt = 3
pi = 3.14
big = 1_000_000
neg = -7
on = true
off = false
)toml",
                                 "test.toml");

    check(tbl["package"]["name"].value<std::string>() == "demo",
          "toml: basic string");
    check(tbl["package"]["version"].value<std::string>() == "1.2.3",
          "toml: literal string");
    check(tbl["language"]["cxx"].value<std::string>() == "c++23",
          "toml: table access");
    const auto* targets = tbl["target"].as_table();
    check(targets != nullptr, "toml: target table");
    check(targets != nullptr &&
          targets->get("*-apple-darwin") != nullptr,
          "toml: quoted glob header");
    const auto* frameworks =
        tbl["target"]["*-apple-darwin"]["frameworks"].as_array();
    check(frameworks != nullptr && frameworks->size() == 2 &&
          (*frameworks)[1].value<std::string>() == "AppKit",
          "toml: multiline array with comments");
    const auto* fmt = tbl["dependencies"]["fmt"].as_table();
    check(fmt != nullptr &&
          fmt->get("url")->value<std::string>() ==
              "https://github.com/fmtlib/fmt",
          "toml: inline table");
    check(tbl["numbers"]["opt"].value<std::int64_t>() == 3, "toml: int");
    check(tbl["numbers"]["big"].value<std::int64_t>() == 1000000,
          "toml: underscore integer");
    check(tbl["numbers"]["neg"].value<std::int64_t>() == -7, "toml: negative");
    check(tbl["numbers"]["pi"].value<double>() == 3.14, "toml: float");
    check(tbl["numbers"]["on"].value<bool>() == true, "toml: true");
    check(tbl["numbers"]["off"].value<bool>() == false, "toml: false");
    check(tbl["numbers"]["opt"].value<double>() == std::nullopt,
          "toml: value<T> is type-exact");

    // Sentinel semantics on missing keys.
    check(tbl["nope"].as_table() == nullptr, "toml: missing → null table");
    check(tbl["nope"]["deeper"].value<std::string>() == std::nullopt,
          "toml: missing → null value");

    // Table iteration.
    std::size_t packages = 0;
    for (const auto& [key, value] : *tbl["package"].as_table())
        (void)value, ++packages;
    check(packages == 3, "toml: structured-binding iteration");
    return true;
}

bool toml_rejections() {
    check(throws_with(
              [] {
                  bake::toml::parse("[a]\nx = 1\nx = 2\n", "t");
              },
              "duplicate key 'x'"),
          "toml: duplicate key rejected");
    check(throws_with(
              [] {
                  bake::toml::parse("[a]\n[a]\n", "t");
              },
              "defined twice"),
          "toml: duplicate table rejected");
    check(throws_with(
              [] { bake::toml::parse("[[a]]\n", "t"); },
              "arrays of tables"),
          "toml: [[array]] rejected");
    check(throws_with(
              [] { bake::toml::parse("[a]\nd = 2024-01-02\n", "t"); },
              "dates are not supported"),
          "toml: dates rejected");
    check(throws_with(
              [] { bake::toml::parse("a.b = 1\n", "t"); },
              "dotted keys"),
          "toml: dotted keys rejected");
    check(throws_with(
              [] { bake::toml::parse("[a]\ns = \"\"\"x\"\"\"\n", "t"); },
              "multiline strings"),
          "toml: multiline strings rejected");
    check(throws_with(
              [] { bake::toml::parse("[a]\nx = \n", "t"); },
              "expected a value"),
          "toml: missing value rejected");

    // Error carries a position.
    try {
        bake::toml::parse("[a]\nx = 1\nx = 2\n", "err.toml");
        check(false, "toml: expected throw");
    } catch (const std::exception& error) {
        std::string what = error.what();
        check(what.find("line 3") != std::string::npos &&
                  what.find("err.toml") != std::string::npos,
              "toml: error names source and line");
    }
    return true;
}

}  // namespace

int main() {
    json_parse_and_query();
    json_build_and_dump();
    json_escapes_and_errors();
    toml_subset_parse();
    toml_rejections();

    if (failures != 0) {
        std::println(std::cerr, "{} parser test failure(s)", failures);
        return 1;
    }
    std::println("parser tests passed");
    return 0;
}
