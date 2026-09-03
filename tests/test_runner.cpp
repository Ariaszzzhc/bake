// ============================================================
// bake test runner
//
// End-to-end tests that spawn the bake binary on real project
// fixtures.  No external test framework — just a minimal
// assert + report loop that integrates with CTest.
//
// Usage:  test_runner <bake_binary> [test_filter]
// ============================================================

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <chrono>
#include <thread>
#include <stdexcept>

namespace fs = std::filesystem;

// ----------------------------------------------------------------
// Minimal test framework
// ----------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;
static std::string g_current_test;
static std::string g_bake_bin;
static std::string g_fixture_root;

struct TestResult {
    bool ok = true;
    std::string message;
};

#define CHECK(cond, msg) \
    do { if (!(cond)) { return TestResult{false, msg}; } } while(0)

#define CHECK_EQ(a, b, msg) \
    do { if ((a) != (b)) { return TestResult{false, msg}; } } while(0)

using TestFunc = std::function<TestResult()>;

struct TestCase {
    std::string name;
    TestFunc func;
    bool needs_network = false;
};

// ----------------------------------------------------------------
// Utility: run a command, capture exit code + combined output
// ----------------------------------------------------------------

struct CmdResult {
    int exit_code = -1;
    std::string stdout;
    std::string stderr;
    bool success() const { return exit_code == 0; }
};

static CmdResult run_cmd(const std::string& cmd, const fs::path& cwd) {
    // Redirect stderr to stdout so we capture everything.
    std::string full = "cd " + cwd.string() + " && " + cmd + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) return {-1, "", "popen failed"};

    CmdResult result;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result.stdout += buffer;
    }
    result.exit_code = pclose(pipe);
    return result;
}

static CmdResult run_bake(const std::string& args, const fs::path& cwd,
                          const std::string& extra_env = {}) {
    std::string env_prefix = extra_env;
    if (!env_prefix.empty() && env_prefix.back() != ' ') {
        env_prefix += ' ';
    }
    return run_cmd(env_prefix + g_bake_bin + " " + args, cwd);
}

// ----------------------------------------------------------------
// Utility: filesystem helpers
// ----------------------------------------------------------------

static fs::path make_temp_dir(const std::string& name) {
    auto tmp = fs::temp_directory_path() / ("bake-test-" + name);
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    return tmp;
}

static void copy_fixture(const std::string& fixture, const fs::path& dest) {
    fs::path src = fs::path(g_fixture_root) / fixture;
    fs::remove_all(dest);
    fs::create_directories(dest);
    fs::copy(src, dest,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

static void write_file(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { perror("fopen"); return; }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
}

static std::string isolated_cache_environment(const fs::path& home) {
    fs::create_directories(home);
#ifdef _WIN32
    return "LOCALAPPDATA=" + home.string();
#else
    return "HOME=" + home.string();
#endif
}

static fs::path isolated_source_cache(const fs::path& home) {
#ifdef _WIN32
    return home / "bake" / "src";
#else
    return home / ".cache" / "bake" / "src";
#endif
}

static std::string read_file(const fs::path& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};

    std::string content;
    char buffer[4096];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), f)) != 0) {
        content.append(buffer, count);
    }
    fclose(f);
    return content;
}

// Every build, including a native build, is isolated under out/<target>/.
// Most tests build exactly one target and use this helper to avoid baking the
// host triple into fixtures. Tests that intentionally build several targets
// inspect target_output_dirs() directly.
static std::vector<fs::path> target_output_dirs(const fs::path& project_root) {
    std::vector<fs::path> result;
    const fs::path out = project_root / "out";
    if (!fs::is_directory(out)) return result;

    for (const auto& entry : fs::directory_iterator(out)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory() && !name.empty() && name.front() != '.') {
            result.push_back(entry.path());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

static fs::path target_output_dir(const fs::path& project_root) {
    const auto targets = target_output_dirs(project_root);
    if (targets.size() != 1) {
        throw std::runtime_error(
            "expected exactly one out/<target> directory under " +
            (project_root / "out").string() + ", found " +
            std::to_string(targets.size()));
    }
    return targets.front();
}

static fs::path native_executable_path(
        const fs::path& target_out, std::string_view name) {
#ifdef _WIN32
    return target_out / "bin" / (std::string(name) + ".exe");
#else
    return target_out / "bin" / std::string(name);
#endif
}

static std::optional<fs::path> find_moid_declaration(const fs::path& bake_dir) {
    if (!fs::is_directory(bake_dir)) return std::nullopt;
    for (const auto& entry : fs::recursive_directory_iterator(bake_dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string filename = entry.path().filename().string();
        constexpr std::string_view suffix = ".moid.json";
        if (filename.size() >= suffix.size() &&
            filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return entry.path();
        }
    }
    return std::nullopt;
}

static std::optional<std::string> json_scalar_field(
        std::string_view document, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t key_pos = document.find(needle);
    if (key_pos == std::string_view::npos) return std::nullopt;

    const std::size_t colon = document.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return std::nullopt;
    std::size_t pos = document.find_first_not_of(" \t\r\n", colon + 1);
    if (pos == std::string_view::npos) return std::nullopt;

    if (document[pos] != '"') {
        const std::size_t end = document.find_first_of(",}] \t\r\n", pos);
        return std::string(document.substr(pos, end - pos));
    }

    std::string value;
    bool escaped = false;
    for (++pos; pos < document.size(); ++pos) {
        const char c = document[pos];
        if (escaped) {
            switch (c) {
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                default: value += c; break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return value;
        } else {
            value += c;
        }
    }
    return std::nullopt;
}

static bool is_json_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static std::optional<std::string> json_value_field(
        std::string_view document, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t key_pos = 0;
    std::size_t colon = std::string_view::npos;
    while ((key_pos = document.find(needle, key_pos)) != std::string_view::npos) {
        colon = key_pos + needle.size();
        while (colon < document.size() && is_json_whitespace(document[colon])) {
            ++colon;
        }
        if (colon < document.size() && document[colon] == ':') break;
        key_pos += needle.size();
    }
    if (key_pos == std::string_view::npos) return std::nullopt;

    std::size_t start = colon + 1;
    while (start < document.size() && is_json_whitespace(document[start])) {
        ++start;
    }
    if (start == document.size()) return std::nullopt;

    if (document[start] == '"') {
        bool escaped = false;
        for (std::size_t pos = start + 1; pos < document.size(); ++pos) {
            const char c = document[pos];
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                return std::string(document.substr(start, pos - start + 1));
            }
        }
        return std::nullopt;
    }

    if (document[start] == '{' || document[start] == '[') {
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (std::size_t pos = start; pos < document.size(); ++pos) {
            const char c = document[pos];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }

            if (c == '"') {
                in_string = true;
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (--depth == 0) {
                    return std::string(document.substr(start, pos - start + 1));
                }
            }
        }
        return std::nullopt;
    }

    std::size_t end = start;
    while (end < document.size() && document[end] != ',' &&
           document[end] != '}' && document[end] != ']' &&
           !is_json_whitespace(document[end])) {
        ++end;
    }
    if (end == start) return std::nullopt;
    return std::string(document.substr(start, end - start));
}

static std::size_t json_key_count(
        std::string_view document, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = document.find(needle, pos)) != std::string_view::npos) {
        std::size_t after = pos + needle.size();
        while (after < document.size() && is_json_whitespace(document[after])) {
            ++after;
        }
        if (after < document.size() && document[after] == ':') ++count;
        pos += needle.size();
    }
    return count;
}

static std::optional<std::string> declaration_semantics(
        std::string_view document) {
    auto id = json_scalar_field(document, "id");
    auto name = json_scalar_field(document, "name");
    auto version = json_scalar_field(document, "version");
    auto type = json_scalar_field(document, "type");
    auto root = json_scalar_field(document, "root");
    auto cxx_standard = json_scalar_field(document, "cxx_std");
    auto c_standard = json_scalar_field(document, "c_std");
    auto pattern = json_scalar_field(document, "pattern");
    auto visibility = json_scalar_field(document, "visibility");
    auto features = json_value_field(document, "features");
    auto flags = json_value_field(document, "flags");
    auto defines = json_value_field(document, "defines");
    auto include_dirs = json_value_field(document, "include_dirs");
    auto public_include_dirs = json_value_field(document, "public_include_dirs");
    auto libraries = json_value_field(document, "libraries");
    auto frameworks = json_value_field(document, "frameworks");
    auto prebuilt_libs = json_value_field(document, "prebuilt_libs");
    auto dependencies = json_value_field(document, "dependencies");
    const std::size_t source_group_count = json_key_count(document, "pattern");
    if (!id || !name || !version || !type || !root ||
        !cxx_standard || !c_standard || !pattern || !visibility || !features || !flags ||
        !defines || !include_dirs || !public_include_dirs || !libraries ||
        !frameworks || !prebuilt_libs || !dependencies || root->empty() ||
        source_group_count == 0) {
        return std::nullopt;
    }

    return "id=<canonical>;name=" + *name +
           ";version=" + *version + ";type=" + *type +
           ";root=<normalized>;cxx_std=" + *cxx_standard +
           ";c_std=" + *c_standard +
           ";source_groups=" + std::to_string(source_group_count) +
           ";pattern=" + *pattern + ";visibility=" + *visibility +
           ";features=" + *features + ";flags=" + *flags +
           ";defines=" + *defines + ";include_dirs=" + *include_dirs +
           ";public_include_dirs=" + *public_include_dirs +
           ";libraries=" + *libraries + ";frameworks=" + *frameworks +
           ";prebuilt_libs=" + *prebuilt_libs +
           ";dependencies=" + *dependencies;
}

static std::size_t count_occurrences(
        std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

static bool contains_command_token(
        std::string_view text, std::string_view token) {
    std::size_t position = 0;
    while ((position = text.find(token, position)) != std::string_view::npos) {
        const auto is_boundary = [](char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
                   c == '"' || c == '[' || c == ']' || c == ',';
        };
        const bool left = position == 0 || is_boundary(text[position - 1]);
        const std::size_t end = position + token.size();
        const bool right = end == text.size() || is_boundary(text[end]);
        if (left && right) return true;
        position = end;
    }
    return false;
}

static std::string declaration_reader_payload(
        const fs::path& root,
        std::string_view type_member = ",\"type\":\"executable\"",
        std::string_view features = "[]",
        std::string_view sources =
            "[{\"pattern\":\"src/main.cpp\",\"visibility\":\"private\"}]",
        std::string_view link = "{\"libraries\":[],\"frameworks\":[]}",
        std::string_view dependencies = "[]",
        std::string_view prebuilt_libs = "[]") {
    (void)root;
    return std::string("{\"id\":\"$BAKE_MOID_ID\"") +
           ",\"name\":\"declaration-reader-validation\"" +
           ",\"version\":\"0.1.0\"" + std::string(type_member) +
           ",\"root\":\"$BAKE_SOURCE_DIR\"" +
           ",\"cxx_std\":\"c++23\",\"c_std\":\"c17\"" +
           ",\"features\":" + std::string(features) +
           ",\"sources\":" + std::string(sources) +
           ",\"public_include_dirs\":[]" +
           ",\"link\":" + std::string(link) +
           ",\"prebuilt_libs\":" + std::string(prebuilt_libs) +
           ",\"dependencies\":" + std::string(dependencies) +
           ",\"flags\":[]" +
           ",\"defines\":[]" +
           ",\"include_dirs\":[]" +
           ",\"link_flags\":[]}";
}

// ----------------------------------------------------------------
// Test cases
// ----------------------------------------------------------------

// Build a simple executable — bake build must succeed.
TestResult test_simple_app_build() {
    auto dir = make_temp_dir("simple_app");
    copy_fixture("simple_app", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "bake build failed for simple_app: " + r.stdout);

    const fs::path exe = target_output_dir(dir) / "bin" / "simple-app";
    CHECK(fs::exists(exe), "executable not found under out/<target>/bin");

    return {};
}

TestResult test_default_executable_type() {
    auto dir = make_temp_dir("default_executable");
    copy_fixture("default_executable", dir);
    auto result = run_bake("build", dir);
    CHECK(result.success(), result.stdout);
    CHECK(fs::exists(target_output_dir(dir) / "bin/default-executable"),
          "missing default executable output");
    return {};
}

TestResult test_invalid_moid_type() {
    auto dir = make_temp_dir("invalid_moid_type");
    copy_fixture("invalid_moid_type", dir);
    auto result = run_bake("build", dir);
    CHECK(!result.success(), "invalid Moid type unexpectedly succeeded");
    CHECK(result.stdout.find("unknown moid type 'static-lib'") != std::string::npos,
          "missing strict Moid type diagnostic: " + result.stdout);
    return {};
}

TestResult test_input_declaration_equivalence() {
    auto dir = make_temp_dir("input_declaration_equivalence");
    auto default_dir = dir / "default_discovery";
    auto build_cpp_dir = dir / "build_cpp";
    copy_fixture("declaration_equivalence/default_discovery", default_dir);
    copy_fixture("declaration_equivalence/build_cpp", build_cpp_dir);

    auto default_build = run_bake("build", default_dir);
    auto build_cpp_build = run_bake("build", build_cpp_dir);
    CHECK(default_build.success(),
          "default input declaration build failed: " + default_build.stdout);

    auto default_path = find_moid_declaration(
        target_output_dir(default_dir) / ".bake");
    CHECK(default_path.has_value(),
          "default input discovery did not persist a .moid.json declaration");
    const std::string default_json = read_file(*default_path);
    CHECK_EQ(json_scalar_field(default_json, "name").value_or("<missing>"),
             std::string("declaration-equivalence"),
             "default input declaration has the wrong name");
    CHECK_EQ(json_scalar_field(default_json, "type").value_or("<missing>"),
             std::string("executable"),
             "default input declaration did not normalize the default type");
    CHECK_EQ(json_key_count(default_json, "pattern"), std::size_t(1),
             "default input declaration must contain exactly one source group");
    CHECK_EQ(json_scalar_field(default_json, "pattern").value_or("<missing>"),
             std::string("src/main.cpp"),
             "default input declaration has the wrong source pattern");
    CHECK_EQ(json_scalar_field(default_json, "visibility").value_or("<missing>"),
             std::string("private"),
             "default input declaration has the wrong source visibility");
    CHECK_EQ(json_value_field(default_json, "features").value_or("<missing>"),
             std::string("[]"), "default input declaration features are not empty");
    CHECK(json_value_field(default_json, "flags").has_value(),
          "default input declaration has no source flags field");
    CHECK(json_value_field(default_json, "defines").has_value(),
          "default input declaration has no source defines field");
    CHECK(json_value_field(default_json, "include_dirs").has_value(),
          "default input declaration has no source include_dirs field");
    CHECK_EQ(json_value_field(default_json, "public_include_dirs").value_or("<missing>"),
             std::string("[]"),
             "default public_include_dirs are not empty");
    CHECK_EQ(json_value_field(default_json, "libraries").value_or("<missing>"),
             std::string("[]"), "default input libraries are not empty");
    CHECK_EQ(json_value_field(default_json, "frameworks").value_or("<missing>"),
             std::string("[]"), "default input frameworks are not empty");
    CHECK_EQ(json_value_field(default_json, "prebuilt_libs").value_or("<missing>"),
             std::string("[]"), "default input prebuilt_libs are not empty");
    CHECK_EQ(json_value_field(default_json, "dependencies").value_or("<missing>"),
             std::string("[]"), "default input dependencies are not empty");

    CHECK(build_cpp_build.success(),
          "build.cpp input declaration failed: " + build_cpp_build.stdout);
    auto build_cpp_path = find_moid_declaration(
        target_output_dir(build_cpp_dir) / ".bake");
    CHECK(build_cpp_path.has_value(),
          "build.cpp input declaration did not persist a .moid.json file");
    const std::string build_cpp_json = read_file(*build_cpp_path);
    CHECK_EQ(json_scalar_field(build_cpp_json, "name").value_or("<missing>"),
             std::string("declaration-equivalence"),
             "build.cpp input declaration has the wrong name");
    CHECK_EQ(json_scalar_field(build_cpp_json, "type").value_or("<missing>"),
             std::string("executable"),
             "build.cpp input declaration did not normalize the default type");
    CHECK_EQ(json_key_count(build_cpp_json, "pattern"), std::size_t(1),
             "build.cpp input declaration must contain exactly one source group");
    CHECK_EQ(json_scalar_field(build_cpp_json, "pattern").value_or("<missing>"),
             std::string("src/main.cpp"),
             "build.cpp input declaration has the wrong source pattern");
    CHECK_EQ(json_scalar_field(build_cpp_json, "visibility").value_or("<missing>"),
             std::string("private"),
             "build.cpp input declaration has the wrong source visibility");
    CHECK_EQ(json_value_field(build_cpp_json, "features").value_or("<missing>"),
             std::string("[]"), "build.cpp declaration features are not empty");
    CHECK(json_value_field(build_cpp_json, "flags").has_value(),
          "build.cpp declaration has no source flags field");
    CHECK(json_value_field(build_cpp_json, "defines").has_value(),
          "build.cpp declaration has no source defines field");
    CHECK(json_value_field(build_cpp_json, "include_dirs").has_value(),
          "build.cpp declaration has no source include_dirs field");
    CHECK_EQ(json_value_field(build_cpp_json, "public_include_dirs").value_or("<missing>"),
             std::string("[]"), "build.cpp public_include_dirs are not empty");
    CHECK_EQ(json_value_field(build_cpp_json, "libraries").value_or("<missing>"),
             std::string("[]"), "build.cpp input libraries are not empty");
    CHECK_EQ(json_value_field(build_cpp_json, "frameworks").value_or("<missing>"),
             std::string("[]"), "build.cpp input frameworks are not empty");
    CHECK_EQ(json_value_field(build_cpp_json, "prebuilt_libs").value_or("<missing>"),
             std::string("[]"), "build.cpp prebuilt_libs are not empty");
    CHECK_EQ(json_value_field(build_cpp_json, "dependencies").value_or("<missing>"),
             std::string("[]"), "build.cpp dependencies are not empty");

    const auto default_id = json_scalar_field(default_json, "id");
    const auto build_cpp_id = json_scalar_field(build_cpp_json, "id");
    CHECK(default_id && default_id->rfind("path:", 0) == 0,
          "default input declaration did not use a canonical path identity");
    CHECK(build_cpp_id && build_cpp_id->rfind("path:", 0) == 0,
          "build.cpp declaration did not use a canonical path identity");
    CHECK(*default_id != *build_cpp_id,
          "distinct canonical source roots unexpectedly share an identity");

    auto default_fields = declaration_semantics(default_json);
    auto build_cpp_fields = declaration_semantics(build_cpp_json);
    CHECK(default_fields.has_value(),
          "default input declaration is missing required semantic fields: " +
              default_json);
    CHECK(build_cpp_fields.has_value(),
          "build.cpp declaration is missing required semantic fields: " +
              build_cpp_json);
    CHECK_EQ(*default_fields, *build_cpp_fields,
             "default-discovered and build.cpp-declared inputs differ semantically");

    return {};
}


TestResult test_declaration_json_escape() {
    auto dir = make_temp_dir("declaration_json_escape");
    copy_fixture("declaration_json_escape", dir);

    auto result = run_bake("build", dir);
    CHECK(result.success(),
          "build.cpp declaration with control characters failed: " + result.stdout);

    auto path = find_moid_declaration(target_output_dir(dir) / ".bake");
    CHECK(path.has_value(), "control-character declaration was not persisted");
    const std::string declaration = read_file(*path);
    // Control characters must be escaped as JSON sequences, not present as
    // literal bytes. Check for the escaped form and absence of raw bytes.
    CHECK(declaration.find("\"include\\b\\f\\u0001\"") != std::string::npos,
          "declaration did not use RFC 8259 control-character escapes");
    CHECK(declaration.find('\b') == std::string::npos &&
              declaration.find('\f') == std::string::npos &&
              declaration.find('\x01') == std::string::npos,
          "declaration contains an unescaped JSON control byte");
    CHECK(fs::exists(target_output_dir(dir) / "bin/declaration-json-escape"),
          "control-character fixture executable was not produced");
    return {};
}

TestResult test_declaration_reader_validation() {
    auto dir = make_temp_dir("declaration_reader_validation");
    copy_fixture("declaration_reader_validation", dir);

    constexpr std::string_view type = ",\"type\":\"executable\"";
    constexpr std::string_view sources =
        "[{\"pattern\":\"src/main.cpp\",\"visibility\":\"private\"}]";
    constexpr std::string_view link =
        "{\"libraries\":[],\"frameworks\":[]}";

    auto replace_marker = [](std::string payload,
                             std::string_view marker,
                             std::string_view value) {
        const auto position = payload.find(marker);
        if (position != std::string::npos)
            payload.replace(position, marker.size(), value);
        return payload;
    };
    const std::string wrong_id = replace_marker(
        declaration_reader_payload(dir), "$BAKE_MOID_ID", "wrong-id");
    const std::string wrong_root = replace_marker(
        declaration_reader_payload(dir), "$BAKE_SOURCE_DIR", "/wrong-root");
    const std::string wrong_version = replace_marker(
        declaration_reader_payload(dir),
        "\"version\":\"0.1.0\"", "\"version\":\"9.9.9\"");

    struct InvalidPayload {
        std::string name;
        std::string document;
        std::string diagnostic;
    };
    const std::vector<InvalidPayload> invalid = {
        {"type type", declaration_reader_payload(
             dir, ",\"type\":17"),
         "moid declaration field 'type' must be a string"},
        {"unknown type", declaration_reader_payload(
             dir, ",\"type\":\"unknown\""),
         "unknown moid type 'unknown'"},
        {"features type", declaration_reader_payload(
             dir, type, "\"not-an-array\""),
         "moid declaration field 'features' must be an array"},
        {"sources type", declaration_reader_payload(
             dir, type, "[]", "{}"),
         "moid declaration field 'sources' must be an array"},
        {"source field type", declaration_reader_payload(
             dir, type, "[]",
             "[{\"pattern\":17,\"visibility\":\"private\"}]"),
         "moid declaration field 'sources[0].pattern' must be a string"},
        {"link type", declaration_reader_payload(
             dir, type, "[]", sources, "[]"),
         "moid declaration field 'link' must be an object"},
        {"dependencies type", declaration_reader_payload(
             dir, type, "[]", sources, link, "{}"),
         "moid declaration field 'dependencies' must be an array"},
        {"prebuilt_libs type", declaration_reader_payload(
             dir, type, "[]", sources, link, "[]", "{}"),
         "moid declaration field 'prebuilt_libs' must be an array"},
        {"prebuilt_libs entry type", declaration_reader_payload(
             dir, type, "[]", sources, link, "[]", "[17]"),
         "moid declaration field 'prebuilt_libs[0]' must be a string"},
        {"duplicate dependency alias", declaration_reader_payload(
             dir, type, "[]", sources, link,
            "[{\"alias\":\"dup\",\"id\":\"first\",\"features\":[]},"
            "{\"alias\":\"dup\",\"id\":\"second\",\"features\":[]}]"),
         "duplicate moid dependency alias 'dup'"},
        {"resolved id mismatch", wrong_id,
         "moid declaration field 'id' does not match resolved identity"},
        {"resolved root mismatch", wrong_root,
         "moid declaration field 'root' does not match resolved root"},
        {"resolved version mismatch", wrong_version,
         "moid declaration field 'version' does not match resolved version"},
        {"resolved features mismatch", declaration_reader_payload(
             dir, type, "[\"extra\"]"),
         "moid declaration field 'features' does not match "
         "resolved features"},
        {"resolved dependencies mismatch", declaration_reader_payload(
             dir, type, "[]", sources, link,
             "[{\"alias\":\"dup\",\"id\":\"first\",\"features\":[]}]"),
         "moid declaration field 'dependencies' does not "
         "match resolved dependencies"},
    };

    std::string failures;
    for (const auto& invalid_payload : invalid) {
        write_file(dir / "payload.json", invalid_payload.document);
        auto result = run_bake("build", dir);
        if (result.success()) {
            failures += invalid_payload.name + " payload unexpectedly succeeded\n";
        } else if (result.stdout.find(invalid_payload.diagnostic) ==
                   std::string::npos) {
            failures += invalid_payload.name + " diagnostic mismatch: " +
                        result.stdout + "\n";
        }
    }

    const std::string missing_type = declaration_reader_payload(dir, "");
    write_file(dir / "payload.json", missing_type);
    auto defaulted = run_bake("build", dir);
    if (!defaulted.success()) {
        failures += "missing declaration type did not default to executable: " +
                    defaulted.stdout + "\n";
    }
    const auto outputs = target_output_dirs(dir);
    if (outputs.size() != 1) {
        failures += "missing-type build did not create one out/<target> directory\n";
    } else {
        const fs::path executable =
            outputs.front() / "bin/declaration-reader-validation";
        auto persisted = find_moid_declaration(outputs.front() / ".bake");
        if (!persisted) {
            failures += "missing-type declaration was not persisted\n";
        } else if (json_scalar_field(read_file(*persisted), "type")
                       .value_or("<missing>") != "executable") {
            failures += "persisted missing-type declaration did not default to executable type\n";
        }
        if (!fs::exists(executable)) {
            failures += "missing declaration type was not normalized to executable\n";
        }
    }

    CHECK(failures.empty(), failures);
    return {};
}

// Build a static library — bake build must produce a .a file.
TestResult test_static_lib_build() {
    auto dir = make_temp_dir("static_lib");
    copy_fixture("static_lib", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "bake build failed for static_lib: " + r.stdout);

    // Verify the library exists in out/<target>/lib/.
    bool found = false;
    fs::path lib_dir = target_output_dir(dir) / "lib";
    if (fs::exists(lib_dir)) {
        for (auto& e : fs::directory_iterator(lib_dir)) {
            if (e.path().extension() == ".a") { found = true; break; }
        }
    }
    CHECK(found, "static library (.a) not found in out/<target>/lib/");

    return {};
}

// Build a genuine C17 executable. C-only syntax keeps this from silently
// passing when a C++ driver treats .c input as C++.
TestResult test_pure_c_build() {
    auto dir = make_temp_dir("pure_c");
    copy_fixture("pure_c", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "bake build failed for pure C project: " + r.stdout);

    const fs::path out = target_output_dir(dir);
    fs::path exe = out / "bin" / "pure-c";
    CHECK(fs::exists(exe),
          "pure C executable not found under out/<target>/bin");

    auto run = run_cmd(exe.string(), dir);
    CHECK_EQ(run.exit_code, 0,
             "pure C executable returned non-zero: " + std::to_string(run.exit_code));

    auto compile_commands = read_file(dir / "compile_commands.json");
    CHECK(compile_commands.find("-std=c17") != std::string::npos,
          "pure C compile command does not select C17: " + compile_commands);

    int same_stem_objects = 0;
    for (const auto& entry : fs::recursive_directory_iterator(out / ".obj")) {
        const std::string filename = entry.path().filename().string();
        if (filename.find("value_") == 0 && entry.path().extension() == ".o") {
            ++same_stem_objects;
        }
    }
    CHECK_EQ(same_stem_objects, 2,
             "default input discovery overwrote same-named source objects");

    return {};
}

// Build a project with path dependencies — include dirs must resolve.
TestResult test_path_dep_build() {
    auto dir = make_temp_dir("path_dep");
    copy_fixture("path_dep", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "bake build failed for path_dep: " + r.stdout);

    // Verify the executable exists and runs correctly
    fs::path exe = target_output_dir(dir) / "bin" / "app";
    CHECK(fs::exists(exe), "executable not found under out/<target>/bin");

    // Run it — should return 0 (add(2,3) - 5 == 0)
    auto run = run_cmd(exe.string(), dir);
    CHECK_EQ(run.exit_code, 0, "app-with-dep returned non-zero: " + std::to_string(run.exit_code));

    return {};
}

TestResult test_moid_outputs() {
    auto dir = make_temp_dir("moid_outputs");
    copy_fixture("moid_outputs", dir);

    auto build = run_bake("build -j 1", dir);
    CHECK(build.success(),
          "four-Moid output build failed: " + build.stdout);

    const fs::path out = target_output_dir(dir);
    std::vector<fs::path> base_objects;
    for (const auto& entry :
         fs::recursive_directory_iterator(out / ".obj")) {
        if (!entry.is_regular_file()) continue;
        const std::string filename = entry.path().filename().string();
        if (entry.path().extension() == ".o" &&
            filename.find("base_") == 0) {
            base_objects.push_back(entry.path());
        }
    }
    CHECK_EQ(base_objects.size(), std::size_t(1),
             "lib Moid must produce exactly one canonical base object");

    const fs::path lib_dir = out / "lib";

#ifdef _WIN32
    const fs::path base_archive = lib_dir / "base.lib";
    const fs::path archive = lib_dir / "archive.lib";
    const fs::path shared = lib_dir / "shared.dll";
#else
    const fs::path base_archive = lib_dir / "libbase.a";
    const fs::path archive = lib_dir / "libarchive.a";
#ifdef __APPLE__
    const fs::path shared = lib_dir / "libshared.dylib";
#else
    const fs::path shared = lib_dir / "libshared.so";
#endif
#endif
    const fs::path executable =
#ifdef _WIN32
        out / "bin" / "app.exe";
#else
        out / "bin" / "app";
#endif

    CHECK(fs::is_regular_file(base_archive),
          "lib archive artifact is missing: " + base_archive.string());
    CHECK(fs::is_regular_file(archive),
          "lib terminal artifact is missing: " + archive.string());
    CHECK(fs::is_regular_file(shared),
          "dylib terminal artifact is missing: " + shared.string());
    CHECK(fs::is_regular_file(executable),
          "default executable terminal artifact is missing");

    auto run = run_cmd(executable.string(), dir);
    CHECK_EQ(run.exit_code, 0,
             "app did not resolve base/archive/shared symbols: " + run.stdout);

    const std::string graph = read_file(out / ".bake" / "graph.json");
    CHECK(!graph.empty(), "moid_outputs did not persist graph.json");

    auto action_for_in = [](const std::string& document,
                            std::string_view moid,
                            std::string_view type)
            -> std::optional<std::string> {
        const std::string needle =
            "\"moid\": \"" + std::string(moid) + "\"";
        std::size_t position = 0;
        while ((position = document.find(needle, position)) !=
               std::string::npos) {
            const std::size_t begin = document.rfind('{', position);
            const std::size_t end = document.find('}', position);
            if (begin == std::string::npos || end == std::string::npos) {
                return std::nullopt;
            }
            std::string action = document.substr(begin, end - begin + 1);
            if (json_scalar_field(action, "moid") == moid &&
                json_scalar_field(action, "type") == type) {
                return action;
            }
            position += needle.size();
        }
        return std::nullopt;
    };
    auto action_for = [&](std::string_view moid, std::string_view type) {
        return action_for_in(graph, moid, type);
    };

    auto json_string_array = [](std::string_view array) {
        std::vector<std::string> values;
        for (std::size_t position = 0; position < array.size();) {
            if (array[position] != '"') {
                ++position;
                continue;
            }
            std::string value;
            bool escaped = false;
            for (++position; position < array.size(); ++position) {
                const char c = array[position];
                if (escaped) {
                    value += c;
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    ++position;
                    break;
                } else {
                    value += c;
                }
            }
            values.push_back(std::move(value));
        }
        return values;
    };

    auto occurrences = [](const std::vector<std::string>& values,
                          const std::string& expected) {
        std::size_t count = 0;
        for (const auto& value : values) {
            if (value == expected) ++count;
        }
        return count;
    };

    auto base_archive_action = action_for("base", "archive");
    CHECK(base_archive_action.has_value(),
          "base lib archive action is missing from graph.json");
    CHECK(!action_for("base", "link").has_value(),
          "lib Moid must not have a link action");

    auto base_compile = action_for("base", "compile");
    auto archive_compile = action_for("archive", "compile");
    auto shared_compile = action_for("shared", "compile");
    auto app_compile = action_for("app", "compile");
    auto archive_action = action_for("archive", "archive");
    auto shared_action = action_for("shared", "link");
    auto app_action = action_for("app", "link");
    CHECK(base_compile.has_value() && archive_compile.has_value() &&
              shared_compile.has_value() && app_compile.has_value(),
          "one or more Moid compile actions are missing from graph.json");
    CHECK(archive_action.has_value(), "archive action is missing from graph.json");
    CHECK(shared_action.has_value(), "shared link action is missing from graph.json");
    CHECK(app_action.has_value(), "app link action is missing from graph.json");

    const auto base_compile_id = json_scalar_field(*base_compile, "id");
    const auto archive_compile_id = json_scalar_field(*archive_compile, "id");
    const auto shared_compile_id = json_scalar_field(*shared_compile, "id");
    const auto app_compile_id = json_scalar_field(*app_compile, "id");
    const auto base_archive_action_id = json_scalar_field(*base_archive_action, "id");
    const auto archive_action_id = json_scalar_field(*archive_action, "id");
    const auto shared_action_id = json_scalar_field(*shared_action, "id");
    CHECK(base_compile_id.has_value() && archive_compile_id.has_value() &&
              shared_compile_id.has_value() && app_compile_id.has_value() &&
              base_archive_action_id.has_value() &&
              archive_action_id.has_value() && shared_action_id.has_value(),
          "one or more graph actions have no id");

    auto archive_inputs_json = json_value_field(*archive_action, "inputs");
    CHECK(archive_inputs_json.has_value(), "archive action has no inputs array");
    const auto archive_inputs = json_string_array(*archive_inputs_json);
    CHECK_EQ(archive_inputs.size(), std::size_t(1),
             "archive must contain only its own object");

    for (const auto& input : archive_inputs) {
        const fs::path member(input);
        CHECK(member.extension() == ".o" || member.extension() == ".obj",
              "archive contains a non-object member: " + input);
    }

    auto archive_depends_json = json_value_field(*archive_action, "depends_on");
    CHECK(archive_depends_json.has_value(),
          "archive action has no depends_on array");
    const auto archive_depends = json_string_array(*archive_depends_json);
    CHECK_EQ(archive_depends.size(), std::size_t(1),
             "archive must depend on exactly its own object producer");
    CHECK_EQ(occurrences(archive_depends, *archive_compile_id), std::size_t(1),
             "archive must depend once on its own object producer");

    auto shared_inputs_json = json_value_field(*shared_action, "inputs");
    CHECK(shared_inputs_json.has_value(), "shared link action has no inputs array");
    const auto shared_inputs = json_string_array(*shared_inputs_json);
    std::size_t shared_archive_count = 0;
    std::size_t shared_base_archive_count = 0;
    for (const auto& input : shared_inputs) {
        if (fs::path(input).filename() == archive.filename()) {
            ++shared_archive_count;
        }
        if (fs::path(input).filename() == base_archive.filename()) {
            ++shared_base_archive_count;
        }
    }
    CHECK_EQ(shared_archive_count, std::size_t(1),
             "shared library must consume the archive exactly once");
    CHECK_EQ(shared_base_archive_count, std::size_t(1),
             "shared library must consume the base archive exactly once");

    auto shared_depends_json = json_value_field(*shared_action, "depends_on");
    CHECK(shared_depends_json.has_value(),
          "shared link action has no depends_on array");
    const auto shared_depends = json_string_array(*shared_depends_json);
    CHECK_EQ(shared_depends.size(), std::size_t(3),
             "shared link must depend on its object and both archive producers");
    CHECK_EQ(occurrences(shared_depends, *shared_compile_id), std::size_t(1),
             "shared link must depend once on its object producer");
    CHECK_EQ(occurrences(shared_depends, *archive_action_id), std::size_t(1),
             "shared link must depend once on the archive producer");
    CHECK_EQ(occurrences(shared_depends, *base_archive_action_id), std::size_t(1),
             "shared link must depend once on the base archive producer");

    auto app_inputs_json = json_value_field(*app_action, "inputs");
    CHECK(app_inputs_json.has_value(), "app link action has no inputs array");
    const auto app_inputs = json_string_array(*app_inputs_json);
    auto app_outputs_json = json_value_field(*app_compile, "outputs");
    CHECK(app_outputs_json.has_value(), "app compile action has no outputs array");
    const auto app_outputs = json_string_array(*app_outputs_json);
    CHECK_EQ(app_outputs.size(), std::size_t(1),
             "app compile action must produce exactly one object");

    std::size_t archive_count = 0;
    std::size_t shared_count = 0;
    std::size_t base_archive_count = 0;
    std::size_t object_count = 0;
    std::size_t own_object_count = 0;
    std::size_t archive_position = app_inputs.size();
    std::size_t shared_position = app_inputs.size();
    for (std::size_t index = 0; index < app_inputs.size(); ++index) {
        const fs::path item(app_inputs[index]);
        if (item.extension() == ".o" || item.extension() == ".obj") {
            ++object_count;
            if (item.lexically_normal() ==
                fs::path(app_outputs.front()).lexically_normal()) {
                ++own_object_count;
            }
        }
        if (item.filename() == archive.filename()) {
            ++archive_count;
            archive_position = index;
        }
        if (item.filename() == shared.filename()) {
            ++shared_count;
            shared_position = index;
        }
        if (item.filename() == base_archive.filename()) {
            ++base_archive_count;
        }
    }
    CHECK_EQ(object_count, std::size_t(1),
             "app link must not receive dependency objects");
    CHECK_EQ(own_object_count, std::size_t(1),
             "app link must consume its own object exactly once");
    CHECK_EQ(archive_count, std::size_t(1),
             "app must consume the archive exactly once");
    CHECK_EQ(base_archive_count, std::size_t(1),
             "app must consume the base archive exactly once");
    CHECK_EQ(shared_count, std::size_t(1),
             "app must consume the shared library exactly once");
    CHECK(shared_position < archive_position,
          "dependency libraries must preserve consumer-before-provider order");

    auto app_depends_json = json_value_field(*app_action, "depends_on");
    CHECK(app_depends_json.has_value(), "app link action has no depends_on array");
    const auto app_depends = json_string_array(*app_depends_json);
    CHECK_EQ(app_depends.size(), std::size_t(4),
             "app link must depend on its object producer and all terminal producers");
    CHECK_EQ(occurrences(app_depends, *app_compile_id), std::size_t(1),
             "app link must depend once on its object producer");
    CHECK_EQ(occurrences(app_depends, *shared_action_id), std::size_t(1),
             "app link must depend once on the shared-library producer");
    CHECK_EQ(occurrences(app_depends, *archive_action_id), std::size_t(1),
             "app link must depend once on the archive producer");
    CHECK_EQ(occurrences(app_depends, *base_archive_action_id), std::size_t(1),
             "app link must depend once on the base archive producer");

    // Independent private modules use owner-scoped logical names. They must
    // not conflict merely because two workspace nodes choose the same name.
    auto module_dir = make_temp_dir("moid_outputs_private_modules");
    copy_fixture("moid_outputs", module_dir);
    write_file(module_dir / "base" / "bake.toml",
        "[package]\n"
        "name = \"base\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\ncxx = \"c++23\"\n");
    write_file(module_dir / "shared" / "bake.toml",
        "[package]\n"
        "name = \"shared\"\n"
        "version = \"0.1.0\"\n"
        "type = \"dylib\"\n"
        "[language]\ncxx = \"c++23\"\n\n"
        "[dependencies]\n"
        "archive = { path = \"../archive\" }\n");
    write_file(module_dir / "app" / "bake.toml",
        "[package]\n"
        "name = \"app\"\n"
        "version = \"0.1.0\"\n"
        "[language]\ncxx = \"c++23\"\n\n"
        "[dependencies]\n"
        "shared = { path = \"../shared\" }\n");
    write_file(module_dir / "base" / "src" / "private.cppm",
        "export module duplicate.detail;\n"
        "export int base_private() { return 1; }\n");
    write_file(module_dir / "shared" / "src" / "private.cppm",
        "export module duplicate.detail;\n"
        "export int shared_private() { return 2; }\n");
    write_file(module_dir / "base" / "public" / "api.cppm",
        "export module base.api;\n"
        "export inline int module_value() { return 9; }\n");
    write_file(module_dir / "app" / "src" / "main.cpp",
        "import base.api;\n"
        "#include \"archive.hpp\"\n"
        "#include \"base.hpp\"\n"
        "#include \"shared.hpp\"\n\n"
        "int main() {\n"
        "    return base_value() == 7 && archive_value() == 12 &&\n"
        "                   shared_value() == 30 && module_value() == 9\n"
        "        ? 0\n"
        "        : 1;\n"
        "}\n");

    auto private_modules = run_bake("build -j 1", module_dir);
    CHECK(private_modules.success(),
          "independent private module names conflicted: " +
              private_modules.stdout);

    const fs::path module_out = target_output_dir(module_dir);
    auto module_run = run_cmd(
#ifdef _WIN32
        (module_out / "bin" / "app.exe").string(),
#else
        (module_out / "bin" / "app").string(),
#endif
        module_dir);
    CHECK_EQ(module_run.exit_code, 0,
             "transitive dylib usage executable returned non-zero");

    const std::string module_graph =
        read_file(module_out / ".bake" / "graph.json");
    auto module_app_action = action_for_in(module_graph, "app", "link");
    CHECK(module_app_action.has_value(),
          "module fixture app link action is missing from graph.json");
    auto module_app_inputs_json =
        json_value_field(*module_app_action, "inputs");
    CHECK(module_app_inputs_json.has_value(),
          "module fixture app link action has no inputs array");
    const auto module_app_inputs =
        json_string_array(*module_app_inputs_json);
    std::size_t module_archive_count = 0;
    std::size_t module_shared_count = 0;
    for (const auto& input : module_app_inputs) {
        const auto filename = fs::path(input).filename();
        if (filename == archive.filename()) ++module_archive_count;
        if (filename == shared.filename()) ++module_shared_count;
    }
    CHECK_EQ(module_archive_count, std::size_t(0),
             "dylib leaked its consumed archive downstream");
    CHECK_EQ(module_shared_count, std::size_t(1),
             "app must consume the shared terminal exactly once");

    std::size_t duplicate_pcms = 0;
    for (const auto& entry : fs::recursive_directory_iterator(
             module_out / ".bmi")) {
        if (entry.is_regular_file() &&
            entry.path().filename() == "duplicate.detail.pcm") {
            ++duplicate_pcms;
        }
    }
    CHECK_EQ(duplicate_pcms, std::size_t(2),
             "private modules did not retain canonical owner storage");

    return {};
}

TestResult test_missing_path_dependency() {
    auto dir = make_temp_dir("missing_path_dependency");
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"missing-path-app\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[dependencies]\n"
        "missing = { path = \"deps/does-not-exist\" }\n");
    write_file(dir / "src/main.c", "int main(void) { return 0; }\n");

    auto result = run_bake("build -j 1", dir);
    CHECK(!result.success(),
          "nonexistent path dependency was silently ignored");
    CHECK(result.stdout.find("path dependency 'missing'") != std::string::npos &&
              result.stdout.find("deps/does-not-exist") != std::string::npos &&
              result.stdout.find("does not exist") != std::string::npos,
          "missing path dependency lacked alias and path: " + result.stdout);

    return {};
}

// bake build --locked must work for path-dep-only projects (no lockfile needed).
TestResult test_path_dep_locked() {
    auto dir = make_temp_dir("path_dep_locked");
    copy_fixture("path_dep", dir);

    auto r = run_bake("build --locked", dir);
    CHECK(r.success(), "--locked failed for path-dep-only project: " + r.stdout);

    return {};
}

// bake build --frozen must fail when no lockfile exists for a remote dep.
TestResult test_frozen_no_lock() {
    auto dir = make_temp_dir("frozen_no_lock");
    copy_fixture("simple_app", dir);

    // Add a fake remote dependency
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"frozen-test\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n\n"
        "[dependencies]\n"
        "fake = { url = \"https://github.com/example/fake\", tag = \"v1.0\" }\n"
    );

    auto r = run_bake("build --frozen", dir);
    CHECK(!r.success(), "--frozen should have failed without lockfile");
    bool has_error = r.stdout.find("stale") != std::string::npos ||
                     r.stdout.find("offline") != std::string::npos;
    CHECK(has_error, "expected 'stale' or 'offline' in error output: " + r.stdout);

    return {};
}

// is_consistent() must reject a lockfile with empty commit/hashes.
TestResult test_lock_consistency() {
    auto dir = make_temp_dir("lock_consistency");
    copy_fixture("simple_app", dir);

    // Add a remote dependency to the manifest
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"lock-test\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n\n"
        "[dependencies]\n"
        "fake = { url = \"https://github.com/example/fake\", tag = \"v1.0\" }\n"
    );

    // Write a lockfile with matching url+tag but EMPTY commit/hashes
    write_file(dir / "bake.lock",
        "# AUTO-GENERATED. Do not edit.\n\n"
        "[root_deps]\n"
        "fake = \"fake-v1.0\"\n\n"
        "[nodes.\"fake-v1.0\"]\n"
        "url              = \"https://github.com/example/fake\"\n"
        "tag              = \"v1.0\"\n"
        "commit           = \"\"\n"
        "transport_sha256 = \"\"\n"
        "tree_sha256      = \"\"\n"
        "native           = false\n"
        "dependencies     = []\n"
    );

    auto r = run_bake("build --locked", dir);
    CHECK(!r.success(), "--locked should reject lock with empty commit");
    CHECK(r.stdout.find("stale") != std::string::npos,
          "expected 'stale' in error: " + r.stdout);

    return {};
}

// bake add with an identical existing entry skips with a hint
// (OverwriteIfExplicit); it never duplicates or errors.
TestResult test_add_duplicate() {
    auto dir = make_temp_dir("add_duplicate");
    copy_fixture("simple_app", dir);

    // First add should succeed
    auto r1 = run_bake("add https://github.com/fmtlib/fmt --tag 10.2.1 fmt", dir);
    CHECK(r1.success(), "first add failed: " + r1.stdout);

    // Identical re-add skips with a hint
    auto r2 = run_bake("add https://github.com/fmtlib/fmt --tag 10.2.1 fmt", dir);
    CHECK(r2.success(), "identical duplicate add should skip, not fail");
    CHECK(r2.stdout.find("already a dependency") != std::string::npos,
          "expected 'already a dependency' message: " + r2.stdout);
    auto manifest = read_file(dir / "bake.toml");
    CHECK(manifest.find("fmtlib/fmt") != std::string::npos,
          "dependency should still be present");

    return {};
}

// A ref flag without a value is treated as unset: the dependency pins the
// remote's default branch (resolved at first build).
TestResult test_add_no_tag() {
    auto dir = make_temp_dir("add_no_tag");
    copy_fixture("simple_app", dir);

    auto r = run_bake("add https://github.com/fmtlib/fmt --tag", dir);
    CHECK(r.success(), "add with empty --tag should add a default-branch dep");
    auto manifest = read_file(dir / "bake.toml");
    CHECK(manifest.find("fmt = { url = \"https://github.com/fmtlib/fmt\" }") !=
              std::string::npos,
          "default-branch entry should carry no ref: " + manifest);

    return {};
}

// Every build, including native, must write into one target-triple directory.
TestResult test_target_output_layout() {
    auto dir = make_temp_dir("target_output");
    copy_fixture("simple_app", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "build failed: " + r.stdout);

    CHECK(fs::is_directory(dir / "out"), "out/ directory not created");
    const auto targets = target_output_dirs(dir);
    CHECK_EQ(targets.size(), std::size_t(1),
             "native build must create exactly one out/<target> directory");
    const fs::path out = targets.front();
    CHECK_EQ(count_occurrences(out.filename().string(), "-"), std::size_t(2),
             "native output directory must use a three-segment target triple");
    CHECK(fs::is_directory(out / "bin"), "out/<target>/bin/ not created");
    CHECK(fs::is_directory(out / ".obj"), "out/<target>/.obj/ not created");
    CHECK(fs::is_directory(out / ".bake"), "out/<target>/.bake/ not created");
    CHECK(!fs::exists(dir / "out/bin") && !fs::exists(dir / "out/lib") &&
              !fs::exists(dir / "out/.obj") && !fs::exists(dir / "out/.bake"),
          "native build leaked artifacts into the legacy target-less layout");

    // Old layout should not exist
    CHECK(!fs::exists(dir / "build"), "old build/ directory should not exist");
    CHECK(!fs::exists(dir / ".bake"), "project-root .bake/ should not exist");

    return {};
}

// bake clean must remove the out/ directory.
TestResult test_clean() {
    auto dir = make_temp_dir("clean_test");
    copy_fixture("simple_app", dir);

    // Build first
    auto r1 = run_bake("build", dir);
    CHECK(r1.success(), "build failed: " + r1.stdout);
    CHECK(fs::exists(dir / "out"), "out/ should exist after build");

    // Clean
    auto r2 = run_bake("clean", dir);
    CHECK(r2.success(), "clean failed: " + r2.stdout);
    CHECK(!fs::exists(dir / "out"), "out/ should be removed after clean");

    return {};
}

// bake init must create a valid project skeleton.
TestResult test_init() {
    auto dir = make_temp_dir("init_test");

    auto r = run_bake("init --type executable", dir);
    CHECK(r.success(), "bake init failed: " + r.stdout);

    CHECK(fs::exists(dir / "bake.toml"), "bake.toml not created by init");
    CHECK(fs::exists(dir / "src"), "src/ not created by init");

    // The generated project should be buildable
    auto r2 = run_bake("build", dir);
    CHECK(r2.success(), "generated project failed to build: " + r2.stdout);

    return {};
}

// bake init should produce a real C project when given a C standard.
TestResult test_init_c() {
    auto dir = make_temp_dir("init_c_test");

    auto r = run_bake("init --type executable --std c17", dir);
    CHECK(r.success(), "bake init --std c17 failed: " + r.stdout);
    CHECK(fs::exists(dir / "src" / "main.c"),
          "C scaffold did not create src/main.c");
    CHECK(!fs::exists(dir / "src" / "main.cpp"),
          "C scaffold unexpectedly created src/main.cpp");

    auto build = run_bake("build", dir);
    CHECK(build.success(), "generated C project failed to build: " + build.stdout);

    fs::path exe = target_output_dir(dir) / "bin" / dir.filename();
    CHECK(fs::exists(exe), "generated C executable was not created");
    auto run = run_cmd(exe.string(), dir);
    CHECK_EQ(run.exit_code, 0,
             "generated C executable returned non-zero: " + std::to_string(run.exit_code));

    return {};
}

// bake build --version must report a version string.
TestResult test_version() {
    auto dir = make_temp_dir("version_test");

    auto r = run_bake("--version", dir);
    CHECK(r.success(), "--version failed");
    CHECK(r.stdout.find("bake") != std::string::npos,
          "expected 'bake' in version output: " + r.stdout);

    return {};
}

// bake update <dep> must only update the specified dependency.
// This is a network test — skipped by default.
TestResult test_update_single_dep() {
    auto dir = make_temp_dir("update_single");

    // This test requires network — just verify the CLI parses the arg.
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"update-test\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n\n"
        "[dependencies]\n"
        "fmt = { url = \"https://github.com/fmtlib/fmt\", tag = \"10.2.1\" }\n"
    );

    auto r = run_bake("update nonexistent-dep", dir);
    CHECK(!r.success(), "update of nonexistent dep should fail");
    CHECK(r.stdout.find("not found") != std::string::npos,
          "expected 'not found' message: " + r.stdout);

    return {};
}

// ----------------------------------------------------------------
// New tests: address Codex review P1/P2 issues
// ----------------------------------------------------------------

// Standalone path dep: the project has a path dep on a library with real
// source code. The library's .cpp must be compiled and linked — not just
// headers injected. This tests that path dep source compilation works
// outside of workspace builds.
TestResult test_standalone_path_dep_build() {
    auto dir = make_temp_dir("standalone_path_dep");
    copy_fixture("standalone_path_dep", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "bake build failed for standalone_path_dep: " + r.stdout);

    // Verify the executable exists
    fs::path exe = target_output_dir(dir) / "bin" / "calc";
    CHECK(fs::exists(exe), "executable not found under out/<target>/bin");

    // Run it — should return 0 (multiply(3,4)=12, subtract(12,2)=10, 10-10=0)
    auto run = run_cmd(exe.string(), dir);
    CHECK_EQ(run.exit_code, 0,
             "standalone path dep exe returned non-zero: "
             + std::to_string(run.exit_code) + "\n" + run.stdout);

    return {};
}

// Standalone path dep with --locked: should work since path deps don't need a lockfile.
TestResult test_standalone_path_dep_locked() {
    auto dir = make_temp_dir("standalone_path_dep_locked");
    copy_fixture("standalone_path_dep", dir);

    auto r = run_bake("build --locked", dir);
    CHECK(r.success(), "--locked failed for standalone path dep: " + r.stdout);

    return {};
}

// Duplicate detection works with compact TOML syntax too: an identical
// entry ("fmt={url=...}") is recognized as already present and skipped.
TestResult test_add_duplicate_compact() {
    auto dir = make_temp_dir("add_dup_compact");
    copy_fixture("simple_app", dir);

    // Write bake.toml with compact TOML dependency (no spaces around =)
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"dup-test\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\ncxx = \"c++17\"\n\n"
        "[dependencies]\n"
        "fmt={url=\"https://github.com/fmtlib/fmt\",tag=\"10.2.1\"}\n"
    );
    write_file(dir / "src" / "main.cpp", "int main() { return 0; }\n");

    // Adding "fmt" again with the same definition skips
    auto r = run_bake("add https://github.com/fmtlib/fmt --tag 10.2.1 fmt", dir);
    CHECK(r.success(), "identical add over compact TOML should skip");
    CHECK(r.stdout.find("already a dependency") != std::string::npos,
          "expected 'already a dependency' for compact TOML: " + r.stdout);
    auto manifest = read_file(dir / "bake.toml");
    CHECK(manifest.find("fmt={url=") != std::string::npos,
          "existing compact entry must be untouched");

    return {};
}

// Lockfile transitive node consistency: if a root dep references a child
// node that doesn't exist in the lock, is_consistent() must reject it.
TestResult test_lock_transitive_consistency() {
    auto dir = make_temp_dir("lock_transitive");
    copy_fixture("simple_app", dir);

    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"trans-test\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n\n"
        "[dependencies]\n"
        "fake = { url = \"https://github.com/example/fake\", tag = \"v1.0\" }\n"
    );

    // Write a lockfile where a node references a non-existent child
    write_file(dir / "bake.lock",
        "# AUTO-GENERATED. Do not edit.\n\n"
        "[root_deps]\n"
        "fake = \"fake-v1.0\"\n\n"
        "[nodes.\"fake-v1.0\"]\n"
        "url              = \"https://github.com/example/fake\"\n"
        "tag              = \"v1.0\"\n"
        "commit           = \"abc123def456\"\n"
        "transport_sha256 = \"aaaabbbbccccdddd\"\n"
        "tree_sha256      = \"1111222233334444\"\n"
        "native           = false\n"
        "dependencies     = [\"nonexistent-child-v2.0\"]\n"
    );

    // --locked should reject because the transitive child node doesn't exist
    auto r = run_bake("build --locked", dir);
    CHECK(!r.success(), "--locked should reject lock with missing transitive node");
    CHECK(r.stdout.find("stale") != std::string::npos,
          "expected 'stale' in error: " + r.stdout);

    return {};
}

TestResult test_frozen_missing_cache() {
    auto dir = make_temp_dir("frozen_missing_cache");
    copy_fixture("simple_app", dir);

    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"cache-test\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n\n"
        "[dependencies]\n"
        "fake = { url = \"https://github.com/example/fake\", tag = \"v1.0\" }\n"
    );

    // Write a lockfile with non-empty hashes but cache doesn't exist
    write_file(dir / "bake.lock",
        "# AUTO-GENERATED. Do not edit.\n\n"
        "[root_deps]\n"
        "fake = \"fake-v1.0\"\n\n"
        "[nodes.\"fake-v1.0\"]\n"
        "url              = \"https://github.com/example/fake\"\n"
        "tag              = \"v1.0\"\n"
        "commit           = \"abc123def456789abc123def456789abc123de\"\n"
        "transport_sha256 = \"aaaabbbbccccddddeeeeffff0000111122223\"\n"
        "tree_sha256      = \"1111222233334444555566667777888899aab\"\n"
        "native           = false\n"
        "dependencies     = []\n"
    );

    // --frozen should fail because cache directory doesn't exist
    auto r = run_bake("build --frozen", dir);
    CHECK(!r.success(),
          "--frozen should fail when cache is missing");

    return {};
}

// Workspace members share one output root within the selected target.
TestResult test_workspace_target_output() {
    auto dir = make_temp_dir("ws_target_output");
    copy_fixture("path_dep", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "workspace build failed: " + r.stdout);

    const fs::path out = target_output_dir(dir);
    CHECK(fs::is_directory(out / "bin"), "out/<target>/bin/ should exist");
    CHECK(fs::is_directory(out / "lib"), "out/<target>/lib/ should exist");

    // Machine storage is partitioned by canonical identity, not display name.
    std::size_t object_namespaces = 0;
    for (const auto& entry : fs::directory_iterator(out / ".obj")) {
        if (entry.is_directory()) ++object_namespaces;
    }
    CHECK_EQ(object_namespaces, std::size_t(2),
             "workspace members do not have distinct canonical object namespaces");

    // Verify executable runs
    fs::path exe = out / "bin" / "app";
    CHECK(fs::exists(exe), "workspace exe not found");
    auto run = run_cmd(exe.string(), dir);
    CHECK_EQ(run.exit_code, 0,
             "workspace exe returned non-zero: " + std::to_string(run.exit_code));

    return {};
}

TestResult test_canonical_engine_namespaces() {
    auto dir = make_temp_dir("canonical_engine_namespaces");
    copy_fixture("same_moid_name", dir);

    auto result = run_bake("build -j 1", dir);
    CHECK(result.success(),
          "same-name canonical moids failed to build: " + result.stdout);

    const fs::path out = target_output_dir(dir);
    const std::string graph = read_file(out / ".bake/graph.json");
    CHECK(!graph.empty(), "same-name build did not persist graph.json");

    std::vector<std::string> action_ids;
    std::size_t position = 0;
    while ((position = graph.find("\"id\"", position)) != std::string::npos) {
        auto id = json_scalar_field(
            std::string_view(graph).substr(position), "id");
        CHECK(id.has_value(), "graph action id is not a JSON string");
        action_ids.push_back(*id);
        position += 4;
    }
    CHECK_EQ(action_ids.size(), std::size_t(4),
             "same-name fixture should produce four actions");
    for (std::size_t i = 0; i < action_ids.size(); ++i) {
        for (std::size_t j = i + 1; j < action_ids.size(); ++j) {
            CHECK(action_ids[i] != action_ids[j],
                  "canonical moids produced duplicate action id '" +
                      action_ids[i] + "'");
        }
    }
    CHECK(graph.find("compile:duplicate:") == std::string::npos &&
              graph.find("link:duplicate") == std::string::npos,
          "machine action ids still use the display name");

    std::vector<fs::path> objects;
    for (const auto& entry : fs::recursive_directory_iterator(out / ".obj")) {
        if (entry.is_regular_file() && entry.path().extension() == ".o")
            objects.push_back(entry.path());
    }
    CHECK_EQ(objects.size(), std::size_t(2),
             "same-name canonical moids collided in the object namespace");
    CHECK(objects[0].parent_path() != objects[1].parent_path(),
          "same-name canonical moids share one object owner directory");

    std::vector<std::string> owner_keys;
    for (const auto& entry : fs::directory_iterator(out / ".bmi")) {
        if (!entry.is_directory() || entry.path().filename() == ".std") continue;
        owner_keys.push_back(entry.path().filename().string());
    }
    CHECK_EQ(owner_keys.size(), std::size_t(2),
             "same-name canonical moids share one BMI owner directory");
    for (const auto& owner : owner_keys) {
        bool found = false;
        for (const auto& id : action_ids) {
            if (id.find(owner) != std::string::npos) {
                found = true;
                break;
            }
        }
        CHECK(found,
              "action ids do not contain the canonical storage key '" + owner + "'");
    }

    const std::string fingerprints =
        read_file(out / ".bake/fingerprints.json");
    for (const auto& id : action_ids) {
        CHECK(fingerprints.find("\"" + id + "\"") != std::string::npos,
              "fingerprint state omitted canonical action id '" + id + "'");
    }

    return {};
}

TestResult test_canonical_terminal_collision() {
    auto dir = make_temp_dir("canonical_terminal_collision");
    copy_fixture("same_moid_name", dir);
    write_file(dir / "right/bake.toml",
        "[package]\n"
        "name = \"duplicate\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n");

    auto result = run_bake("build -j 1", dir);
    CHECK(!result.success(),
          "same terminal path for distinct canonical moids unexpectedly succeeded");
    CHECK(result.stdout.find("terminal output collision") != std::string::npos &&
              result.stdout.find("workspace:left") != std::string::npos &&
              result.stdout.find("workspace:right") != std::string::npos &&
              result.stdout.find("duplicate") != std::string::npos,
          "terminal collision diagnostic did not identify both canonical moids: " +
              result.stdout);

    return {};
}

TestResult test_archive_rebuild_drops_removed_objects() {
    auto dir = make_temp_dir("archive_rebuild_drops_removed_objects");
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"fresh-archive\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n");
    write_file(dir / "src/old.c", "int old_value(void) { return 1; }\n");

    auto initial = run_bake("build -j 1", dir);
    CHECK(initial.success(), "initial archive build failed: " + initial.stdout);
    CHECK(fs::remove(dir / "src/old.c"),
          "failed to remove old archive source");
    write_file(dir / "src/new.c", "int new_value(void) { return 2; }\n");

    auto rebuilt = run_bake("build -j 1", dir);
    CHECK(rebuilt.success(), "archive rebuild failed: " + rebuilt.stdout);
    const fs::path archive =
        target_output_dir(dir) / "lib/libfresh-archive.a";
    auto listing = run_cmd("ar t \"" + archive.string() + "\"", dir);
    CHECK(listing.success(), "could not inspect rebuilt archive: " + listing.stdout);
    CHECK(listing.stdout.find("new_") != std::string::npos &&
              listing.stdout.find("old_") == std::string::npos,
          "archive rebuild retained an object removed from the build graph: " +
              listing.stdout);

    return {};
}

TestResult test_archive_failure_is_atomic() {
#ifdef _WIN32
    return {};
#else
    auto dir = make_temp_dir("archive_failure_is_atomic");
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"atomic-archive\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n");
    const fs::path source = dir / "src/value.c";
    write_file(source, "int archive_value(void) { return 1; }\n");

    // Build initial archive.
    auto initial = run_bake("build -j 1", dir);
    CHECK(initial.success(), "initial atomic archive build failed: " + initial.stdout);
    const fs::path archive =
        target_output_dir(dir) / "lib/libatomic-archive.a";
    auto initial_listing = run_cmd("ar t \"" + archive.string() + "\"", dir);
    CHECK(initial_listing.success() &&
              initial_listing.stdout.find("value") != std::string::npos,
          "initial archive was not readable: " + initial_listing.stdout);

    // Inject a compile failure: invalid source code.
    write_file(source, "BROKEN SYNTAX\n");
    auto failed = run_bake("build -j 1", dir);
    CHECK(!failed.success(), "injected compile failure unexpectedly succeeded");

    // Old archive must be preserved (atomic: failed build doesn't replace it).
    auto preserved = run_cmd("ar t \"" + archive.string() + "\"", dir);
    CHECK(preserved.success() &&
              preserved.stdout.find("value") != std::string::npos,
          "failed build replaced the last good archive: " +
              preserved.stdout);

    // Fix source → rebuild should succeed.
    write_file(source, "int archive_value(void) { return 2; }\n");
    auto recovered = run_bake("build -j 1", dir);
    CHECK(recovered.success(), "archive did not recover after failure: " +
              recovered.stdout);
    CHECK(recovered.stdout.find("Compiling") != std::string::npos,
          "partial archive was mistaken for an up-to-date output: " +
              recovered.stdout);
    auto recovered_listing = run_cmd("ar t \"" + archive.string() + "\"", dir);
    CHECK(recovered_listing.success() &&
              recovered_listing.stdout.find("value") != std::string::npos,
          "recovered archive was not readable: " + recovered_listing.stdout);

    return {};
#endif
}

TestResult test_terminal_case_collision() {
    auto dir = make_temp_dir("terminal_case_collision");
    copy_fixture("same_moid_name", dir);
    write_file(dir / "right/bake.toml",
        "[package]\n"
        "name = \"DUPLICATE\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n");

    auto result = run_bake("build -j 2", dir);
    CHECK(!result.success(),
          "case-only terminal paths unexpectedly resolved as distinct");
    CHECK(result.stdout.find("terminal output collision") != std::string::npos &&
              result.stdout.find("workspace:left") != std::string::npos &&
              result.stdout.find("workspace:right") != std::string::npos,
          "case-only terminal collision lacked canonical owners: " +
              result.stdout);

    return {};
}

TestResult test_terminal_output_escape() {
    auto write_project = [](const fs::path& dir, const std::string& name) {
        write_file(dir / "bake.toml",
            "[package]\n"
            "name = \"" + name + "\"\n"
            "version = \"0.1.0\"\n"
            "type = \"executable\"\n"
            "[language]\nc = \"c17\"\n");
        write_file(dir / "src/main.c", "int main(void) { return 0; }\n");
    };

    auto relative = make_temp_dir("terminal_output_relative_escape");
    write_project(relative, "../.bake/fingerprints.json");
    auto relative_result = run_bake("build -j 1", relative);
    CHECK(!relative_result.success(),
          "terminal output escaped out/bin through a relative moid name");
    CHECK(relative_result.stdout.find("invalid terminal output name") !=
              std::string::npos,
          "relative terminal escape lacked a precise diagnostic: " +
              relative_result.stdout);

    auto absolute = make_temp_dir("terminal_output_absolute_escape");
    const fs::path escaped = absolute / "escaped-output";
    write_project(absolute, escaped.string());
    auto absolute_result = run_bake("build -j 1", absolute);
    CHECK(!absolute_result.success(),
          "absolute moid name wrote a terminal outside out/bin");
    CHECK(absolute_result.stdout.find("invalid terminal output name") !=
              std::string::npos,
          "absolute terminal escape lacked a precise diagnostic: " +
              absolute_result.stdout);
    CHECK(!fs::exists(escaped),
          "absolute terminal output was created outside out/bin");

    auto nonportable = make_temp_dir("terminal_output_nonportable_name");
    const std::vector<std::pair<std::string, std::string>> invalid_names = {
        {"unicode", "\xC3\x89"},
        {"reserved", "CON"},
        {"trailing-dot", "trailing."},
        {"space", "contains space"},
    };
    for (const auto& [label, name] : invalid_names) {
        write_project(nonportable, name);
        auto result = run_bake("build -j 1", nonportable);
        CHECK(!result.success(),
              "non-portable terminal name '" + label + "' unexpectedly succeeded");
        CHECK(result.stdout.find("portable ASCII") != std::string::npos,
              "non-portable terminal name '" + label +
                  "' lacked a portable-name diagnostic: " + result.stdout);
    }

    return {};
}

// Workspace build with -p filter: only the specified member should be built.
TestResult test_workspace_member_filter() {
    auto dir = make_temp_dir("ws_filter");
    copy_fixture("path_dep", dir);

    // Build only mylib
    auto r = run_bake("build -p mylib", dir);
    CHECK(r.success(), "build -p mylib failed: " + r.stdout);

    // mylib should be built
    fs::path lib_dir = target_output_dir(dir) / "lib";
    bool found_lib = false;
    if (fs::exists(lib_dir)) {
        for (auto& e : fs::directory_iterator(lib_dir)) {
            if (e.path().extension() == ".a") { found_lib = true; break; }
        }
    }
    CHECK(found_lib, "mylib .a should exist after build -p mylib");

    return {};
}

TestResult test_workspace_selection_identity() {
    auto dir = make_temp_dir("workspace_selection_identity");
    write_file(dir / "bake.toml",
        "[workspace]\n"
        "members = [\"./chosen\", \"other\"]\n");
    write_file(dir / "chosen/bake.toml",
        "[package]\n"
        "name = \"path-target\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[features]\n"
        "marker = {}\n");
    write_file(dir / "chosen/src/main.c",
        "int main(void) { return 0; }\n");
    write_file(dir / "other/bake.toml",
        "[package]\n"
        "name = \"chosen\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n");
    write_file(dir / "other/src/value.c",
        "int other_value(void) { return 1; }\n");
    auto by_path = run_bake(
        "build -p chosen --feature marker -j 1", dir);
    CHECK(by_path.success(),
          "canonical workspace path did not take precedence over display name: " +
              by_path.stdout);
    const fs::path selected_out = target_output_dir(dir);
    CHECK(fs::exists(selected_out / "bin/path-target"),
          "canonical workspace path did not build the selected executable");
    CHECK(!fs::exists(selected_out / "lib/libchosen.a"),
          "display-name cross-match built an unselected workspace member");

    auto run_by_path = run_bake(
        "run -p chosen --option marker -j 1", dir);
    CHECK(run_by_path.success(),
          "run did not reuse canonical workspace selection: " +
              run_by_path.stdout);

    fs::remove_all(dir / "out");
    auto by_unique_name = run_bake("build -p path-target -j 1", dir);
    CHECK(by_unique_name.success(),
          "unique workspace display-name fallback failed: " +
              by_unique_name.stdout);
    const std::string selected_graph =
        read_file(target_output_dir(dir) / ".bake/graph.json");
    CHECK(selected_graph.find("\"moid_id\": \"workspace:chosen\"") !=
              std::string::npos &&
              selected_graph.find("\"moid_id\": \"workspace:other\"") ==
              std::string::npos,
          "display-name fallback built the wrong canonical owner: " +
              selected_graph);

    write_file(dir / "chosen/bake.toml",
        "[package]\n"
        "name = \"friendly\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\nc = \"c17\"\n");
    write_file(dir / "other/bake.toml",
        "[package]\n"
        "name = \"friendly\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n");

    auto ambiguous = run_bake("build -p friendly -j 1", dir);
    CHECK(!ambiguous.success(),
          "ambiguous workspace display name unexpectedly resolved");
    CHECK(ambiguous.stdout.find("ambiguous") != std::string::npos &&
              ambiguous.stdout.find("chosen") != std::string::npos &&
              ambiguous.stdout.find("other") != std::string::npos,
          "workspace ambiguity diagnostic omitted canonical candidates: " +
              ambiguous.stdout);

    return {};
}

TestResult test_workspace_duplicate_canonical_member() {
    auto dir = make_temp_dir("workspace_duplicate_canonical_member");
    write_file(dir / "bake.toml",
        "[workspace]\n"
        "members = [\"member\", \"./member\"]\n");
    write_file(dir / "member/bake.toml",
        "[package]\n"
        "name = \"duplicate-member\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\nc = \"c17\"\n");
    write_file(dir / "member/src/main.c", "int main(void) { return 0; }\n");

    auto result = run_bake("build -j 1", dir);
    CHECK(!result.success(),
          "duplicate canonical workspace members were silently merged");
    CHECK(result.stdout.find("duplicate canonical workspace member") !=
              std::string::npos &&
              result.stdout.find("'member'") != std::string::npos &&
              result.stdout.find("'./member'") != std::string::npos,
          "duplicate member diagnostic omitted original spellings: " +
              result.stdout);

    return {};
}

TestResult test_workspace_symlink_selector() {
    auto dir = make_temp_dir("workspace_symlink_selector");
    write_file(dir / "bake.toml",
        "[workspace]\n"
        "members = [\"real\", \"other\"]\n");
    write_file(dir / "real/bake.toml",
        "[package]\n"
        "name = \"real-app\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\nc = \"c17\"\n");
    write_file(dir / "real/src/main.c", "int main(void) { return 0; }\n");
    write_file(dir / "other/bake.toml",
        "[package]\n"
        "name = \"other-lib\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n");
    write_file(dir / "other/src/value.c", "int value(void) { return 1; }\n");
    std::error_code ec;
    fs::create_directory_symlink("real", dir / "alias", ec);
    CHECK(!ec, "failed to create workspace selector symlink: " + ec.message());

    auto result = run_bake("build -p alias -j 1", dir);
    CHECK(result.success(),
          "symlink selector did not resolve to the canonical workspace root: " +
              result.stdout);
    const fs::path out = target_output_dir(dir);
    CHECK(fs::exists(out / "bin/real-app"),
          "symlink selector did not build its canonical member");
    CHECK(!fs::exists(out / "lib/libother-lib.a"),
          "symlink selector built an unselected member");

    return {};
}

TestResult test_executable_dependency() {
    auto dir = make_temp_dir("executable_dependency");
    copy_fixture("executable_dependency", dir);

    auto result = run_bake("build", dir);
    CHECK(!result.success(),
          "normal dependency on an executable moid unexpectedly succeeded");
    CHECK(result.stdout.find(
              "moid 'app' cannot use executable moid 'tool' as a normal dependency") !=
              std::string::npos,
          "missing executable dependency diagnostic: " + result.stdout);

    auto build_cpp_project = make_temp_dir("build_cpp_executable_dependency");
    copy_fixture("executable_dependency", build_cpp_project);
    write_file(build_cpp_project / "tool/bake.toml",
        "[package]\n"
        "name = \"tool\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\ncxx = \"c++23\"\n");
    write_file(build_cpp_project / "tool/build.cpp",
        "import bake.build;\n\n"
        "int main() {\n"
        "    bake::Builder builder;\n"
        "    builder.sources(\"src/*.cpp\");\n"
        "    return builder.build();\n"
        "}\n");
    write_file(build_cpp_project / "app/build.cpp",
        "import bake.build;\n"
        "import std;\n\n"
        "int main() {\n"
        "    std::ofstream marker(\"configured.marker\");\n"
        "    marker << \"configured\";\n"
        "    marker.close();\n"
        "    bake::Builder builder;\n"
        "    builder.sources(\"src/*.cpp\");\n"
        "    return builder.build();\n"
        "}\n");

    auto build_cpp_result = run_bake("build -j 1", build_cpp_project);
    CHECK(!build_cpp_result.success(),
          "build.cpp-declared executable dependency unexpectedly succeeded");
    CHECK(build_cpp_result.stdout.find(
              "moid 'app' cannot use executable moid 'tool' as a normal dependency") !=
              std::string::npos,
          "build.cpp dependency lost the executable diagnostic: " +
              build_cpp_result.stdout);
    CHECK(!fs::exists(build_cpp_project / "app/configured.marker"),
          "consumer build.cpp ran after its dependency declared executable");

    auto build_cpp_library = make_temp_dir("build_cpp_library_dependency");
    copy_fixture("executable_dependency", build_cpp_library);
    write_file(build_cpp_library / "tool/bake.toml",
        "[package]\n"
        "name = \"tool\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\ncxx = \"c++23\"\n");
    write_file(build_cpp_library / "tool/build.cpp",
        "import bake.build;\n\n"
        "int main() {\n"
        "    bake::Builder builder;\n"
        "    builder.sources(\"src/*.cpp\");\n"
        "    return builder.build();\n"
        "}\n");
    write_file(build_cpp_library / "tool/src/main.cpp",
        "int tool_value() { return 42; }\n");
    write_file(build_cpp_library / "app/src/main.cpp",
        "int tool_value();\n"
        "int main() { return tool_value() == 42 ? 0 : 1; }\n");

    auto library_result = run_bake("build -j 1", build_cpp_library);
    CHECK(library_result.success(),
          "build.cpp-declared library was rejected by its manifest type: " +
              library_result.stdout);
    auto library_run = run_cmd(
        (target_output_dir(build_cpp_library) / "bin/app").string(),
        build_cpp_library);
    CHECK(library_run.success(),
          "consumer did not link the build.cpp-declared library dependency");

    return {};
}

TestResult test_run_build_cpp_declaration() {
    auto build_cpp_executable = make_temp_dir("run_build_cpp_executable");
    write_file(build_cpp_executable / "bake.toml",
        "[package]\n"
        "name = \"build-cpp-run\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\ncxx = \"c++23\"\n");
    write_file(build_cpp_executable / "build.cpp",
        "import bake.build;\n\n"
        "int main() {\n"
        "    bake::Builder builder;\n"
        "    builder.sources(\"src/main.cpp\");\n"
        "    return builder.build();\n"
        "}\n");
    write_file(build_cpp_executable / "src/main.cpp",
        "import std;\n"
        "int main() { std::println(\"RUNTIME_FINAL_DECLARATION_SENTINEL\"); "
        "return 0; }\n");

    auto executable_run = run_bake("run -j 1", build_cpp_executable);
    CHECK(executable_run.success(),
          "run ignored the build.cpp executable declaration: " +
              executable_run.stdout);
    CHECK(executable_run.stdout.find(
              "RUNTIME_FINAL_DECLARATION_SENTINEL\n") != std::string::npos,
          "run did not execute the build.cpp declaration output: " +
              executable_run.stdout);

    return {};
}

// BAKE_TARGET must always carry the effective triple — the host triple for
// native builds, the -t triple for cross builds. build.cpp platform
// branching (per-target source selection) depends on it.
TestResult test_build_cpp_target_env() {
    auto dir = make_temp_dir("build_cpp_target_env");
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"target-env\"\n"
        "version = \"0.1.0\"\n"
        "[language]\ncxx = \"c++23\"\n");
    write_file(dir / "build.cpp",
        "import bake.build;\n"
        "import std;\n\n"
        "int main() {\n"
        "    bake::Builder builder;\n"
        "    if (builder.target().empty()) {\n"
        "        std::println(std::cerr, \"build.cpp: BAKE_TARGET is empty\");\n"
        "        return 1;\n"
        "    }\n"
        "    builder.sources(\"src/main.cpp\");\n"
        "    return builder.build();\n"
        "}\n");
    write_file(dir / "src/main.cpp",
        "import std;\n"
        "int main() { std::println(\"TARGET_ENV_OK\"); return 0; }\n");

    auto native = run_bake("build -j 1", dir);
    CHECK(native.success(),
          "native build.cpp saw empty BAKE_TARGET: " + native.stdout);

    auto cross = run_bake("build -j 1 -t x86_64-linux-musl", dir);
    CHECK(cross.success(),
          "cross build.cpp saw empty BAKE_TARGET: " + cross.stdout);

    return {};
}

// Local header edits must reach the rebuild: the compile action's inputs
// cover the #include closure (and -include forced headers), so an edit
// recompiles — and an untouched build stays a no-op.
TestResult test_header_incremental_rebuild() {
    auto dir = make_temp_dir("header_incremental_rebuild");
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"hdr-inc\"\n"
        "version = \"0.1.0\"\n");
    write_file(dir / "src/answer.h",
        "#pragma once\n"
        "inline int answer() { return 1; }\n");
    write_file(dir / "src/main.cpp",
        "#include \"answer.h\"\n"
        "#include <cstdio>\n"
        "int main() { std::printf(\"%d\\n\", answer()); return 0; }\n");

    auto first = run_bake("run -j 1", dir);
    CHECK(first.success() && first.stdout.find("1\n") != std::string::npos,
          "initial build/run failed: " + first.stdout);

    write_file(dir / "src/answer.h",
        "#pragma once\n"
        "inline int answer() { return 2; }\n");
    auto second = run_bake("run -j 1", dir);
    CHECK(second.success() && second.stdout.find("2\n") != std::string::npos,
          "header edit did not reach the binary (stale value): " +
              second.stdout);

    auto untouched = run_bake("build -j 1", dir);
    CHECK(untouched.success() &&
              untouched.stdout.find("Compiling") == std::string::npos,
          "untouched rebuild recompiled (over-rebuild): " +
              untouched.stdout);

    return {};
}

// Binary sources consume the main moid's public module interfaces the same
// way external consumers do.
TestResult test_build_cpp_binary_module_imports() {
    auto dir = make_temp_dir("build_cpp_binary_module_imports");
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"binary-module-imports\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\ncxx = \"c++23\"\n");
    write_file(dir / "public/mod.cppm",
        "export module bmi_math;\n\n"
        "export int triple(int x) { return x * 3; }\n");
    write_file(dir / "tools/user.cpp",
        "import std;\n"
        "import bmi_math;\n\n"
        "int main() {\n"
        "    std::println(\"BMI_MATH_{}\", triple(14));\n"
        "    return 0;\n"
        "}\n");
    write_file(dir / "build.cpp",
        "import bake.build;\n\n"
        "int main() {\n"
        "    bake::Builder b;\n"
        "    b.public_modules(\"public/mod.cppm\");\n"
        "    b.binary(\"module-user\")\n"
        "        .sources(\"tools/user.cpp\");\n"
        "    return b.build();\n"
        "}\n");

    auto build = run_bake("build", dir);
    CHECK(build.success(),
          "binary importing a public module failed to build: " + build.stdout);
    const fs::path out = target_output_dir(dir);
#ifdef _WIN32
    const fs::path main_library = out / "lib/binary-module-imports.lib";
#else
    const fs::path main_library = out / "lib/libbinary-module-imports.a";
#endif
    CHECK(fs::exists(main_library), "main lib archive was not produced");
    const fs::path user = native_executable_path(out, "module-user");
    CHECK(fs::exists(user), "module-importing binary was not produced");
    auto run = run_cmd(user.string(), dir);
    CHECK(run.success() && run.stdout.find("BMI_MATH_42") != std::string::npos,
          "module-importing binary did not run correctly: " + run.stdout);

    return {};
}

TestResult test_dependency_binaries_stay_out() {
    auto dir = make_temp_dir("dep_binaries_stay_out");
    write_file(dir / "dep/bake.toml",
        "[package]\n"
        "name = \"tooldep\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\ncxx = \"c++23\"\n");
    write_file(dir / "dep/dep.cppm",
        "export module tooldep;\n"
        "export int dep_value() { return 7; }\n");
    write_file(dir / "dep/tool.cpp",
        "import std;\n"
        "import tooldep;\n"
        "int main() { std::println(\"TOOL_{}\", dep_value()); }\n");
    write_file(dir / "dep/build.cpp",
        "import bake.build;\n"
        "int main() {\n"
        "    bake::Builder b;\n"
        "    b.public_modules(\"dep.cppm\");\n"
        "    b.binary(\"dep-tool\").sources(\"tool.cpp\");\n"
        "    return b.build();\n"
        "}\n");
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"dep-bin-consumer\"\n"
        "version = \"0.1.0\"\n"
        "[language]\ncxx = \"c++23\"\n"
        "[dependencies]\n"
        "tooldep = { path = \"dep\" }\n");
    write_file(dir / "src/main.cpp",
        "import std;\n"
        "import tooldep;\n"
        "int main() { std::println(\"CONSUMER_{}\", dep_value()); }\n");

    // As a dependency: the lib flows in, its binary does not.
    auto build = run_bake("build", dir);
    CHECK(build.success(), "consumer build failed: " + build.stdout);
    const fs::path out = target_output_dir(dir);
    CHECK(fs::exists(native_executable_path(out, "dep-bin-consumer")),
          "consumer executable was not produced");
    CHECK(!fs::exists(native_executable_path(out, "dep-tool")),
          "dependency binary leaked into the consumer build");

    // As the root: the same port builds its own binary.
    auto root_build = run_bake("build", dir / "dep");
    CHECK(root_build.success(),
          "dependency-as-root build failed: " + root_build.stdout);
    const fs::path dep_out = target_output_dir(dir / "dep");
    CHECK(fs::exists(native_executable_path(dep_out, "dep-tool")),
          "root binary was not produced when building the port itself");

    return {};
}

TestResult test_source_less_executable_rejects_stale_output() {
    auto dir = make_temp_dir("source_less_executable");
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"stale-source\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\nc = \"c17\"\n");
    write_file(dir / "src/main.c",
        "#include <stdio.h>\n"
        "int main(void) { puts(\"STALE_SOURCE_SENTINEL\"); return 0; }\n");

    auto initial = run_bake("build -j 1", dir);
    CHECK(initial.success(),
          "could not create stale source executable: " + initial.stdout);
    CHECK(fs::remove(dir / "src/main.c"),
          "failed to remove executable source from fixture");

    auto run = run_bake("run -j 1", dir);
    CHECK(!run.success(),
          "source-less executable ran an output from the previous build");
    CHECK(run.stdout.find("executable moid 'stale-source' has no sources") !=
              std::string::npos,
          "source-less executable lacked a precise diagnostic: " + run.stdout);
    CHECK(run.stdout.find("STALE_SOURCE_SENTINEL") == std::string::npos,
          "run executed the stale source-less binary");

    return {};
}

TestResult test_run_requires_member_for_multiple_executables() {
    auto dir = make_temp_dir("run_multiple_executables");
    write_file(dir / "bake.toml",
        "[workspace]\n"
        "members = [\"left\", \"right\"]\n");
    auto write_member = [&](const std::string& member,
                            const std::string& name) {
        write_file(dir / member / "bake.toml",
            "[package]\n"
            "name = \"" + name + "\"\n"
            "version = \"0.1.0\"\n"
            "type = \"executable\"\n"
            "[language]\nc = \"c17\"\n");
        write_file(dir / member / "src/main.c",
            "#include <stdio.h>\n"
            "int main(void) { puts(\"RAN_" + name + "\"); return 0; }\n");
    };
    write_member("left", "left-app");
    write_member("right", "right-app");

    auto result = run_bake("run -j 1", dir);
    CHECK(!result.success(),
          "workspace run silently chose one of multiple executable roots");
    CHECK(result.stdout.find("multiple executable packages") !=
              std::string::npos &&
              result.stdout.find("workspace:left") != std::string::npos &&
              result.stdout.find("workspace:right") != std::string::npos &&
              result.stdout.find("-p") != std::string::npos,
          "ambiguous run did not list canonical candidates and -p: " +
              result.stdout);
    CHECK(result.stdout.find("RAN_left-app") == std::string::npos &&
              result.stdout.find("RAN_right-app") == std::string::npos,
          "ambiguous run executed an arbitrary workspace member");

    return {};
}

// A consumer using default input discovery needs only bake.toml and src/.
// Bake must build each native dependency's own recipe and automatically consume
// its exported usage requirements; the consumer must not need build.cpp glue.
TestResult test_default_discovery_meta_dependency() {
    auto dir = make_temp_dir("build_cpp_meta_dep");
    copy_fixture("build_cpp_meta_dep", dir);

    auto build = run_bake("build", dir);
    CHECK(build.success(), "meta dependency build failed: " + build.stdout);

    const fs::path out = target_output_dir(dir);
    const fs::path answer_lib =
        out / "lib" / "libanswer.a";
    const fs::path base_lib =
        out / "lib" / "libbase.a";
    CHECK(fs::exists(answer_lib),
          "dependency library was not built under out/<target>/lib");
    CHECK(fs::exists(base_lib),
          "transitive dependency library was not built under out/<target>/lib");
    std::size_t object_namespaces = 0;
    std::size_t object_files = 0;
    for (const auto& entry : fs::directory_iterator(out / ".obj")) {
        if (entry.is_directory()) ++object_namespaces;
    }
    for (const auto& entry : fs::recursive_directory_iterator(out / ".obj")) {
        if (entry.is_regular_file() && entry.path().extension() == ".o")
            ++object_files;
    }
    CHECK_EQ(object_namespaces, std::size_t(3),
             "meta dependency graph lacks canonical object namespaces");
    CHECK(object_files >= 3,
          "dependency objects were not built under consumer out/<target>/.obj");
    CHECK(!fs::exists(dir / "answer" / "out") &&
              !fs::exists(dir / "answer" / ".bake"),
          "dependency source directory was modified by the build");
    CHECK(!fs::exists(dir / "base" / "out") &&
              !fs::exists(dir / "base" / ".bake"),
          "transitive dependency source directory was modified by the build");

    fs::path exe = out / "bin" / "meta-consumer";
    CHECK(fs::exists(exe), "meta consumer executable was not created");
    auto initial_run = run_cmd(exe.string(), dir);
    CHECK_EQ(initial_run.exit_code, 0,
             "dependency option activation did not reach build.cpp source: " +
                 initial_run.stdout);

    // A CLI option belongs to the package at the command root. Even when the
    // consumer and a transitive dependency declare the same name, the root's
    // value must not activate the dependency package.
    auto consumer_option = run_bake("build --option wolfssl", dir);
    CHECK(consumer_option.success(),
          "consumer's own option should be accepted: " + consumer_option.stdout);
    auto isolated_run = run_cmd(exe.string(), dir);
    CHECK_EQ(isolated_run.exit_code, 0,
             "consumer CLI option leaked into the dependency package");

    const auto answer_mtime = fs::last_write_time(answer_lib);
    const auto base_mtime = fs::last_write_time(base_lib);

    auto rebuild = run_bake("build", dir);
    CHECK(rebuild.success(), "meta dependency rebuild failed: " + rebuild.stdout);
    CHECK_EQ(fs::last_write_time(answer_lib),
             answer_mtime, "unchanged direct meta dependency was rebuilt");
    CHECK_EQ(fs::last_write_time(base_lib),
             base_mtime, "unchanged transitive meta dependency was rebuilt");

    auto unknown_dir = make_temp_dir("build_cpp_meta_dep_unknown_option");
    copy_fixture("build_cpp_meta_dep", unknown_dir);
    write_file(unknown_dir / "bake.toml",
        "[package]\n"
        "name = \"meta-consumer\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\ncxx = \"c++23\"\n\n"
        "[dependencies]\n"
        "answer = { path = \"answer\", features = [\"missing\"] }\n");
    auto unknown = run_bake("build", unknown_dir);
    CHECK(!unknown.success(), "undeclared dependency feature should fail");
    CHECK(unknown.stdout.find(
              "feature 'missing' is not declared by package 'answer'") !=
              std::string::npos,
          "undeclared dependency feature did not identify its owner: " +
              unknown.stdout);
    auto wrong_type_dir = make_temp_dir("build_cpp_meta_dep_non_table_feature");
    copy_fixture("build_cpp_meta_dep", wrong_type_dir);
    write_file(wrong_type_dir / "answer/bake.toml",
        "[package]\n"
        "name = \"answer\"\n"
        "version = \"1.0.0\"\n"
        "type = \"lib\"\n"
        "[language]\ncxx = \"c++23\"\n\n"
        "[dependencies]\n"
        "base = { path = \"../base\" }\n\n"
        "[features]\n"
        "biased = 1\n");
    auto wrong_type = run_bake("build", wrong_type_dir);
    CHECK(!wrong_type.success(), "non-table feature declaration should fail");
    CHECK(wrong_type.stdout.find("biased") != std::string::npos &&
              wrong_type.stdout.find("table") != std::string::npos,
          "non-table feature type was not diagnosed: " +
              wrong_type.stdout);
    // One path leaves base.wolfssl at its default while another enables it.
    // Features unify by union, so base is compiled once with wolfssl active.
    auto unified_dir = make_temp_dir("build_cpp_meta_dep_unified_feature");
    copy_fixture("build_cpp_meta_dep", unified_dir);
    write_file(unified_dir / "bake.toml",
        "[package]\n"
        "name = \"meta-consumer\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\ncxx = \"c++23\"\n\n"
        "[dependencies]\n"
        "answer = { path = \"answer\", features = [\"biased\"] }\n"
        "base = { path = \"base\", features = [\"wolfssl\"] }\n");
    write_file(unified_dir / "src/main.cpp",
        "#include <answer/answer.hpp>\n\n"
        "int main() { return answer() == 42 ? 0 : 1; }\n");
    auto unified = run_bake("build", unified_dir);
    CHECK(unified.success(),
          "bool options from multiple dependency paths did not merge with OR: " +
              unified.stdout);
    auto unified_run = run_cmd(
        (target_output_dir(unified_dir) / "bin" / "meta-consumer").string(),
        unified_dir);
    CHECK_EQ(unified_run.exit_code, 0,
             "OR-merged dependency option was not used by the single base build");
    const std::string unified_graph = read_file(
        target_output_dir(unified_dir) / ".bake/graph.json");
    CHECK_EQ(count_occurrences(unified_graph, "\"moid\": \"base\""),
             std::size_t(2),
             "OR resolution built more than one base compile/archive pair");

    return {};
}

TestResult test_overlapping_source_groups() {
    auto duplicate = make_temp_dir("duplicate_source_groups");
    write_file(duplicate / "bake.toml",
        "[package]\n"
        "name = \"duplicate-sources\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\nc = \"c17\"\n");
    write_file(duplicate / "src/main.c",
        "int main(void) { return 0; }\n");
    write_file(duplicate / "build.cpp",
        "import bake.build;\n\n"
        "int main() {\n"
        "    bake::Builder builder;\n"
        "    builder.sources(\"src/*.c\")\n"
        "        .sources(\"src/main.c\");\n"
        "    return builder.build();\n"
        "}\n");

    auto duplicate_result = run_bake("build -j 2", duplicate);
    CHECK(duplicate_result.success(),
          "identical overlapping source groups created duplicate actions: " +
              duplicate_result.stdout);
    const std::string graph = read_file(
        target_output_dir(duplicate) / ".bake/graph.json");
    CHECK_EQ(count_occurrences(graph, "\"type\": \"compile\""),
             std::size_t(1),
             "one source produced more than one compile action");

    // Per-source options are removed in the new design.
    // Duplicate sources always merge silently.
    auto triple = make_temp_dir("triple_source_groups");
    write_file(triple / "bake.toml",
        "[package]\n"
        "name = \"triple-sources\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\nc = \"c17\"\n");
    write_file(triple / "src/main.c",
        "int main(void) { return 0; }\n");
    write_file(triple / "build.cpp",
        "import bake.build;\n\n"
        "int main() {\n"
        "    bake::Builder builder;\n"
        "    builder.sources(\"src/*.c\")\n"
        "        .sources(\"src/main.c\")\n"
        "        .sources(\"src/main.c\");\n"
        "    return builder.build();\n"
        "}\n");

    auto triple_result = run_bake("build -j 2", triple);
    CHECK(triple_result.success(),
          "triple overlapping source groups created duplicate actions: " +
              triple_result.stdout);
    const std::string triple_graph = read_file(
        target_output_dir(triple) / ".bake/graph.json");
    CHECK_EQ(count_occurrences(triple_graph, "\"type\": \"compile\""),
             std::size_t(1),
             "triple-overlap source produced more than one compile action");
    return {};
}

TestResult test_symlink_source_identity() {
    auto dir = make_temp_dir("symlink_source_identity");
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"symlink-sources\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\nc = \"c17\"\n");
    write_file(dir / "src/main.c", "int main(void) { return 0; }\n");
    std::error_code ec;
    fs::create_symlink("main.c", dir / "src/main-link.c", ec);
    CHECK(!ec, "failed to create source symlink: " + ec.message());
    write_file(dir / "build.cpp",
        "import bake.build;\n\n"
        "int main() {\n"
        "    bake::Builder builder;\n"
        "    builder.sources(\"src/main.c\")\n"
        "        .sources(\"src/main-link.c\");\n"
        "    return builder.build();\n"
        "}\n");

    auto result = run_bake("build -j 2", dir);
    CHECK(result.success(),
          "symlink-equivalent source paths produced duplicate actions: " +
              result.stdout);
    const std::string graph = read_file(
        target_output_dir(dir) / ".bake/graph.json");
    CHECK_EQ(count_occurrences(graph, "\"type\": \"compile\""),
             std::size_t(1),
             "symlink-equivalent source produced more than one compile action");

    return {};
}

// build.cpp can read bool options to select inputs. The same options are also
// injected automatically as normalized 0/1 macros for compiled sources.
TestResult test_build_cpp_options() {
    auto dir = make_temp_dir("build_cpp_options");
    copy_fixture("build_cpp_options", dir);

    auto test_home = dir / "home";
    fs::create_directories(test_home);
#ifdef _WIN32
    const std::string cache_env =
        "LOCALAPPDATA=" + test_home.string();
#else
    const std::string cache_env = "HOME=" + test_home.string();
#endif
    auto run_option_bake = [&](const std::string& args) {
        return run_bake(args, dir, cache_env);
    };

    auto initial = run_option_bake("build");
    CHECK(initial.success(), "default option build failed: " + initial.stdout);
    CHECK(initial.stdout.find("Building option-app v0.1.0") !=
              std::string::npos,
          "build output did not identify the project: " + initial.stdout);
    CHECK(initial.stdout.find("Compiling option-app") != std::string::npos,
          "build output did not show Compiling line: " + initial.stdout);
    CHECK(initial.stdout.find("Finished") != std::string::npos,
          "build output did not show Finished line: " +
              initial.stdout);
    const fs::path out = target_output_dir(dir);
    CHECK(!fs::exists(out / ".bmi" / ".std"),
          "std module PCMs should not be in out/<target>/.bmi/.std");
    const std::string compile_commands =
        read_file(dir / "compile_commands.json");
    CHECK(contains_command_token(compile_commands, "-O2") &&
              !contains_command_token(compile_commands, "-g") &&
              !contains_command_token(compile_commands, "-Wall"),
          "manifest profile was not applied to build.cpp-declared sources: " +
              compile_commands);

    int same_stem_objects = 0;
    std::set<std::string> object_owners;
    for (const auto& entry : fs::recursive_directory_iterator(out / ".obj")) {
        const std::string filename = entry.path().filename().string();
        if (filename.find("value_") == 0 &&
            entry.path().extension() == ".o") {
            ++same_stem_objects;
            object_owners.insert(
                entry.path().parent_path().filename().string());
        }
    }
    CHECK_EQ(same_stem_objects, 2,
             "same-named sources did not receive distinct object files");
    CHECK_EQ(object_owners.size(), std::size_t(1),
             "one canonical moid used multiple object namespaces");

    const std::string& owner = *object_owners.begin();
    const auto build_graph = read_file(out / ".bake" / "graph.json");
    CHECK(build_graph.find(
              "compile:" + owner + ":src/left/value.c") !=
              std::string::npos &&
              build_graph.find(
              "compile:" + owner + ":src/right/value.c") !=
              std::string::npos,
          "build graph action IDs do not include stable source identities: " +
              build_graph);

    fs::path exe = out / "bin" / "option-app";
    CHECK(fs::exists(exe), "option-app executable was not produced");
    auto initial_run = run_cmd(exe.string(), dir);
    CHECK(initial_run.success(), "default option executable failed");
    CHECK_EQ(initial_run.stdout, std::string("portable|0\n"),
             "build.cpp or automatic macros lost default bool options");

    auto unchanged = run_option_bake("build");
    CHECK(unchanged.success(), "unchanged option rebuild failed: " + unchanged.stdout);
    CHECK(unchanged.stdout.find("Compiling") == std::string::npos,
          "unchanged options did not reuse build actions: " + unchanged.stdout);
    auto overridden = run_option_bake(
        "build --feature native-backend --feature diagnostics");
    CHECK(overridden.success(), "activated feature build failed: " + overridden.stdout);
    CHECK(overridden.stdout.find("Compiling") != std::string::npos,
          "feature change incorrectly reused stale actions: " + overridden.stdout);

    auto overridden_run = run_cmd(exe.string(), dir);
    CHECK(overridden_run.success(), "activated feature executable failed");
    CHECK_EQ(overridden_run.stdout, std::string("native|1\n"),
             "build.cpp and generated macros disagree on activated features");

    auto unknown = run_option_bake("build --feature missing");
    CHECK(!unknown.success(), "undeclared feature should fail");
    CHECK(unknown.stdout.find("unknown feature 'missing'") != std::string::npos,
          "unknown feature did not report its name: " + unknown.stdout);

    auto run_via_bake = run_option_bake("run");
    CHECK(run_via_bake.success(),
          "bake run could not read out/<target>/.bake/build.json: " +
              run_via_bake.stdout);
    CHECK(run_via_bake.stdout.find("portable|0\n") != std::string::npos,
          "bake run executed the wrong build.cpp output: " +
              run_via_bake.stdout);

    return {};
}

TestResult test_options_reject_non_bool() {
    struct InvalidManifest {
        std::string name;
        std::string extra;
        std::string diagnostic;
    };
    const std::vector<InvalidManifest> invalid = {
        {"legacy_options_table",
         "[options]\nflag = true\n",
         "[options] was replaced by [features]"},
        {"default_not_array",
         "[features]\ndefault = { accelerated = true }\n",
         "features key 'default' must be an array"},
        {"feature_not_table",
         "[features]\naccelerated = true\n",
         "feature 'accelerated' must be a table"},
        {"bad_platform_pattern",
         "[features]\naccelerated = { platforms = [\"x86*-linux\"] }\n",
         "not a valid triple pattern"},
    };

    for (const auto& manifest_case : invalid) {
        auto dir = make_temp_dir("features_manifest_" + manifest_case.name);
        write_file(dir / "bake.toml",
            "[package]\n"
            "name = \"feature-manifest\"\n"
            "version = \"0.1.0\"\n"
            "[language]\nc = \"c17\"\n\n" +
            manifest_case.extra);
        write_file(dir / "src/main.c", "int main(void) { return 0; }\n");

        auto result = run_bake("build", dir);
        CHECK(!result.success(),
              manifest_case.name + " unexpectedly succeeded");
        CHECK(result.stdout.find(manifest_case.diagnostic) !=
                  std::string::npos,
              manifest_case.name + " lacked its diagnostic: " +
                  result.stdout);
    }

    return {};
}

// A feature can pull its own dependencies (resolved only when active) and
// inject compile defines into the declaring package.
TestResult test_feature_activation() {
    auto dir = make_temp_dir("feature_activation");
    copy_fixture("feature_activation", dir);

    auto activated = run_bake("build", dir);
    CHECK(activated.success(),
          "feature-activated build failed: " + activated.stdout);
    auto run_activated = run_cmd(
        (target_output_dir(dir) / "bin" / "feature-app").string(), dir);
    CHECK_EQ(run_activated.exit_code, 0,
             "activated feature path was not taken (expected 8): " +
                 run_activated.stdout);
    CHECK_EQ(run_activated.stdout, std::string("8\n"),
             "feature dependency was not pulled (expected tabdata+1): " +
                 run_activated.stdout);

    // Without the activation the feature dependency is absent and the
    // plain implementation is used.
    auto plain_dir = make_temp_dir("feature_activation_plain");
    copy_fixture("feature_activation", plain_dir);
    write_file(plain_dir / "bake.toml",
        "[package]\n"
        "name = \"feature-app\"\n"
        "version = \"0.1.0\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[dependencies]\n"
        "mathlib = { path = \"mathlib\" }\n");
    auto plain = run_bake("build", plain_dir);
    CHECK(plain.success(), "feature-off build failed: " + plain.stdout);
    auto run_plain = run_cmd(
        (target_output_dir(plain_dir) / "bin" / "feature-app").string(),
        plain_dir);
    CHECK_EQ(run_plain.exit_code, 0,
             "feature-off build failed to run: " + run_plain.stdout);
    CHECK_EQ(run_plain.stdout, std::string("1\n"),
             "feature-off path was not taken: " + run_plain.stdout);

    return {};
}

// Conflicting features activated through the same dependency edge fail at
// configure time and name both features.
TestResult test_feature_conflict() {
    auto dir = make_temp_dir("feature_conflict");
    write_file(dir / "conflicted/bake.toml",
        "[package]\n"
        "name = \"conflicted\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[features]\n"
        "alpha = { defines = [\"ALPHA=1\"], conflicts = [\"beta\"] }\n"
        "beta = { defines = [\"BETA=1\"] }\n");
    write_file(dir / "conflicted/src/v.c",
        "int v(void) { return 1; }\n");
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"conflict-app\"\n"
        "version = \"0.1.0\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[dependencies]\n"
        "conflicted = { path = \"conflicted\", features = [\"alpha\", \"beta\"] }\n");
    write_file(dir / "src/main.c",
        "int v(void);\n"
        "int main(void) { return v(); }\n");

    auto result = run_bake("build", dir);
    CHECK(!result.success(), "conflicting features unexpectedly succeeded");
    CHECK(result.stdout.find(
              "features 'alpha' and 'beta' of package 'conflicted' are "
              "mutually exclusive") != std::string::npos,
          "conflicting features did not name both sides: " + result.stdout);

    return {};
}

// Explicit activation demotes a conflicting default: switching a package's
// default backend from a dependent's edge must succeed, drop the default's
// conditional dependency from the graph, and note the demotion.
TestResult test_feature_demotion() {
    auto dir = make_temp_dir("feature_demotion");
    write_file(dir / "legacylib/bake.toml",
        "[package]\n"
        "name = \"legacylib\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n");
    write_file(dir / "legacylib/src/legacy.c",
        "int legacy_value(void) { return 10; }\n");
    write_file(dir / "modernlib/bake.toml",
        "[package]\n"
        "name = \"modernlib\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n");
    write_file(dir / "modernlib/src/modern.c",
        "int modern_value(void) { return 20; }\n");
    write_file(dir / "backends/bake.toml",
        "[package]\n"
        "name = \"backends\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[features]\n"
        "default = [\"legacy\"]\n"
        "legacy = { defines = [\"BACKEND=1\"], dependencies = "
        "{ legacylib = { path = \"../legacylib\" } } }\n"
        "modern = { defines = [\"BACKEND=2\"], conflicts = [\"legacy\"], "
        "dependencies = { modernlib = { path = \"../modernlib\" } } }\n");
    write_file(dir / "backends/src/v.c",
        "int v(void) { return 1; }\n");
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"demote-app\"\n"
        "version = \"0.1.0\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[dependencies]\n"
        "backends = { path = \"backends\", features = [\"modern\"] }\n");
    write_file(dir / "src/main.c",
        "#include <stdio.h>\n"
        "int v(void);\n"
        "int modern_value(void);\n"
        "int main(void) {\n"
        "    printf(\"%d\\n%d\\n\", v(), modern_value());\n"
        "    return 0;\n"
        "}\n");

    auto switched = run_bake("build", dir);
    CHECK(switched.success(),
          "explicit backend switch over a default failed: " + switched.stdout);
    CHECK(switched.stdout.find(
              "feature 'legacy' of package 'backends' is disabled") !=
              std::string::npos,
          "demotion was not reported: " + switched.stdout);
    CHECK(fs::exists(target_output_dir(dir) / "lib" / "libmodernlib.a"),
          "modern backend library missing from output");
    CHECK(!fs::exists(target_output_dir(dir) / "lib" / "liblegacylib.a"),
          "demoted default's library leaked into the build");
    auto run_switched = run_cmd(
        (target_output_dir(dir) / "bin" / "demote-app").string(), dir);
    CHECK_EQ(run_switched.stdout, std::string("1\n20\n"),
             "switched build did not use the modern backend: " +
                 run_switched.stdout);

    // A CLI activation on the root demotes the root's own default.
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"demote-root\"\n"
        "version = \"0.1.0\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[features]\n"
        "default = [\"legacy\"]\n"
        "legacy = { defines = [\"BACKEND=1\"], dependencies = "
        "{ legacylib = { path = \"legacylib\" } } }\n"
        "modern = { defines = [\"BACKEND=2\"], conflicts = [\"legacy\"], "
        "dependencies = { modernlib = { path = \"modernlib\" } } }\n");
    write_file(dir / "src/main.c",
        "#include <stdio.h>\n"
        "int modern_value(void);\n"
        "int main(void) {\n"
        "    printf(\"%d\\n%d\\n\", BACKEND, modern_value());\n"
        "    return 0;\n"
        "}\n");
    auto root_cli = run_bake("build --feature modern", dir);
    CHECK(root_cli.success(),
          "CLI activation did not demote the root's default: " +
              root_cli.stdout);
    auto run_root = run_cmd(
        (target_output_dir(dir) / "bin" / "demote-root").string(), dir);
    CHECK_EQ(run_root.stdout, std::string("2\n20\n"),
             "CLI-demoted root build kept the legacy backend: " +
                 run_root.stdout);

    return {};
}

// default-features = false on a dependency suppresses that edge's
// contribution of the target's default feature set: only explicitly named
// features activate. Union semantics hold — another edge (or building the
// package as its own root) still contributes the defaults, with a warning.
TestResult test_feature_default_off() {
    auto write_lib = [&](const fs::path& root) {
        write_file(root / "lib/bake.toml",
            "[package]\n"
            "name = \"lib\"\n"
            "version = \"0.1.0\"\n"
            "type = \"lib\"\n"
            "[language]\nc = \"c17\"\n\n"
            "[features]\n"
            "default = [\"extra\"]\n"
            "extra = { defines = [\"LIB_EXTRA=1\"] }\n");
        write_file(root / "lib/src/v.c",
            "int v(void) {\n"
            "#ifdef LIB_EXTRA\n"
            "return 2;\n"
            "#else\n"
            "return 1;\n"
            "#endif\n"
            "}\n");
    };
    auto write_main = [&](const fs::path& root) {
        write_file(root / "src/main.c",
            "#include <stdio.h>\n"
            "int v(void);\n"
            "int main(void) {\n"
            "#ifdef LIB_EXTRA\n"
            "printf(\"extra %d\\n\", v());\n"
            "#else\n"
            "printf(\"plain %d\\n\", v());\n"
            "#endif\n"
            "return 0;\n"
            "}\n");
    };

    // Opt-out edge: the default feature's define is gone everywhere.
    auto off = make_temp_dir("feature_default_off");
    write_lib(off);
    write_file(off / "bake.toml",
        "[package]\n"
        "name = \"off-app\"\n"
        "version = \"0.1.0\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[dependencies]\n"
        "lib = { path = \"lib\", default-features = false }\n");
    write_main(off);
    auto off_build = run_bake("build", off);
    CHECK(off_build.success(), "default-features=false build failed: " +
                                   off_build.stdout);
    auto run_off = run_cmd(
        (target_output_dir(off) / "bin" / "off-app").string(), off);
    CHECK_EQ(run_off.stdout, std::string("plain 1\n"),
             "default-features=false kept the default feature: " +
                 run_off.stdout);

    // Plain edge: defaults stay on (and reach consumers transitively).
    auto on = make_temp_dir("feature_default_on");
    write_lib(on);
    write_file(on / "bake.toml",
        "[package]\n"
        "name = \"on-app\"\n"
        "version = \"0.1.0\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[dependencies]\n"
        "lib = { path = \"lib\" }\n");
    write_main(on);
    auto on_build = run_bake("build", on);
    CHECK(on_build.success(), "plain dependency build failed: " +
                                  on_build.stdout);
    auto run_on = run_cmd(
        (target_output_dir(on) / "bin" / "on-app").string(), on);
    CHECK_EQ(run_on.stdout, std::string("extra 2\n"),
             "plain dependency lost its default feature: " + run_on.stdout);

    // Mixed edges: union keeps the defaults and warns about it.
    auto mixed = make_temp_dir("feature_default_mixed");
    write_lib(mixed);
    write_file(mixed / "via-off/bake.toml",
        "[package]\n"
        "name = \"via-off\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[dependencies]\n"
        "lib = { path = \"../lib\", default-features = false }\n");
    write_file(mixed / "via-off/src/p.c", "int via_off(void) { return 0; }\n");
    write_file(mixed / "via-on/bake.toml",
        "[package]\n"
        "name = \"via-on\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[dependencies]\n"
        "lib = { path = \"../lib\" }\n");
    write_file(mixed / "via-on/src/q.c", "int via_on(void) { return 0; }\n");
    write_file(mixed / "bake.toml",
        "[package]\n"
        "name = \"mixed-app\"\n"
        "version = \"0.1.0\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[dependencies]\n"
        "via-off = { path = \"via-off\" }\n"
        "via-on = { path = \"via-on\" }\n");
    write_main(mixed);
    auto mixed_build = run_bake("build", mixed);
    CHECK(mixed_build.success(),
          "mixed-edge build failed: " + mixed_build.stdout);
    CHECK(mixed_build.stdout.find("keeps default features") !=
              std::string::npos,
          "mixed edges did not warn about retained defaults: " +
              mixed_build.stdout);
    auto run_mixed = run_cmd(
        (target_output_dir(mixed) / "bin" / "mixed-app").string(), mixed);
    CHECK_EQ(run_mixed.stdout, std::string("extra 2\n"),
             "mixed edges dropped the union default feature: " +
                 run_mixed.stdout);

    return {};
}


// A platform-restricted feature in the default set is a silent no-op on
// non-matching targets; activating it explicitly on such a target is an
// error.
TestResult test_feature_platform() {
    auto dir = make_temp_dir("feature_platform");
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"platform-app\"\n"
        "version = \"0.1.0\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[features]\n"
        "default = [\"win-only\"]\n"
        "win-only = { platforms = [\"*-windows-gnu\"], defines = [\"PLATFORM_LEAK=1\"] }\n");
    write_file(dir / "src/main.c",
        "#ifdef PLATFORM_LEAK\n"
        "#error \"platform-restricted feature leaked into a non-matching build\"\n"
        "#endif\n"
        "int main(void) { return 0; }\n");

    auto silent = run_bake("build", dir);
    CHECK(silent.success(),
          "default platform-restricted feature broke a non-matching target: " +
              silent.stdout);

    auto explicit_build = run_bake("build --feature win-only", dir);
    CHECK(!explicit_build.success(),
          "explicit activation on an unsupported platform unexpectedly "
          "succeeded");
    CHECK(explicit_build.stdout.find("does not support target") !=
              std::string::npos,
          "unsupported activation did not explain itself: " +
              explicit_build.stdout);

    return {};
}

// Module interfaces compiled by build.cpp need BMI paths for the complete
// import closure. A direct-only mapping makes the third module unable to load
// declarations exported through the second module.
TestResult test_build_cpp_transitive_modules() {
    // clang-scan-deps reports canonical paths; keep the fixture root canonical
    // too so macOS's /var -> /private/var alias does not hide module sources.
    auto dir = fs::weakly_canonical(
        make_temp_dir("build_cpp_transitive_modules"));
    copy_fixture("build_cpp_transitive_modules", dir);

    auto build = run_bake("build", dir);
    CHECK(build.success(),
          "build.cpp could not compile transitive module imports: " +
              build.stdout);

    fs::path exe = target_output_dir(dir) / "bin" / "transitive-modules";
    CHECK(fs::exists(exe), "transitive module executable was not produced");
    auto run = run_cmd(exe.string(), dir);
    CHECK(run.success(), "transitive module executable returned a failure");

    return {};
}

// Default input discovery must provide std.compat so end-user C++ can use
// global compatibility names (size_t, uintmax_t) via `import std.compat;`.
TestResult test_std_compat_default_discovery() {
    auto dir = make_temp_dir("std_compat_default_discovery");
    copy_fixture("std_compat_default_discovery", dir);

    auto build = run_bake("build", dir);
    CHECK(build.success(),
          "default-discovered inputs with import std.compat failed: " +
              build.stdout);

    const fs::path out = target_output_dir(dir);
    fs::path exe = out / "bin" / "std-compat-default-discovery";
    CHECK(fs::exists(exe),
          "std.compat default-discovery executable was not produced");
    auto run = run_cmd(exe.string(), dir);
    CHECK(run.success(),
          "std.compat default-discovery executable returned failure");

    // Toolchain PCMs are now in the global cache, not project-local.
    fs::path local_std = out / ".bmi" / ".std";
    CHECK(!fs::exists(local_std),
          "out/.bmi/.std should not exist (std PCMs moved to global cache)");

    // The std module is compiler-provided (the bake c++ shim injects it at
    // -std=c++23); compile_commands.json records the build system's plain
    // argv and must not synthesize std module flags.
    auto cc = read_file(dir / "compile_commands.json");
    CHECK(cc.find("-fmodule-file=std=") == std::string::npos,
          "compile_commands must not synthesize -fmodule-file=std=");

    return {};
}

// build.cpp path must also provide std.compat.
TestResult test_std_compat_build_cpp() {
    auto dir = make_temp_dir("std_compat_build_cpp");
    copy_fixture("std_compat_build_cpp", dir);

    auto build = run_bake("build", dir);
    CHECK(build.success(),
          "build.cpp with import std.compat failed: " + build.stdout);

    const fs::path out = target_output_dir(dir);
    fs::path exe = out / "bin" / "std-compat-build-cpp";
    CHECK(fs::exists(exe), "std.compat build.cpp executable was not produced");
    auto run = run_cmd(exe.string(), dir);
    CHECK(run.success(), "std.compat build.cpp executable returned failure");

    // Toolchain PCMs are now in the global cache, not project-local.
    fs::path local_std = out / ".bmi" / ".std";
    CHECK(!fs::exists(local_std),
          "out/.bmi/.std should not exist (std PCMs moved to global cache)");

    // bake.build.pcm / bake.build.o must not be in project-local scripts dir.
    fs::path scripts = out / ".bake" / "scripts";
    if (fs::exists(scripts)) {
        for (auto& e : fs::directory_iterator(scripts)) {
            if (fs::is_directory(e)) {
                for (auto& f : fs::directory_iterator(e)) {
                    std::string name = f.path().filename().string();
                    CHECK(name != "bake.build.pcm",
                          "bake.build.pcm should not be in project-local scripts");
                    CHECK(name != "bake.build.o",
                          "bake.build.o should not be in project-local scripts");
                }
            }
        }
    }

    return {};
}

// Verify that toolchain artifacts (std.pcm, std.compat.pcm, bake.build.pcm)
// are stored in the global content-addressed cache and shared across projects.
// Project A builds them; project B must reuse them without recompilation.
TestResult test_cache_sharing() {
    auto base = make_temp_dir("cache_sharing");
    auto cache_dir = base / "tc-cache";
    auto proj_a = base / "proj_a";
    auto proj_b = base / "proj_b";

    // Both projects use import std — reuse the default-discovery fixture.
    copy_fixture("std_compat_default_discovery", proj_a);
    copy_fixture("std_compat_default_discovery", proj_b);

    std::string env = "BAKE_CACHE_DIR=" + cache_dir.string();

    // --- Build project A (cold cache) ---
    auto build_a = run_bake("build", proj_a, env);
    CHECK(build_a.success(),
          "project A build failed: " + build_a.stdout);

    const fs::path out_a = target_output_dir(proj_a);
    fs::path exe_a = out_a / "bin" / "std-compat-default-discovery";
    CHECK(fs::exists(exe_a), "project A executable was not produced");

    // Cache must contain std.pcm/std.compat.pcm (manifest layout:
    // <triple>/o/<hash>/std.pcm) — scan triple dirs generically.
    bool found_std = false, found_compat = false;
    if (fs::exists(cache_dir)) {
        std::error_code ec;
        for (auto& triple_entry : fs::directory_iterator(cache_dir, ec)) {
            if (!triple_entry.is_directory()) continue;
            fs::path o_dir = triple_entry.path() / "o";
            if (!fs::exists(o_dir)) continue;
            for (auto& hash_entry : fs::directory_iterator(o_dir, ec)) {
                if (!hash_entry.is_directory()) continue;
                if (fs::exists(hash_entry.path() / "std.pcm")) found_std = true;
                if (fs::exists(hash_entry.path() / "std.compat.pcm"))
                    found_compat = true;
            }
        }
    }
    CHECK(found_std, "std.pcm not found in global cache after project A build");
    CHECK(found_compat,
          "std.compat.pcm not found in global cache after project A build");

    // Project A's out/ must not contain .bmi/.std.
    CHECK(!fs::exists(out_a / ".bmi" / ".std"),
          "project A out/.bmi/.std should not exist");

    // Record the build output for A (should show "Preparing standard library module").
    bool a_prepared_std =
        build_a.stdout.find("Preparing standard library module") != std::string::npos;

    // --- Build project B (warm cache — should reuse) ---
    auto build_b = run_bake("build", proj_b, env);
    CHECK(build_b.success(),
          "project B build failed: " + build_b.stdout);

    const fs::path out_b = target_output_dir(proj_b);
    fs::path exe_b = out_b / "bin" / "std-compat-default-discovery";
    CHECK(fs::exists(exe_b), "project B executable was not produced");
    auto run_b = run_cmd(exe_b.string(), proj_b);
    CHECK(run_b.success(), "project B executable returned failure");

    // Project B should NOT have recompiled std modules (cache hit).
    bool b_prepared_std =
        build_b.stdout.find("Preparing standard library module") != std::string::npos;
    CHECK(!b_prepared_std,
          "project B should not recompile std modules (cache miss expected hit)");

    // Project B's out/ must not contain .bmi/.std either.
    CHECK(!fs::exists(out_b / ".bmi" / ".std"),
          "project B out/.bmi/.std should not exist");

    // If project A actually prepared the std module, we have a solid
    // before/after comparison. If not (cache was pre-warmed by an earlier
    // test in the same HOME), both builds hit cache — still a valid pass.
    (void)a_prepared_std;

    return {};
}

// Remote dependency archives must extract with the platform tar. This uses a
// local file:// Git repository and archive, so it exercises the real resolver
// without relying on the network. In particular, macOS bsdtar does not support
// GNU tar's --no-overwrite-dir option.
TestResult test_remote_archive_extract() {
#ifdef _WIN32
    return {};
#else
    auto dir = make_temp_dir("remote_archive_extract");
    auto remote = dir / "remote";
    auto project = dir / "project";
    auto test_home = dir / "home";

    fs::create_directories(remote / "src");
    fs::create_directories(test_home);
    copy_fixture("simple_app", project);

    fs::create_directories(remote / "public");
    write_file(remote / "public" / "remote_fixture.hpp",
        "#pragma once\nint remote_archive_fixture();\n");
    write_file(remote / "src" / "fixture.cpp",
        "#include <remote_fixture.hpp>\n"
        "int remote_archive_fixture() { return 42; }\n");

    auto init = run_cmd("git init -q", remote);
    CHECK(init.success(), "failed to initialize local remote: " + init.stdout);
    CHECK(run_cmd("git config user.email bake-test@example.invalid", remote).success(),
          "failed to configure fixture git email");
    CHECK(run_cmd("git config user.name bake-test", remote).success(),
          "failed to configure fixture git name");
    CHECK(run_cmd("git config commit.gpgsign false", remote).success(),
          "failed to disable fixture commit signing");
    CHECK(run_cmd("git add public/remote_fixture.hpp src/fixture.cpp", remote).success(),
          "failed to stage fixture repository");
    auto commit = run_cmd("git commit -q -m fixture", remote);
    CHECK(commit.success(), "failed to commit fixture repository: " + commit.stdout);
    CHECK(run_cmd("git tag v1.0", remote).success(),
          "failed to tag fixture repository");

    auto rev = run_cmd("git rev-parse HEAD", remote);
    CHECK(rev.success(), "failed to resolve fixture commit: " + rev.stdout);
    while (!rev.stdout.empty() &&
           (rev.stdout.back() == '\n' || rev.stdout.back() == '\r')) {
        rev.stdout.pop_back();
    }
    CHECK(!rev.stdout.empty(), "fixture commit was empty");

    fs::create_directories(remote / "archive");
    auto archive = remote / "archive" / (rev.stdout + ".tar.gz");
    auto make_archive = run_cmd(
        "git archive --format=tar.gz --prefix=remote-archive-fixture/ -o " +
            archive.string() + " HEAD",
        remote);
    CHECK(make_archive.success(),
          "failed to create fixture archive: " + make_archive.stdout);

    write_file(project / "bake.toml",
        "[package]\n"
        "name = \"remote-archive-app\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\ncxx = \"c++17\"\n\n"
        "[dependencies]\n"
        "fixture = { url = \"file://" + remote.string() +
            "\", tag = \"v1.0\" }\n"
    );

    auto build = run_bake("build", project, "HOME=" + test_home.string());
    CHECK(build.success(),
          "local remote dependency failed to extract/build: " + build.stdout);
    CHECK(fs::exists(project / "bake.lock"),
          "remote dependency build did not write bake.lock");

    return {};
#endif
}

// A direct archive dependency (.tar.gz) whose content declares a moid:
// download, safe extraction, tree hash, graph node, exports — the full
// pipeline through file:// so no network is needed.
TestResult test_archive_dependency_tar() {
#ifdef _WIN32
    return {};
#else
    auto dir = make_temp_dir("archive_dependency_tar");
    auto project = dir / "project";
    auto test_home = dir / "home";
    auto staging = dir / "staging";
    auto lib = staging / "fixture-archive-lib";

    fs::create_directories(lib / "public");
    fs::create_directories(lib / "src");
    fs::create_directories(test_home);
    copy_fixture("simple_app", project);

    write_file(lib / "bake.toml",
        "[package]\n"
        "name = \"fixture-archive-lib\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\ncxx = \"c++17\"\n");
    write_file(lib / "public" / "fixture_archive.hpp",
        "#pragma once\nint fixture_archive_value();\n");
    write_file(lib / "src" / "lib.cpp",
        "#include <fixture_archive.hpp>\n"
        "int fixture_archive_value() { return 7; }\n");

    auto archive = dir / "fixture-archive-lib.tar.gz";
    auto pack = run_cmd("tar czf " + archive.string() + " -C " +
                            staging.string() + " fixture-archive-lib",
                        dir);
    CHECK(pack.success(), "failed to pack fixture archive: " + pack.stdout);

    write_file(project / "bake.toml",
        "[package]\n"
        "name = \"archive-dep-app\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\ncxx = \"c++17\"\n\n"
        "[dependencies]\n"
        "fixture = { url = \"file://" + archive.string() + "\" }\n");
    write_file(project / "src" / "main.cpp",
        "#include <fixture_archive.hpp>\n"
        "int main() { return fixture_archive_value() == 7 ? 0 : 1; }\n");

    auto build = run_bake("build", project, "HOME=" + test_home.string());
    CHECK(build.success(), "archive dependency build failed: " + build.stdout);
    CHECK(fs::exists(project / "bake.lock"),
          "archive dependency build did not write bake.lock");
    auto lock = read_file(project / "bake.lock");
    CHECK(lock.find("archive:") != std::string::npos,
          "lock entry for archive dep should use the archive: locator key");

    return {};
#endif
}

// Zip archives take the unzip path (prescan via `unzip -l`, extract via
// `unzip`); skipped when the host lacks zip tooling.
TestResult test_archive_dependency_zip() {
#ifdef _WIN32
    return {};
#else
    auto dir = make_temp_dir("archive_dependency_zip");
    if (!run_cmd("which zip", dir).success()) return {};

    auto project = dir / "project";
    auto test_home = dir / "home";
    auto staging = dir / "staging";
    auto lib = staging / "fixture-zip-lib";

    fs::create_directories(lib / "public");
    fs::create_directories(lib / "src");
    fs::create_directories(test_home);
    copy_fixture("simple_app", project);

    write_file(lib / "bake.toml",
        "[package]\n"
        "name = \"fixture-zip-lib\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\ncxx = \"c++17\"\n");
    write_file(lib / "public" / "fixture_zip.hpp",
        "#pragma once\nint fixture_zip_value();\n");
    write_file(lib / "src" / "lib.cpp",
        "#include <fixture_zip.hpp>\n"
        "int fixture_zip_value() { return 9; }\n");

    auto archive = dir / "fixture-zip-lib.zip";
    auto pack = run_cmd("zip -q -r " + archive.string() + " fixture-zip-lib",
                        staging);
    CHECK(pack.success(), "failed to pack fixture zip: " + pack.stdout);

    write_file(project / "bake.toml",
        "[package]\n"
        "name = \"zip-dep-app\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\ncxx = \"c++17\"\n\n"
        "[dependencies]\n"
        "fixture = { url = \"file://" + archive.string() + "\" }\n");
    write_file(project / "src" / "main.cpp",
        "#include <fixture_zip.hpp>\n"
        "int main() { return fixture_zip_value() == 9 ? 0 : 1; }\n");

    auto build = run_bake("build", project, "HOME=" + test_home.string());
    CHECK(build.success(), "zip dependency build failed: " + build.stdout);

    return {};
#endif
}

// Locked-as-hints: an unchanged archive dependency must be carried over
// verbatim on re-resolve (no re-download), while a newly added dependency
// downloads for the first time.
TestResult test_incremental_lock_carry() {
#ifdef _WIN32
    return {};
#else
    auto dir = make_temp_dir("incremental_lock_carry");
    auto project = dir / "project";
    auto test_home = dir / "home";
    auto staging = dir / "staging";

    fs::create_directories(test_home);
    fs::create_directories(staging);
    copy_fixture("simple_app", project);

    auto make_archive_dep = [&](const std::string& name, int value) {
        std::string ident = name;
        std::replace(ident.begin(), ident.end(), '-', '_');
        auto lib = staging / name;
        fs::create_directories(lib / "public");
        fs::create_directories(lib / "src");
        write_file(lib / "bake.toml",
            "[package]\n"
            "name = \"" + name + "-lib\"\n"
            "version = \"0.1.0\"\n"
            "type = \"lib\"\n"
            "[language]\ncxx = \"c++17\"\n");
        write_file(lib / "public" / (name + ".hpp"),
            "#pragma once\nint " + ident + "_value();\n");
        write_file(lib / "src" / "lib.cpp",
            "#include <" + name + ".hpp>\n"
            "int " + ident + "_value() { return " +
                std::to_string(value) + "; }\n");
        auto archive = dir / (name + ".tar.gz");
        auto pack = run_cmd("tar czf " + archive.string() + " -C " +
                                staging.string() + " " + name,
                            dir);
        if (!pack.success()) return std::string();
        return archive.string();
    };

    auto first = make_archive_dep("fixture-carry-a", 3);
    CHECK(!first.empty(), "failed to pack fixture-carry-a");
    auto second = make_archive_dep("fixture-carry-b", 4);
    CHECK(!second.empty(), "failed to pack fixture-carry-b");

    write_file(project / "bake.toml",
        "[package]\n"
        "name = \"carry-app\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\ncxx = \"c++17\"\n\n"
        "[dependencies]\n"
        "a = { url = \"file://" + first + "\" }\n");
    write_file(project / "src" / "main.cpp",
        "#include <fixture-carry-a.hpp>\n"
        "int main() { return fixture_carry_a_value() == 3 ? 0 : 1; }\n");

    std::string home_env = "HOME=" + test_home.string();
    auto build1 = run_bake("build", project, home_env);
    CHECK(build1.success(), "first build failed: " + build1.stdout);
    CHECK(build1.stdout.find("Downloading fixture-carry-a") !=
              std::string::npos,
          "first build should download the archive dependency");

    // Add a second dependency; the first must be carried from the lock.
    std::string manifest = read_file(project / "bake.toml");
    manifest += "b = { url = \"file://" + second + "\" }\n";
    write_file(project / "bake.toml", manifest);

    auto build2 = run_bake("build", project, home_env);
    CHECK(build2.success(), "second build failed: " + build2.stdout);
    CHECK(build2.stdout.find("Downloading fixture-carry-a") ==
              std::string::npos,
          "unchanged dependency must be carried from the lock, not "
          "re-downloaded: " + build2.stdout);
    CHECK(build2.stdout.find("Downloading fixture-carry-b") !=
              std::string::npos,
          "new dependency must be resolved: " + build2.stdout);

    auto lock = read_file(project / "bake.lock");
    CHECK(lock.find("fixture-carry-a") == std::string::npos ||
              lock.find("archive:") != std::string::npos,
          "lock should use archive: locator keys");

    return {};
#endif
}

// bake remove drops the entry from the right scope, prunes orphaned lock
// entries, leaves the content cache untouched, and preserves manifest
// comments. add --target writes target-scoped entries with table creation.
TestResult test_add_remove_commands() {
#ifdef _WIN32
    return {};
#else
    auto dir = make_temp_dir("add_remove_commands");
    auto project = dir / "project";
    auto test_home = dir / "home";
    auto staging = dir / "staging";
    fs::create_directories(test_home);
    fs::create_directories(staging);
    copy_fixture("simple_app", project);

    write_file(project / "bake.toml",
        "[package]\n"
        "name = \"remove-app\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\ncxx = \"c++17\"\n\n"
        "# favored deps live here\n"
        "[dependencies]\n");
    write_file(project / "src" / "main.cpp", "int main() { return 0; }\n");

    auto make_archive = [&](const std::string& name) {
        std::string ident = name;
        std::replace(ident.begin(), ident.end(), '-', '_');
        auto lib = staging / name;
        fs::create_directories(lib / "public");
        fs::create_directories(lib / "src");
        write_file(lib / "bake.toml",
            "[package]\n"
            "name = \"" + name + "-lib\"\n"
            "version = \"0.1.0\"\n"
            "type = \"lib\"\n"
            "[language]\ncxx = \"c++17\"\n");
        write_file(lib / "public" / (name + ".hpp"),
            "#pragma once\nint " + ident + "_value();\n");
        write_file(lib / "src" / "lib.cpp",
            "#include <" + name + ".hpp>\n"
            "int " + ident + "_value() { return 1; }\n");
        auto archive = dir / (name + ".tar.gz");
        auto pack = run_cmd("tar czf " + archive.string() + " -C " +
                                staging.string() + " " + name,
                            dir);
        if (!pack.success()) return std::string();
        return archive.string();
    };

    auto first = make_archive("fixture-rm-a");
    CHECK(!first.empty(), "failed to pack fixture-rm-a");
    auto second = make_archive("fixture-rm-b");
    CHECK(!second.empty(), "failed to pack fixture-rm-b");
    std::string home_env = "HOME=" + test_home.string();

    // Flag validation.
    CHECK(!run_bake("add " + first + " --tag v1 --branch main", project)
              .success(),
          "conflicting ref flags must be rejected");
    CHECK(!run_bake("add " + first + " --tag v1", project).success(),
          "archive with a ref flag must be rejected");
    CHECK(!run_bake("add https://example.com/x.git --tag v1 --target "
                    "\"x86*-linux\"",
                    project)
              .success(),
          "invalid --target glob must be rejected");

    // Global add + target-scoped add (creates the table).
    auto add_a = run_bake("add " + first, project);
    CHECK(add_a.success(), "add archive failed: " + add_a.stdout);
    auto add_b = run_bake("add " + second + " --target \"*-linux-musl\"",
                          project);
    CHECK(add_b.success(), "target-scoped add failed: " + add_b.stdout);

    auto manifest = read_file(project / "bake.toml");
    CHECK(manifest.find("# favored deps live here") != std::string::npos,
          "manifest comment must survive adds: " + manifest);
    CHECK(manifest.find("[target.\"*-linux-musl\".dependencies]") !=
              std::string::npos,
          "target-scoped add must create the table: " + manifest);
    CHECK(manifest.find("fixture-rm-b = { url = ") != std::string::npos,
          "target-scoped entry must be written: " + manifest);

    // Build: lock covers the union of scopes (both entries), even though
    // the native build graph never sees the musl-scoped one.
    auto build = run_bake("build", project, home_env);
    CHECK(build.success(), "build with archive deps failed: " + build.stdout);
    auto lock_text = read_file(project / "bake.lock");
    CHECK(lock_text.find("fixture-rm-a.tar.gz") != std::string::npos ||
              lock_text.find("archive:") != std::string::npos,
          "lock should contain archive entries");

    // Remember the cache directories before removal.
    auto cache_root = test_home / ".cache" / "bake" / "src";
    std::size_t cache_entries_before = 0;
    for (auto it = fs::directory_iterator(cache_root);
         it != fs::directory_iterator(); ++it)
        if (it->is_directory()) ++cache_entries_before;
    CHECK(cache_entries_before >= 2,
          "both archives should be cached before remove");

    // Remove the target-scoped entry with the scope pin-pointed.
    auto rm_b = run_bake("remove fixture-rm-b --target \"*-linux-musl\"",
                         project);
    CHECK(rm_b.success(), "target-scoped remove failed: " + rm_b.stdout);
    manifest = read_file(project / "bake.toml");
    CHECK(manifest.find("fixture-rm-b") == std::string::npos,
          "removed entry must be gone from the manifest");
    CHECK(manifest.find("fixture-rm-a") != std::string::npos,
          "other entries must survive");
    CHECK(manifest.find("# favored deps live here") != std::string::npos,
          "manifest comment must survive remove");

    // The lock was pruned of the removed entry's reach.
    lock_text = read_file(project / "bake.lock");
    CHECK(lock_text.find("fixture-rm-b") == std::string::npos,
          "lock must be pruned of removed entries");

    // Removing an ambiguous/unknown alias errors with guidance.
    CHECK(!run_bake("remove no-such-dep", project).success(),
          "removing an unknown dependency must fail");
    CHECK(!run_bake("remove fixture-rm-b", project).success(),
          "removing an already-removed dependency must fail");

    // Cache is untouched by removals.
    std::size_t cache_entries_after = 0;
    for (auto it = fs::directory_iterator(cache_root);
         it != fs::directory_iterator(); ++it)
        if (it->is_directory()) ++cache_entries_after;
    CHECK(cache_entries_after == cache_entries_before,
          "remove must never touch the content cache");

    // Build still works after removal.
    auto rebuild = run_bake("build", project, home_env);
    CHECK(rebuild.success(), "build after remove failed: " + rebuild.stdout);

    return {};
#endif
}

// Target-scoped dependencies are visible only to matching build triples:
// a musl-scoped lib dep makes the native build fail (header missing) and
// the musl cross build succeed, and only the matching graph contains it.
TestResult test_target_scoped_dependency() {
    auto dir = make_temp_dir("target_scoped_dependency");
    auto project = dir / "project";
    auto test_home = dir / "home";
    auto staging = dir / "staging";
    auto lib = staging / "fixture-scope-lib";

    fs::create_directories(lib / "public");
    fs::create_directories(lib / "src");
    fs::create_directories(test_home);
    copy_fixture("simple_app", project);

    write_file(lib / "bake.toml",
        "[package]\n"
        "name = \"fixture-scope-lib\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\ncxx = \"c++17\"\n");
    write_file(lib / "public" / "fixture_scope.hpp",
        "#pragma once\nint fixture_scope_value();\n");
    write_file(lib / "src" / "lib.cpp",
        "#include <fixture_scope.hpp>\n"
        "int fixture_scope_value() { return 5; }\n");

    auto archive = dir / "fixture-scope-lib.tar.gz";
    auto pack = run_cmd("tar czf " + archive.string() + " -C " +
                            staging.string() + " fixture-scope-lib",
                        dir);
    CHECK(pack.success(), "failed to pack scope fixture: " + pack.stdout);

    write_file(project / "bake.toml",
        "[package]\n"
        "name = \"scope-app\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\ncxx = \"c++17\"\n\n"
        "[target.\"*-linux-musl\".dependencies]\n"
        "fixture = { url = \"file://" + archive.string() + "\" }\n");
    write_file(project / "src" / "main.cpp",
        "#include <fixture_scope.hpp>\n"
        "int main() { return fixture_scope_value() == 5 ? 0 : 1; }\n");

    std::string home_env = "HOME=" + test_home.string();

    // Native build: the musl-scoped dependency is filtered out, so the
    // header cannot resolve.
    auto native = run_bake("build", project, home_env);
    CHECK(!native.success(),
          "native build must not see the musl-scoped dependency");
    CHECK(native.stdout.find("fixture_scope.hpp") != std::string::npos,
          "native failure should be the missing header: " + native.stdout);

    // Cross build for musl: the dependency enters the graph and the build
    // succeeds.
    auto musl = run_bake("build -t x86_64-linux-musl", project, home_env);
    CHECK(musl.success(),
          "musl build must see the scoped dependency: " + musl.stdout);
    CHECK(fs::exists(project / "out" / "x86_64-linux-musl" / "bin" /
                     "scope-app"),
          "musl binary missing");

    return {};
}

// Declaring a raw source dependency and never consuming it draws a
// configure-time warning; consuming it via dep_src_dir silences the warning.
TestResult test_unused_source_dep_warning() {
#ifdef _WIN32
    return {};
#else
    auto dir = make_temp_dir("unused_source_dep");
    auto unused_project = dir / "unused";
    auto used_project = dir / "used";
    auto test_home = dir / "home";
    auto staging = dir / "staging" / "fixture-raw-src";

    fs::create_directories(staging / "src");
    fs::create_directories(test_home);
    fs::create_directories(unused_project / "src");
    fs::create_directories(used_project);

    // A raw source package: no bake.toml, just sources.
    write_file(staging / "src" / "raw_main.c",
        "int raw_served_main(void) { return 0; }\n");
    auto archive = dir / "fixture-raw-src.tar.gz";
    auto pack = run_cmd("tar czf " + archive.string() + " -C " +
                            (dir / "staging").string() + " fixture-raw-src",
                        dir);
    CHECK(pack.success(), "failed to pack raw source fixture: " + pack.stdout);

    std::string dep_url = "file://" + archive.string();

    // Unused: default discovery has no way to consume a source dependency.
    write_file(unused_project / "bake.toml",
        "[package]\n"
        "name = \"unused-src-dep\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\ncxx = \"c++17\"\n\n"
        "[dependencies]\n"
        "raw = { url = \"" + dep_url + "\" }\n");
    write_file(unused_project / "src" / "main.cpp",
        "int main() { return 0; }\n");

    std::string home_env = "HOME=" + test_home.string();
    auto unused_build = run_bake("build", unused_project, home_env);
    CHECK(unused_build.success(),
          "unused source dep build should still succeed: " +
              unused_build.stdout);
    CHECK(unused_build.stdout.find("'raw'") != std::string::npos &&
              unused_build.stdout.find("never consumed") !=
                  std::string::npos,
          "expected unused-source-dep warning: " + unused_build.stdout);

    // Consumed: build.cpp pulls the raw sources in via dep_src_dir.
    write_file(used_project / "bake.toml",
        "[package]\n"
        "name = \"used-src-dep\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\ncxx = \"c++17\"\n\n"
        "[dependencies]\n"
        "raw = { url = \"" + dep_url + "\" }\n");
    write_file(used_project / "build.cpp",
        "import bake.build;\n"
        "import std;\n"
        "int main() {\n"
        "    bake::Builder b;\n"
        "    std::string dir(b.dep_src_dir(\"raw\"));\n"
        "    if (dir.empty()) return 1;\n"
        "    b.sources(\"src/main.cpp\");\n"
        "    b.sources(dir + \"/src/raw_main.c\");\n"
        "    return b.build();\n"
        "}\n");
    write_file(used_project / "src" / "main.cpp",
        "int main() { return 0; }\n");

    auto used_build = run_bake("build", used_project, home_env);
    CHECK(used_build.success(),
          "consumed source dep build failed: " + used_build.stdout);
    CHECK(used_build.stdout.find("never consumed") == std::string::npos,
          "no warning expected when dep_src_dir is used: " + used_build.stdout);

    return {};
#endif
}


// A Lib moid with module interfaces must include the module interface objects
// in its archive action's inputs and depends_on, alongside regular source
// objects. This verifies that module objects enter the link/archive DAG.
TestResult test_module_archive_edges() {
    auto dir = make_temp_dir("module_archive_edges");
    copy_fixture("module_archive", dir);

    auto build = run_bake("build -j 1", dir);
    CHECK(build.success(),
          "module_archive workspace build failed: " + build.stdout);

    const fs::path out = target_output_dir(dir);
    fs::path exe = out / "bin" / "consumer";
    CHECK(fs::exists(exe), "consumer executable was not produced");
    auto run = run_cmd(exe.string(), dir);
    CHECK_EQ(run.exit_code, 0,
             "consumer executable returned non-zero: " + run.stdout);

    // Verify the archive exists.
    fs::path archive = out / "lib" / "libmodlib.a";
    CHECK(fs::exists(archive), "modlib static archive was not produced");

    // Inspect graph.json for module object → archive producer edges.
    const std::string graph =
        read_file(out / ".bake" / "graph.json");
    CHECK(!graph.empty(), "module_archive did not persist graph.json");

    auto action_for_in = [](const std::string& document,
                            std::string_view moid,
                            std::string_view type)
            -> std::optional<std::string> {
        const std::string needle =
            "\"moid\": \"" + std::string(moid) + "\"";
        std::size_t position = 0;
        while ((position = document.find(needle, position)) !=
               std::string::npos) {
            const std::size_t begin = document.rfind('{', position);
            const std::size_t end = document.find('}', position);
            if (begin == std::string::npos || end == std::string::npos) {
                return std::nullopt;
            }
            std::string action = document.substr(begin, end - begin + 1);
            if (json_scalar_field(action, "moid") == moid &&
                json_scalar_field(action, "type") == type) {
                return action;
            }
            position += needle.size();
        }
        return std::nullopt;
    };
    auto action_for = [&](std::string_view moid, std::string_view type) {
        return action_for_in(graph, moid, type);
    };

    auto json_string_array = [](std::string_view array) {
        std::vector<std::string> values;
        for (std::size_t position = 0; position < array.size();) {
            if (array[position] != '"') {
                ++position;
                continue;
            }
            std::string value;
            bool escaped = false;
            for (++position; position < array.size(); ++position) {
                const char c = array[position];
                if (escaped) {
                    value += c;
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    ++position;
                    break;
                } else {
                    value += c;
                }
            }
            values.push_back(std::move(value));
        }
        return values;
    };

    auto occurrences = [](const std::vector<std::string>& values,
                          const std::string& expected) {
        std::size_t count = 0;
        for (const auto& value : values) {
            if (value == expected) ++count;
        }
        return count;
    };

    // Find the modlib archive action and verify it depends on module compiles.
    auto archive_action = action_for("modlib", "archive");
    CHECK(archive_action.has_value(),
          "modlib archive action is missing from graph.json");

    auto archive_depends_json =
        json_value_field(*archive_action, "depends_on");
    CHECK(archive_depends_json.has_value(),
          "modlib archive action has no depends_on");
    const auto archive_deps = json_string_array(*archive_depends_json);

    auto archive_inputs_json =
        json_value_field(*archive_action, "inputs");
    CHECK(archive_inputs_json.has_value(),
          "modlib archive action has no inputs");
    const auto archive_inputs = json_string_array(*archive_inputs_json);

    // Count module compile vs regular compile dependencies.
    std::size_t module_compile_deps = 0;
    std::size_t regular_compile_deps = 0;
    for (const auto& dep_id : archive_deps) {
        if (dep_id.find("module:") == 0)
            ++module_compile_deps;
        else if (dep_id.find("compile:") == 0)
            ++regular_compile_deps;
    }
    CHECK_EQ(module_compile_deps, std::size_t(2),
             "archive must depend on both module interface compile actions");
    CHECK_EQ(regular_compile_deps, std::size_t(1),
             "archive must depend on the regular source compile action");

    // The archive inputs must include all three objects (two module + one src).
    std::size_t object_count = 0;
    for (const auto& input : archive_inputs) {
        if (fs::path(input).extension() == ".o")
            ++object_count;
    }
    CHECK_EQ(object_count, std::size_t(3),
             "archive must contain three objects (two module + one source)");

    // Verify the consumer's link action depends on the archive producer.
    auto link_action = action_for("consumer", "link");
    CHECK(link_action.has_value(),
          "consumer link action is missing from graph.json");
    auto link_depends_json = json_value_field(*link_action, "depends_on");
    CHECK(link_depends_json.has_value(),
          "consumer link action has no depends_on");
    const auto link_deps = json_string_array(*link_depends_json);

    auto archive_action_id = json_scalar_field(*archive_action, "id");
    CHECK(archive_action_id.has_value(), "archive action has no id");
    CHECK_EQ(occurrences(link_deps, *archive_action_id), std::size_t(1),
             "consumer link must depend on the archive producer");

    return {};
}

// graph.json must be a faithful round-trip of the build graph: the number of
// actions, their IDs, types, and dependency edges must survive serialization.
TestResult test_graph_json_round_trip() {
    auto dir = make_temp_dir("graph_json_round_trip");
    copy_fixture("moid_outputs", dir);

    auto build = run_bake("build -j 1", dir);
    CHECK(build.success(), "graph.json fixture build failed: " + build.stdout);

    const std::string graph_str =
        read_file(target_output_dir(dir) / ".bake" / "graph.json");
    CHECK(!graph_str.empty(), "graph.json was not written");

    // Every action must have a unique id, a type, and a depends_on array.
    std::vector<std::string> action_ids;
    std::size_t position = 0;
    while ((position = graph_str.find("\"id\"", position)) !=
           std::string::npos) {
        auto id = json_scalar_field(
            std::string_view(graph_str).substr(position), "id");
        if (id) action_ids.push_back(*id);
        position += 4;
    }
    CHECK(action_ids.size() > 0, "graph.json has no action ids");

    for (std::size_t i = 0; i < action_ids.size(); ++i) {
        for (std::size_t j = i + 1; j < action_ids.size(); ++j) {
            CHECK(action_ids[i] != action_ids[j],
                  "graph.json has duplicate action id '" + action_ids[i] + "'");
        }
    }

    // Every depends_on entry must reference an existing action id.
    std::set<std::string> id_set(action_ids.begin(), action_ids.end());
    position = 0;
    while ((position = graph_str.find("\"depends_on\"", position)) !=
           std::string::npos) {
        auto deps_json = json_value_field(
            std::string_view(graph_str).substr(position), "depends_on");
        position += 12;
        if (!deps_json) continue;
        auto deps = [&]() {
            std::vector<std::string> values;
            for (std::size_t p = 0; p < deps_json->size();) {
                if ((*deps_json)[p] != '"') { ++p; continue; }
                std::string value;
                bool escaped = false;
                for (++p; p < deps_json->size(); ++p) {
                    char c = (*deps_json)[p];
                    if (escaped) { value += c; escaped = false; }
                    else if (c == '\\') escaped = true;
                    else if (c == '"') { ++p; break; }
                    else value += c;
                }
                values.push_back(std::move(value));
            }
            return values;
        }();
        for (const auto& dep : deps) {
            CHECK(id_set.count(dep) > 0,
                  "graph.json depends_on references unknown action '" + dep + "'");
        }
    }

    // The build output must not contain a round-trip warning.
    CHECK(build.stdout.find("graph.json round-trip mismatch") ==
              std::string::npos,
          "graph.json round-trip verification reported a mismatch");

    return {};
}

// Built-in profiles require no manifest declaration. A custom profile uses
// the same semantic fields and must affect both compile and link actions.
TestResult test_profile_configuration() {
    auto dir = make_temp_dir("profile_configuration");
    copy_fixture("profile_configuration", dir);

    auto dev = run_bake("build", dir);
    CHECK(dev.success(), "implicit dev profile failed: " + dev.stdout);
    auto commands = read_file(dir / "compile_commands.json");
    CHECK(contains_command_token(commands, "-O0") &&
              contains_command_token(commands, "-g") &&
              contains_command_token(commands, "-Wall"),
          "implicit dev profile flags are incomplete: " + commands);
    CHECK(!contains_command_token(commands, "-flto=thin"),
          "dev profile leaked release-only flags: " + commands);
    const fs::path exe = native_executable_path(
        target_output_dir(dir), "profile-configuration");
    auto dev_run = run_cmd(exe.string(), dir);
    CHECK(dev_run.success() && dev_run.stdout == "NDEBUG=0\n",
          "dev profile unexpectedly defined NDEBUG: " + dev_run.stdout);

    auto release = run_bake("build --release", dir);
    CHECK(release.success(), "implicit release profile failed: " + release.stdout);
    commands = read_file(dir / "compile_commands.json");
    CHECK(contains_command_token(commands, "-O3") &&
              contains_command_token(commands, "-flto=thin") &&
              contains_command_token(commands, "-Wall"),
          "implicit release profile flags are incomplete: " + commands);
    CHECK(!contains_command_token(commands, "-g"),
          "release profile unexpectedly emitted full debug information: " +
              commands);
    auto release_run = run_cmd(exe.string(), dir);
    CHECK(release_run.success() && release_run.stdout == "NDEBUG=1\n",
          "release profile did not define NDEBUG: " + release_run.stdout);

    std::string manifest = read_file(dir / "bake.toml");
    manifest += "\n[profile.release]\n"
                "opt = 1\n"
                "debug = true\n"
                "lto = false\n"
                "warnings = \"none\"\n";
    write_file(dir / "bake.toml", manifest);
    auto overridden_release = run_bake("build --release", dir);
    CHECK(overridden_release.success(),
          "overridden release profile failed: " + overridden_release.stdout);
    commands = read_file(dir / "compile_commands.json");
    CHECK(contains_command_token(commands, "-O1") &&
              contains_command_token(commands, "-g") &&
              !contains_command_token(commands, "-flto=thin") &&
              !contains_command_token(commands, "-Wall"),
          "user release profile did not override built-in fields: " + commands);
    auto overridden_run = run_cmd(exe.string(), dir);
    CHECK(overridden_run.success() && overridden_run.stdout == "NDEBUG=1\n",
          "overriding release fields removed NDEBUG: " + overridden_run.stdout);

    auto bench = run_bake("build --profile bench", dir);
    CHECK(bench.success(), "custom bench profile failed: " + bench.stdout);
    commands = read_file(dir / "compile_commands.json");
    CHECK(contains_command_token(commands, "-Oz") &&
              contains_command_token(commands, "-gline-tables-only") &&
              contains_command_token(commands, "-flto") &&
              contains_command_token(commands, "-Wall") &&
              contains_command_token(commands, "-Wextra") &&
              contains_command_token(commands, "-Werror"),
          "custom profile compile flags are incomplete: " + commands);
    CHECK(!contains_command_token(commands, "-flto=thin"),
          "full LTO was incorrectly converted to thin LTO: " + commands);

    const std::string graph = read_file(
        target_output_dir(dir) / ".bake/graph.json");
    CHECK(contains_command_token(graph, "-Wl,-S"),
          "custom profile strip flag is absent from the link action: " + graph);

    auto missing = run_bake("build --profile does-not-exist", dir);
    CHECK(!missing.success(), "unknown profile unexpectedly succeeded");
    CHECK(missing.stdout.find("does-not-exist") != std::string::npos &&
              missing.stdout.find("profile") != std::string::npos,
          "unknown profile diagnostic omitted its name: " + missing.stdout);

    return {};
}

// Broad and exact target tables both contribute. Exact flags are appended
// last, non-matching tables contribute nothing, and frameworks are ignored on
// a non-Darwin target without turning into linker arguments.
TestResult test_target_conditions() {
    auto dir = make_temp_dir("target_conditions");
    copy_fixture("target_conditions", dir);

    auto build = run_bake("build -t x86_64-linux-musl", dir);
    CHECK(build.success(), "target-conditioned build failed: " + build.stdout);

    const fs::path out = dir / "out/x86_64-linux-musl";
    CHECK(fs::is_directory(out),
          "explicit target did not use out/x86_64-linux-musl");
    CHECK(fs::exists(out / "bin/target-conditions"),
          "target-conditioned executable was not produced");

    const std::string commands = read_file(dir / "compile_commands.json");
    const auto profile = commands.find("-Wall");
    const auto broad = commands.find("-DTARGET_PRIORITY=1");
    const auto undef = commands.find("-UTARGET_PRIORITY");
    const auto exact = commands.find("-DTARGET_PRIORITY=2");
    CHECK(profile != std::string::npos && broad != std::string::npos &&
              undef != std::string::npos && exact != std::string::npos &&
              profile < broad && broad < undef && undef < exact,
          "target flags were not merged from broad to specific: " + commands);
    CHECK(commands.find("WRONG_ARCH_MATCH") == std::string::npos,
          "non-matching target defines leaked into compile actions: " + commands);

    const std::string graph = read_file(out / ".bake/graph.json");
    CHECK(contains_command_token(graph, "-lm"),
          "target library was not added to the link action: " + graph);
    CHECK(graph.find("DefinitelyIgnoredOutsideDarwin") == std::string::npos,
          "non-Darwin link action retained a framework: " + graph);

    auto darwin_dir = make_temp_dir("darwin_framework_declaration");
    write_file(darwin_dir / "bake.toml",
        "[package]\n"
        "name = \"darwin-framework-declaration\"\n"
        "version = \"0.1.0\"\n"
        "type = \"lib\"\n"
        "[language]\nc = \"c17\"\n\n"
        "[target.\"*-apple-darwin\"]\n"
        "frameworks = [\"Foundation\"]\n");
    write_file(darwin_dir / "src/value.c", "int value(void) { return 1; }\n");
    auto darwin = run_bake("build -t aarch64-apple-darwin", darwin_dir);
    CHECK(darwin.success(),
          "Darwin framework target table failed to configure: " +
              darwin.stdout);
    auto declaration = find_moid_declaration(
        darwin_dir / "out/aarch64-apple-darwin/.bake");
    CHECK(declaration.has_value(),
          "Darwin framework declaration was not persisted");
    const auto frameworks =
        json_value_field(read_file(*declaration), "frameworks");
    CHECK(frameworks.has_value() &&
              frameworks->find("\"Foundation\"") != std::string::npos,
          "matching Darwin framework was not added to the declaration");

    auto invalid_dir = make_temp_dir("invalid_target_pattern");
    copy_fixture("target_conditions", invalid_dir);
    std::string manifest = read_file(invalid_dir / "bake.toml");
    manifest += "\n[target.\"x86*-linux-musl\"]\n"
                "defines = [\"INVALID_PARTIAL_WILDCARD=1\"]\n";
    write_file(invalid_dir / "bake.toml", manifest);
    auto invalid = run_bake("build -t x86_64-linux-musl", invalid_dir);
    CHECK(!invalid.success(), "partial-segment target wildcard was accepted");
    CHECK(invalid.stdout.find("x86*") != std::string::npos &&
              invalid.stdout.find("target") != std::string::npos,
          "invalid target wildcard lacked a precise diagnostic: " +
              invalid.stdout);

    auto four_segment = run_bake(
        "build -t x86_64-unknown-linux-musl", dir);
    CHECK(!four_segment.success(),
          "four-segment target triple was accepted");
    CHECK(four_segment.stdout.find("x86_64-unknown-linux-musl") !=
              std::string::npos &&
              four_segment.stdout.find("target") != std::string::npos,
          "four-segment target lacked a three-segment diagnostic: " +
              four_segment.stdout);

    return {};
}

// ── glibc cross target ──

struct Elf64Info {
    bool valid = false;
    std::string interp;
    std::vector<std::string> needed;
    std::vector<std::string> glibc_versions;  // GLIBC_x.y strings in dynstr
};

// Minimal ELF64 little-endian inspector: PT_INTERP, DT_NEEDED, and the
// GLIBC_* version strings from the dynamic string table. Runs on any host.
static Elf64Info inspect_elf64(const fs::path& p) {
    Elf64Info info;
    std::ifstream f(p, std::ios::binary);
    if (!f) return info;
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) -> uint64_t {
        return b[o] | (b[o + 1] << 8); };
    auto u64 = [&](size_t o) -> uint64_t {
        return u16(o) | (u16(o + 2) << 16) | (u16(o + 4) << 32) |
               (u16(o + 6) << 48); };
    if (b.size() < 64 || b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' ||
        b[3] != 'F' || b[4] != 2 /*ELF64*/ || b[5] != 1 /*LE*/)
        return info;
    info.valid = true;

    size_t phoff = u64(32);
    size_t phentsize = u16(54), phnum = u16(56);
    size_t dyn_off = 0, dyn_sz = 0;
    std::vector<std::pair<uint64_t, uint64_t>> loads;  // vaddr → file off
    std::vector<std::pair<uint64_t, uint64_t>> dyn;    // tag → val
    for (size_t i = 0; i < phnum; ++i) {
        size_t ph = phoff + i * phentsize;
        uint64_t type = u16(ph);
        if (type == 3) {  // PT_INTERP
            size_t off = u64(ph + 8), sz = u64(ph + 32);
            info.interp.assign(reinterpret_cast<char*>(b.data()) + off,
                               strnlen(reinterpret_cast<char*>(b.data()) + off,
                                       sz));
        } else if (type == 1) {  // PT_LOAD
            loads.emplace_back(u64(ph + 16), u64(ph + 8));
        } else if (type == 2) {  // PT_DYNAMIC
            dyn_off = u64(ph + 8);
            dyn_sz = u64(ph + 32);
        }
    }
    if (dyn_off == 0) return info;
    auto vaddr_to_off = [&](uint64_t v) -> size_t {
        for (auto& [va, off] : loads)
            if (v >= va && v - va <= (1ull << 40)) return off + (v - va);
        return static_cast<size_t>(v);  // identity for most linkers
    };
    size_t strtab = 0;
    for (size_t o = dyn_off; o + 16 <= dyn_off + dyn_sz; o += 16) {
        uint64_t tag = u64(o), val = u64(o + 8);
        dyn.emplace_back(tag, val);
        if (tag == 5) strtab = val;  // DT_STRTAB
    }
    if (strtab == 0) return info;
    size_t strtab_off = vaddr_to_off(strtab);
    auto dynstr = [&](uint64_t off) -> std::string {
        size_t o = strtab_off + off;
        if (o >= b.size()) return {};
        return std::string(reinterpret_cast<char*>(b.data()) + o,
                           strnlen(reinterpret_cast<char*>(b.data()) + o,
                                   b.size() - o));
    };
    for (auto& [tag, val] : dyn) {
        if (tag == 1) info.needed.push_back(dynstr(val));  // DT_NEEDED
    }
    // GLIBC_* version strings live in the dynamic string table.
    size_t end = b.size();
    for (size_t o = strtab_off; o < end; ++o) {
        if (o + 6 <= end && std::memcmp(b.data() + o, "GLIBC_", 6) == 0 &&
            (o == strtab_off || b[o - 1] == '\0')) {
            size_t e = o;
            while (e < end && b[e] != '\0') ++e;
            info.glibc_versions.emplace_back(
                reinterpret_cast<char*>(b.data()) + o, e - o);
            o = e;
        }
    }
    return info;
}

TestResult test_cross_gnu() {
    // C pipeline: compile + link against the vendored glibc subset and
    // synthesized stubs, then verify the ELF structure from the host.
    auto dir = make_temp_dir("cross_gnu");
    copy_fixture("cross_gnu", dir);

    auto build = run_bake("build -t x86_64-linux-gnu", dir);
    CHECK(build.success(), "glibc cross build failed: " + build.stdout);

    const fs::path out = dir / "out/x86_64-linux-gnu/bin/cross-gnu";
    CHECK(fs::exists(out), "glibc cross executable missing");

    auto elf = inspect_elf64(out);
    CHECK(elf.valid, "output is not a valid ELF64 binary");
    CHECK(elf.interp == "/lib64/ld-linux-x86-64.so.2",
          "unexpected PT_INTERP: " + elf.interp);
    bool has_libc = false;
    for (auto& n : elf.needed) {
        if (n == "libc.so.6") has_libc = true;
        CHECK(n != "libpthread.so.0" && n != "libdl.so.2" &&
                  n != "librt.so.1" && n != "libutil.so.1",
              "unused glibc stub leaked into DT_NEEDED: " + n);
    }
    CHECK(has_libc, "libc.so.6 missing from DT_NEEDED");
    bool saw_glibc_ver = false;
    for (auto& v : elf.glibc_versions)
        if (v.find("GLIBC_2.") == 0) saw_glibc_ver = true;
    CHECK(saw_glibc_ver, "no GLIBC_* version references in dynamic strings");

    // aarch64 variant.
    auto arm = run_bake("build -t aarch64-linux-gnu", dir);
    CHECK(arm.success(), "aarch64 glibc cross build failed: " + arm.stdout);
    auto arm_elf = inspect_elf64(dir / "out/aarch64-linux-gnu/bin/cross-gnu");
    CHECK(arm_elf.valid && arm_elf.interp == "/lib/ld-linux-aarch64.so.1",
          "aarch64 PT_INTERP wrong: " + arm_elf.interp);

    // Explicit glibc version via triple suffix: still builds — the
    // header surface follows the target version (see cross_gnu_prereq),
    // and symbol versions resolve from the per-version stubs.
    auto dir31 = make_temp_dir("cross_gnu_231");
    copy_fixture("cross_gnu", dir31);
    auto v31 = run_bake("build -t x86_64-linux-gnu.2.31", dir31);
    CHECK(v31.success(), "glibc 2.31 target build failed: " + v31.stdout);
    CHECK(fs::exists(dir31 / "out/x86_64-linux-gnu/bin/cross-gnu"),
          "versioned-target build produced no binary in the triple dir");

    // Static linking on glibc is rejected with musl guidance.
    auto stat = run_bake(
        "cc -target x86_64-linux-gnu -static " +
            (dir / "src/main.c").string() + " -o " +
            (dir / "static-app").string(),
        dir);
    CHECK(!stat.success(), "static glibc link unexpectedly succeeded");
    CHECK(stat.stdout.find("musl") != std::string::npos,
          "static-glibc diagnostic lacks musl guidance: " + stat.stdout);

    return {};
}

TestResult test_cross_gnu_prereq() {
    // features.h version pinning: headers are vendored from the newest
    // glibc, and __GLIBC_MINOR__ is pinned to the target version, so
    // __GLIBC_PREREQ gates present the requested surface.
    auto dir = make_temp_dir("cross_gnu_prereq");
    copy_fixture("cross_gnu_prereq", dir);

    // 2.36 target: the post-2.28 API (gettid, glibc 2.30) is visible,
    // links, and carries the GLIBC_2.30 version reference.
    auto v36 = run_bake("build -t x86_64-linux-gnu.2.36", dir);
    CHECK(v36.success(), "glibc 2.36 prereq build failed: " + v36.stdout);
    const fs::path out = dir / "out/x86_64-linux-gnu/bin/cross-gnu-prereq";
    CHECK(fs::exists(out), "2.36 target binary missing");
    auto elf = inspect_elf64(out);
    CHECK(elf.valid, "2.36 output is not a valid ELF64 binary");
    bool has_2_30 = false;
    for (auto& v : elf.glibc_versions)
        if (v == "GLIBC_2.30") has_2_30 = true;
    CHECK(has_2_30, "gettid reference missing its GLIBC_2.30 version");

    // Default (2.28) target: the same source must be rejected at
    // compile time — the declaration is version-gated away.
    auto base = run_bake("build -t x86_64-linux-gnu", dir);
    CHECK(!base.success(),
          "2.28 target unexpectedly accepted a 2.30-gated API");

    return {};
}

TestResult test_with_asan() {
    // Native asan on whatever host the suite runs on: the runtime is
    // built from the vendored compiler-rt sources into the global cache
    // (per-platform form — static archive or dylib). The faulting
    // program aborts with a report.
    auto dir = make_temp_dir("with_asan");
    copy_fixture("with_asan", dir);

    auto build = run_bake("build", dir);
    CHECK(build.success(), "asan native build failed: " + build.stdout);
    const fs::path exe =
        native_executable_path(target_output_dir(dir), "with-asan");
    CHECK(fs::exists(exe), "asan executable missing");

    auto run = run_cmd(exe.string(), dir);
    CHECK(!run.success(),
          "asan-detected overflow must abort: " + run.stdout);
    CHECK(run.stdout.find("AddressSanitizer: heap-buffer-overflow") !=
              std::string::npos,
          "asan report missing from output: " + run.stdout);
    CHECK(run.stdout.find("SUMMARY: AddressSanitizer") != std::string::npos,
          "asan SUMMARY line missing: " + run.stdout);

    return {};
}

TestResult test_with_ubsan() {
    // Native ubsan on whatever host the suite runs on. The faulting
    // program reports and (default recover mode) keeps running.
    auto dir = make_temp_dir("with_ubsan");
    copy_fixture("with_ubsan", dir);

    auto build = run_bake("build", dir);
    CHECK(build.success(), "ubsan native build failed: " + build.stdout);
    const fs::path exe =
        native_executable_path(target_output_dir(dir), "with-ubsan");
    CHECK(fs::exists(exe), "ubsan executable missing");

    auto run = run_cmd(exe.string(), dir);
    CHECK(run.stdout.find("runtime error: division by zero") !=
              std::string::npos,
          "ubsan report missing from output: " + run.stdout);
    CHECK(run.stdout.find("SUMMARY: UndefinedBehaviorSanitizer") !=
              std::string::npos,
          "ubsan SUMMARY line missing: " + run.stdout);

    // Runtimes without a vendored counterpart are rejected at link with
    // a clear message. (fuzzer: every platform's driver accepts it, so
    // the rejection comes from bake's link interception.)
    auto fz = run_bake("cc -fsanitize=fuzzer src/main.c -o fuzzer-rejected",
                       dir);
    CHECK(!fz.success() && fz.stdout.find("not vendored") != std::string::npos,
          "fuzzer link should be rejected with a clear message");

    return {};
}

TestResult test_cross_gnu_cpp() {
    // C++ pipeline: import std against the gnu libc++ config.
    auto dir = make_temp_dir("cross_gnu_cpp");
    copy_fixture("cross_gnu_cpp", dir);

    auto build = run_bake("build -t x86_64-linux-gnu", dir);
    CHECK(build.success(), "glibc C++ cross build failed: " + build.stdout);
    const fs::path out = dir / "out/x86_64-linux-gnu/bin/cross-gnu-cpp";
    CHECK(fs::exists(out), "glibc C++ executable missing");
    auto elf = inspect_elf64(out);
    CHECK(elf.valid && elf.interp == "/lib64/ld-linux-x86-64.so.2",
          "C++ glibc PT_INTERP wrong: " + elf.interp);

    return {};
}

TestResult test_default_source_extensions() {
    auto dir = make_temp_dir("default_source_extensions");
    copy_fixture("default_source_extensions", dir);

    auto build = run_bake("build", dir);
    CHECK(build.success(),
          "default source extensions did not build: " + build.stdout);
    const fs::path exe = native_executable_path(
        target_output_dir(dir), "default-source-extensions");
    CHECK(fs::exists(exe), "default-extension executable was not produced");
    auto run = run_cmd(exe.string(), dir);
    CHECK(run.success(),
          "one or more default source/header/module extensions were omitted");

    const std::string commands = read_file(dir / "compile_commands.json");
    for (const std::string_view source : {
             "main.cpp", "c_value.c", "cc_value.cc", "cxx_value.cxx",
             "mm_value.mm", "detail.cppm", "api.ixx"}) {
        CHECK(commands.find(source) != std::string::npos,
              "compile_commands omitted default extension source '" +
                  std::string(source) + "': " + commands);
    }

    return {};
}

TestResult test_header_only_dependency() {
    auto dir = make_temp_dir("header_only_dependency");
    copy_fixture("header_only_dependency", dir);

    auto build = run_bake("build", dir);
    CHECK(build.success(),
          "header-only dependency build failed: " + build.stdout);
    const fs::path out = target_output_dir(dir);
    const fs::path exe =
        native_executable_path(out, "header-only-consumer");
    CHECK(fs::exists(exe), "header-only consumer was not produced");
    auto run = run_cmd(exe.string(), dir);
    CHECK(run.success(),
          "header include dirs or public option/package defines were not forwarded");

#ifdef _WIN32
    const fs::path unexpected_archive = out / "lib/header-only.lib";
#else
    const fs::path unexpected_archive = out / "lib/libheader-only.a";
#endif
    CHECK(!fs::exists(unexpected_archive),
          "pure header library unexpectedly produced an archive");
    const std::string graph = read_file(out / ".bake/graph.json");
    CHECK(contains_command_token(graph, "-lm"),
          "header-only dependency did not forward its link libraries: " + graph);

    return {};
}

TestResult test_build_cpp_test_orchestration() {
    auto dir = make_temp_dir("build_cpp_tests");
    copy_fixture("build_cpp_tests", dir);

    auto default_test = run_bake("test", dir);
    CHECK(default_test.success(),
          "bake test could not build and run the default test: " +
              default_test.stdout);
    CHECK(default_test.stdout.find("BAKE_TEST_PARSER_RAN\n") !=
              std::string::npos &&
              default_test.stdout.find("BAKE_TEST_STRING_RAN\n") ==
              std::string::npos &&
              default_test.stdout.find("BAKE_TEST_INTEGRATION_RAN\n") ==
              std::string::npos,
          "the last set_default() call did not uniquely select parser-suite: " +
              default_test.stdout);

    const fs::path out = target_output_dir(dir);
#ifdef _WIN32
    const std::string executable_suffix = ".exe";
    const fs::path main_library = out / "lib/build-cpp-tests.lib";
#else
    const std::string executable_suffix;
    const fs::path main_library = out / "lib/libbuild-cpp-tests.a";
#endif
    CHECK(fs::exists(main_library),
          "build.cpp-declared tests did not preserve default-discovered main inputs");
    for (const std::string_view binary : {
             "unit_string", "unit_parser", "integration"}) {
        CHECK(fs::exists(out / "bin" /
                         (std::string(binary) + executable_suffix)),
              "registered test binary was not produced: " +
                  std::string(binary));
    }

    auto named = run_bake("test string-suite", dir);
    CHECK(named.success(), "named test failed: " + named.stdout);
    CHECK(named.stdout.find("BAKE_TEST_STRING_RAN\n") != std::string::npos &&
              named.stdout.find("BAKE_TEST_PARSER_RAN\n") == std::string::npos &&
              named.stdout.find("BAKE_TEST_INTEGRATION_RAN\n") ==
              std::string::npos,
          "named test did not run only its mapped binary: " + named.stdout);

    auto all = run_bake("test --all", dir);
    CHECK(all.success(), "bake test --all failed: " + all.stdout);
    CHECK(all.stdout.find("BAKE_TEST_STRING_RAN\n") != std::string::npos &&
              all.stdout.find("BAKE_TEST_PARSER_RAN\n") != std::string::npos &&
              all.stdout.find("BAKE_TEST_INTEGRATION_RAN\n") !=
              std::string::npos,
          "bake test --all did not run every registered test: " + all.stdout);

    return {};
}

// A main lib can use fully custom input discovery and still contribute
// several binaries. Top-level input calls describe the bake.toml-defined
// main lib; they do not create a second moid or change its configuration.
TestResult test_build_cpp_custom_lib_with_binaries() {
    auto dir = make_temp_dir("build_cpp_custom_lib_binaries");
    copy_fixture("build_cpp_custom_lib_binaries", dir);

    auto build = run_bake("build", dir);
    CHECK(build.success(),
          "custom-input main lib with binaries failed: " + build.stdout);
    const fs::path out = target_output_dir(dir);

#ifdef _WIN32
    const fs::path main_library = out / "lib/custom-input-lib.lib";
#else
    const fs::path main_library = out / "lib/libcustom-input-lib.a";
#endif
    CHECK(fs::exists(main_library),
          "build.cpp did not produce the bake.toml-defined main lib");

    const fs::path tool_a =
        native_executable_path(out, "custom-tool-a");
    const fs::path tool_b =
        native_executable_path(out, "custom-tool-b");
    CHECK(fs::exists(tool_a) && fs::exists(tool_b),
          "build.cpp did not produce every additional binary");
    auto run_a = run_cmd(tool_a.string(), dir);
    auto run_b = run_cmd(tool_b.string(), dir);
    CHECK(run_a.success() && run_b.success(),
          "additional binaries did not inherit the main lib and public headers");

    const std::string graph = read_file(out / ".bake/graph.json");
    CHECK(graph.find("must_not_compile.cpp") == std::string::npos,
          "explicit main-lib inputs did not replace default source discovery");
    CHECK(graph.find("custom/src/value.cpp") != std::string::npos &&
              graph.find("tools/tool_a.cpp") != std::string::npos &&
              graph.find("tools/tool_b.cpp") != std::string::npos,
          "custom main-lib or binary inputs are absent from the build graph: " +
              graph);

    return {};
}

TestResult test_target_output_isolation() {
    auto dir = make_temp_dir("target_output_isolation");
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"target-output-isolation\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "[language]\nc = \"c17\"\n");
    write_file(dir / "src/main.c", "int main(void) { return 0; }\n");

    auto native = run_bake("build", dir);
    CHECK(native.success(), "native isolation build failed: " + native.stdout);
    auto outputs = target_output_dirs(dir);
    CHECK_EQ(outputs.size(), std::size_t(1),
             "native isolation build did not create one target directory");
    const std::string native_target = outputs.front().filename().string();
    const std::string cross_target = native_target == "x86_64-linux-musl"
        ? "x86_64-windows-gnu"
        : "x86_64-linux-musl";

    auto cross = run_bake("build -t " + cross_target, dir);
    CHECK(cross.success(), "cross-target isolation build failed: " + cross.stdout);
    outputs = target_output_dirs(dir);
    CHECK_EQ(outputs.size(), std::size_t(2),
             "native and cross builds did not retain two isolated target roots");

    for (const auto& target : {native_target, cross_target}) {
        const fs::path out = dir / "out" / target;
        CHECK(fs::is_directory(out / "bin") &&
                  fs::is_directory(out / ".obj") &&
                  fs::is_directory(out / ".bake"),
              "target output is incomplete for " + target);
        CHECK(!read_file(out / ".bake/graph.json").empty(),
              "target graph is missing for " + target);
        CHECK(!read_file(out / ".bake/fingerprints.json").empty(),
              "target fingerprints are missing for " + target);
    }
    CHECK(!fs::exists(dir / "out/bin") &&
              !fs::exists(dir / "out/.bake"),
          "multi-target build leaked state into the target-less output root");

    return {};
}

// ----------------------------------------------------------------
// Test registry
// ----------------------------------------------------------------

static std::vector<TestCase> all_tests = {
    {"with_ubsan",                   test_with_ubsan},
    {"with_asan",                    test_with_asan},
    {"cross_gnu",                     test_cross_gnu},
    {"cross_gnu_cpp",                 test_cross_gnu_cpp},
    {"cross_gnu_prereq",              test_cross_gnu_prereq},
    {"invalid_moid_type",             test_invalid_moid_type},
    {"input_declaration_equivalence", test_input_declaration_equivalence},
    {"declaration_json_escape",       test_declaration_json_escape},
    {"declaration_reader_validation", test_declaration_reader_validation},
    {"static_lib_build",              test_static_lib_build},
    {"pure_c_build",                  test_pure_c_build},
    {"path_dep_build",                test_path_dep_build},
    {"moid_outputs",                  test_moid_outputs},
    {"missing_path_dependency",       test_missing_path_dependency},
    {"path_dep_locked",               test_path_dep_locked},
    {"frozen_no_lock",                test_frozen_no_lock},
    {"lock_consistency",              test_lock_consistency},
    {"lock_transitive_consistency",   test_lock_transitive_consistency},
    {"frozen_missing_cache",          test_frozen_missing_cache},
    {"add_duplicate",                 test_add_duplicate},
    {"add_duplicate_compact",         test_add_duplicate_compact},
    {"add_no_tag",                    test_add_no_tag},
    {"target_output_layout",          test_target_output_layout},
    {"clean",                         test_clean},
    {"init",                          test_init},
    {"init_c",                        test_init_c},
    {"version",                       test_version},
    {"update_single_dep",             test_update_single_dep},
    {"standalone_path_dep_build",     test_standalone_path_dep_build},
    {"standalone_path_dep_locked",    test_standalone_path_dep_locked},
    {"workspace_target_output",       test_workspace_target_output},
    {"canonical_engine_namespaces",   test_canonical_engine_namespaces},
    {"canonical_terminal_collision",  test_canonical_terminal_collision},
    {"archive_rebuild_drops_removed_objects", test_archive_rebuild_drops_removed_objects},
    {"archive_failure_is_atomic", test_archive_failure_is_atomic},
    {"terminal_case_collision",       test_terminal_case_collision},
    {"terminal_output_escape",        test_terminal_output_escape},
    {"workspace_member_filter",       test_workspace_member_filter},
    {"workspace_selection_identity",  test_workspace_selection_identity},
    {"workspace_duplicate_canonical_member", test_workspace_duplicate_canonical_member},
    {"workspace_symlink_selector",    test_workspace_symlink_selector},
    {"executable_dependency",         test_executable_dependency},
    {"run_build_cpp_declaration",     test_run_build_cpp_declaration},
    {"build_cpp_binary_module_imports", test_build_cpp_binary_module_imports},
    {"build_cpp_target_env",          test_build_cpp_target_env},
    {"header_incremental_rebuild",    test_header_incremental_rebuild},
    {"dependency_binaries_stay_out",  test_dependency_binaries_stay_out},
    {"source_less_executable_rejects_stale_output", test_source_less_executable_rejects_stale_output},
    {"run_requires_member_for_multiple_executables", test_run_requires_member_for_multiple_executables},
    {"default_discovery_meta_dependency", test_default_discovery_meta_dependency},
    {"overlapping_source_groups",     test_overlapping_source_groups},
    {"symlink_source_identity",       test_symlink_source_identity},
    {"build_cpp_options",             test_build_cpp_options},
    {"options_reject_non_bool",       test_options_reject_non_bool},
    {"feature_activation",            test_feature_activation},
    {"feature_conflict",              test_feature_conflict},
    {"feature_platform",              test_feature_platform},
    {"feature_demotion",              test_feature_demotion},
    {"feature_default_off",           test_feature_default_off},
    {"build_cpp_transitive_modules",  test_build_cpp_transitive_modules},
    {"std_compat_default_discovery",  test_std_compat_default_discovery},
    {"std_compat_build_cpp",          test_std_compat_build_cpp},
    {"cache_sharing",                 test_cache_sharing},
    {"remote_archive_extract",        test_remote_archive_extract},
    {"archive_dependency_tar",        test_archive_dependency_tar},
    {"archive_dependency_zip",        test_archive_dependency_zip},
    {"incremental_lock_carry",        test_incremental_lock_carry},
    {"add_remove_commands",          test_add_remove_commands},
    {"target_scoped_dependency",      test_target_scoped_dependency},
    {"unused_source_dep_warning",     test_unused_source_dep_warning},
    {"module_archive_edges",          test_module_archive_edges},
    {"graph_json_round_trip",         test_graph_json_round_trip},
    {"profile_configuration",         test_profile_configuration},
    {"target_conditions",             test_target_conditions},
    {"default_source_extensions",     test_default_source_extensions},
    {"header_only_dependency",        test_header_only_dependency},
    {"build_cpp_test_orchestration",  test_build_cpp_test_orchestration},
    {"build_cpp_custom_lib_with_binaries", test_build_cpp_custom_lib_with_binaries},
    {"target_output_isolation",       test_target_output_isolation},
};

// ----------------------------------------------------------------
// Main
// ----------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <bake_binary> [test_filter]\n", argv[0]);
        return 1;
    }

    g_bake_bin = fs::absolute(argv[1]).string();
#ifdef BAKE_SRC_DIR
    g_fixture_root = std::string(BAKE_SRC_DIR) + "/tests/projects";
#else
    g_fixture_root = fs::path(argv[0]).parent_path() / "projects";
#endif

    std::string filter = (argc >= 3) ? argv[2] : "";

    std::printf("Running bake test suite (%zu tests)\n", all_tests.size());
    std::printf("  binary:   %s\n", g_bake_bin.c_str());
    std::printf("  fixtures: %s\n\n", g_fixture_root.c_str());

    for (auto& tc : all_tests) {
        if (!filter.empty() && tc.name.find(filter) == std::string::npos)
            continue;
        std::fflush(stdout);

        try {
            auto result = tc.func();
            if (result.ok) {
                std::printf("PASS\n");
                g_passed++;
            } else {
                std::printf("FAIL\n");
                std::printf("    %s\n", result.message.c_str());
                g_failed++;
            }
        } catch (const std::exception& e) {
            std::printf("ERROR\n");
            std::printf("    unexpected exception: %s\n", e.what());
            g_failed++;
        }
    }

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
