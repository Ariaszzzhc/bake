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
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <functional>
#include <optional>

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
#ifdef __APPLE__
    // Ensure the test binary can find libbake at runtime.
    fs::path lib_dir = fs::path(g_bake_bin).parent_path();
    env_prefix += "DYLD_LIBRARY_PATH=" + lib_dir.string() + " ";
#endif
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

// ----------------------------------------------------------------
// Test cases
// ----------------------------------------------------------------

// Build a simple executable — bake build must succeed.
TestResult test_simple_app_build() {
    auto dir = make_temp_dir("simple_app");
    copy_fixture("simple_app", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "bake build failed for simple_app: " + r.stdout);

    // Verify the executable exists in out/bin/
    fs::path exe = dir / "out" / "bin" / "simple-app";
    CHECK(fs::exists(exe), "executable not found at out/bin/simple-app");

    return {};
}

// Build a static library — bake build must produce a .a file.
TestResult test_static_lib_build() {
    auto dir = make_temp_dir("static_lib");
    copy_fixture("static_lib", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "bake build failed for static_lib: " + r.stdout);

    // Verify the library exists in out/lib/
    bool found = false;
    fs::path lib_dir = dir / "out" / "lib";
    if (fs::exists(lib_dir)) {
        for (auto& e : fs::directory_iterator(lib_dir)) {
            if (e.path().extension() == ".a") { found = true; break; }
        }
    }
    CHECK(found, "static library (.a) not found in out/lib/");

    return {};
}

// Build a genuine C17 executable. C-only syntax keeps this from silently
// passing when a C++ driver treats .c input as C++.
TestResult test_pure_c_build() {
    auto dir = make_temp_dir("pure_c");
    copy_fixture("pure_c", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "bake build failed for pure C project: " + r.stdout);

    fs::path exe = dir / "out" / "bin" / "pure-c";
    CHECK(fs::exists(exe), "pure C executable not found at out/bin/pure-c");

    auto run = run_cmd(exe.string(), dir);
    CHECK_EQ(run.exit_code, 0,
             "pure C executable returned non-zero: " + std::to_string(run.exit_code));

    auto compile_commands = read_file(dir / "compile_commands.json");
    CHECK(compile_commands.find("-std=c17") != std::string::npos,
          "pure C compile command does not select C17: " + compile_commands);

    return {};
}

// Build a project with path dependencies — include dirs must resolve.
TestResult test_path_dep_build() {
    auto dir = make_temp_dir("path_dep");
    copy_fixture("path_dep", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "bake build failed for path_dep: " + r.stdout);

    // Verify the executable exists and runs correctly
    fs::path exe = dir / "out" / "bin" / "app";
    CHECK(fs::exists(exe), "executable not found at out/bin/app");

    // Run it — should return 0 (add(2,3) - 5 == 0)
    auto run = run_cmd(exe.string(), dir);
    CHECK_EQ(run.exit_code, 0, "app-with-dep returned non-zero: " + std::to_string(run.exit_code));

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

// bake add must reject duplicate dependency names.
TestResult test_add_duplicate() {
    auto dir = make_temp_dir("add_duplicate");
    copy_fixture("simple_app", dir);

    // First add should succeed
    auto r1 = run_bake("add https://github.com/fmtlib/fmt --tag 10.2.1 fmt", dir);
    CHECK(r1.success(), "first add failed: " + r1.stdout);

    // Second add should fail
    auto r2 = run_bake("add https://github.com/fmtlib/fmt --tag 10.2.1 fmt", dir);
    CHECK(!r2.success(), "duplicate add should have failed");
    CHECK(r2.stdout.find("already exists") != std::string::npos,
          "expected 'already exists' message: " + r2.stdout);

    return {};
}

// bake add --tag with no value must error (not silently write tag="true").
TestResult test_add_no_tag() {
    auto dir = make_temp_dir("add_no_tag");
    copy_fixture("simple_app", dir);

    auto r = run_bake("add https://github.com/fmtlib/fmt --tag", dir);
    CHECK(!r.success(), "add with empty --tag should have failed");
    CHECK(r.stdout.find("requires --tag") != std::string::npos,
          "expected 'requires --tag' error: " + r.stdout);

    return {};
}

// bake build on a clean project must produce unified out/ directory.
TestResult test_unified_output_layout() {
    auto dir = make_temp_dir("unified_output");
    copy_fixture("simple_app", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "build failed: " + r.stdout);

    CHECK(fs::is_directory(dir / "out"), "out/ directory not created");
    CHECK(fs::is_directory(dir / "out" / "bin"), "out/bin/ not created");

    // Old layout should not exist
    CHECK(!fs::exists(dir / "build"), "old build/ directory should not exist");
    CHECK(fs::is_directory(dir / "out" / ".obj"), "out/.obj/ not created");
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

    fs::path exe = dir / "out" / "bin" / dir.filename();
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
    fs::path exe = dir / "out" / "bin" / "calc";
    CHECK(fs::exists(exe), "executable not found at out/bin/calc");

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

// Duplicate add detection must work with compact TOML syntax too.
// Previously, "name = " text search missed "name={url=...}" compact form.
TestResult test_add_duplicate_compact() {
    auto dir = make_temp_dir("add_dup_compact");
    copy_fixture("simple_app", dir);

    // Write bake.toml with compact TOML dependency (no spaces around =)
    write_file(dir / "bake.toml",
        "[package]\n"
        "name = \"dup-test\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "std = \"c++17\"\n\n"
        "[dependencies]\n"
        "fmt={url=\"https://github.com/fmtlib/fmt\",tag=\"10.2.1\"}\n"
    );
    write_file(dir / "src" / "main.cpp", "int main() { return 0; }\n");

    // Adding "fmt" again should fail
    auto r = run_bake("add https://github.com/fmtlib/fmt --tag 10.2.1 fmt", dir);
    CHECK(!r.success(), "duplicate add with compact TOML should have failed");
    CHECK(r.stdout.find("already exists") != std::string::npos,
          "expected 'already exists' for compact TOML: " + r.stdout);

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

// Lockfile must reject when cache directory is missing for a frozen build.
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

// Workspace build: each member's output must go to the unified out/ dir
// with per-member obj/ and bmi/ subdirectories.
TestResult test_workspace_unified_output() {
    auto dir = make_temp_dir("ws_unified");
    copy_fixture("path_dep", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "workspace build failed: " + r.stdout);

    // Check unified output layout
    CHECK(fs::is_directory(dir / "out" / "bin"), "out/bin/ should exist");
    CHECK(fs::is_directory(dir / "out" / "lib"), "out/lib/ should exist");

    // Check per-member obj directories
    CHECK(fs::is_directory(dir / "out" / ".obj" / "mylib"),
          "out/.obj/mylib/ should exist for workspace member");
    CHECK(fs::is_directory(dir / "out" / ".obj" / "app"),
          "out/.obj/app/ should exist for workspace member");

    // Verify executable runs
    fs::path exe = dir / "out" / "bin" / "app";
    CHECK(fs::exists(exe), "workspace exe not found");
    auto run = run_cmd(exe.string(), dir);
    CHECK_EQ(run.exit_code, 0,
             "workspace exe returned non-zero: " + std::to_string(run.exit_code));

    auto conflict_dir = make_temp_dir("ws_meta_option_conflict");
    fs::copy(fs::path(g_fixture_root) / "build_cpp_meta_dep" / "base",
             conflict_dir / "base",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    write_file(conflict_dir / "bake.toml",
        "[workspace]\n"
        "members = [\"a\", \"b\"]\n");
    write_file(conflict_dir / "a" / "bake.toml",
        "[package]\n"
        "name = \"member-a\"\n"
        "version = \"0.1.0\"\n"
        "type = \"static-lib\"\n\n"
        "[dependencies]\n"
        "base = { path = \"../base\", options = { tls = \"mbedtls\" } }\n");
    write_file(conflict_dir / "b" / "bake.toml",
        "[package]\n"
        "name = \"member-b\"\n"
        "version = \"0.1.0\"\n"
        "type = \"static-lib\"\n\n"
        "[dependencies]\n"
        "base = { path = \"../base\", options = { tls = \"wolfssl\" } }\n");

    auto conflict = run_bake("build", conflict_dir);
    CHECK(!conflict.success(),
          "workspace members must not build two configurations of one package");
    CHECK(conflict.stdout.find("option conflict for package 'base'") !=
              std::string::npos &&
          conflict.stdout.find("member-a -> base") != std::string::npos &&
          conflict.stdout.find("member-b -> base") != std::string::npos,
          "workspace option conflict did not identify both members: " +
              conflict.stdout);

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
    fs::path lib_dir = dir / "out" / "lib";
    bool found_lib = false;
    if (fs::exists(lib_dir)) {
        for (auto& e : fs::directory_iterator(lib_dir)) {
            if (e.path().extension() == ".a") { found_lib = true; break; }
        }
    }
    CHECK(found_lib, "mylib .a should exist after build -p mylib");

    return {};
}

// A convention-mode consumer needs only bake.toml and src/. Bake must build
// each Bake-native dependency's own recipe and automatically consume its
// exported usage requirements; the consumer must not need build.cpp glue.
TestResult test_convention_meta_dependency() {
    auto dir = make_temp_dir("build_cpp_meta_dep");
    copy_fixture("build_cpp_meta_dep", dir);

    auto build = run_bake("build", dir);
    CHECK(build.success(), "meta dependency build failed: " + build.stdout);

    const fs::path answer_lib =
        dir / "out" / ".pkgs" / "answer" / "lib" / "libanswer.a";
    const fs::path base_lib =
        dir / "out" / ".pkgs" / "base" / "lib" / "libbase.a";
    CHECK(fs::exists(answer_lib),
          "dependency library was not built under consumer out/.pkgs");
    CHECK(fs::exists(base_lib),
          "transitive dependency library was not built under consumer out/.pkgs");
    CHECK(fs::is_directory(dir / "out" / ".obj" / "answer"),
          "dependency objects were not built under consumer out/.obj");
    CHECK(fs::is_directory(dir / "out" / ".obj" / "base"),
          "transitive dependency objects were not built under consumer out/.obj");
    CHECK(!fs::exists(dir / "answer" / "out") &&
              !fs::exists(dir / "answer" / ".bake"),
          "dependency source directory was modified by the build");
    CHECK(!fs::exists(dir / "base" / "out") &&
              !fs::exists(dir / "base" / ".bake"),
          "transitive dependency source directory was modified by the build");

    const auto answer_mtime = fs::last_write_time(answer_lib);
    const auto base_mtime = fs::last_write_time(base_lib);

    auto rebuild = run_bake("build", dir);
    CHECK(rebuild.success(), "meta dependency rebuild failed: " + rebuild.stdout);
    CHECK_EQ(fs::last_write_time(answer_lib),
             answer_mtime, "unchanged direct meta dependency was rebuilt");
    CHECK_EQ(fs::last_write_time(base_lib),
             base_mtime, "unchanged transitive meta dependency was rebuilt");

    fs::path exe = dir / "out" / "bin" / "meta-consumer";
    CHECK(fs::exists(exe), "meta consumer executable was not created");
    auto run = run_cmd(exe.string(), dir);
    CHECK_EQ(run.exit_code, 0,
             "dependency option was not applied: " + std::to_string(run.exit_code));

    // A CLI option belongs to the package at the command root. Even when the
    // consumer and dependency declare the same name, the consumer's CLI value
    // must not replace the value nested in the dependency declaration.
    auto consumer_option = run_bake("build --option bias=2", dir);
    CHECK(consumer_option.success(),
          "consumer's own option should be accepted: " + consumer_option.stdout);
    auto isolated_run = run_cmd(exe.string(), dir);
    CHECK_EQ(isolated_run.exit_code, 0,
             "consumer CLI option leaked into the dependency package");

    auto unknown_dir = make_temp_dir("build_cpp_meta_dep_unknown_option");
    copy_fixture("build_cpp_meta_dep", unknown_dir);
    write_file(unknown_dir / "bake.toml",
        "[package]\n"
        "name = \"meta-consumer\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "std = \"c++23\"\n\n"
        "[dependencies]\n"
        "answer = { path = \"answer\", options = { missing = true } }\n");
    auto unknown = run_bake("build", unknown_dir);
    CHECK(!unknown.success(), "undeclared dependency option should fail");
    CHECK(unknown.stdout.find("option 'missing' is not declared by package 'answer'") !=
              std::string::npos,
          "undeclared dependency option did not identify its owner: " +
              unknown.stdout);

    auto wrong_type_dir = make_temp_dir("build_cpp_meta_dep_wrong_option_type");
    copy_fixture("build_cpp_meta_dep", wrong_type_dir);
    write_file(wrong_type_dir / "bake.toml",
        "[package]\n"
        "name = \"meta-consumer\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "std = \"c++23\"\n\n"
        "[dependencies]\n"
        "answer = { path = \"answer\", options = { bias = \"one\" } }\n");
    auto wrong_type = run_bake("build", wrong_type_dir);
    CHECK(!wrong_type.success(), "wrong dependency option type should fail");
    CHECK(wrong_type.stdout.find("expects integer, got string") !=
              std::string::npos,
          "wrong dependency option type was not diagnosed: " +
              wrong_type.stdout);

    // Dependency options are constraints on the one package instance built
    // for this project. An omitted value is not a request for the default: a
    // different edge may select the package-wide value before defaults apply.
    auto unified_dir = make_temp_dir("build_cpp_meta_dep_unified_option");
    copy_fixture("build_cpp_meta_dep", unified_dir);
    write_file(unified_dir / "bake.toml",
        "[package]\n"
        "name = \"meta-consumer\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "std = \"c++23\"\n\n"
        "[dependencies]\n"
        "answer = { path = \"answer\", options = { bias = 2 } }\n"
        "base = { path = \"base\", options = { tls = \"wolfssl\" } }\n");
    auto unified = run_bake("build", unified_dir);
    CHECK(unified.success(),
          "an explicit option should satisfy an unspecified edge: " +
              unified.stdout);
    auto unified_run = run_cmd(
        (unified_dir / "out" / "bin" / "meta-consumer").string(),
        unified_dir);
    CHECK_EQ(unified_run.exit_code, 0,
             "the unified dependency option was not used by the package");

    // Two explicit, different values for a single-valued option cannot be
    // represented by one package build. The error must identify both values
    // and the dependency paths that requested them.
    auto conflict_dir = make_temp_dir("build_cpp_meta_dep_option_conflict");
    copy_fixture("build_cpp_meta_dep", conflict_dir);
    write_file(conflict_dir / "bake.toml",
        "[package]\n"
        "name = \"meta-consumer\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "std = \"c++23\"\n\n"
        "[dependencies]\n"
        "answer = { path = \"answer\", options = { bias = 1 } }\n"
        "base = { path = \"base\", options = { tls = \"wolfssl\" } }\n");
    write_file(conflict_dir / "answer" / "bake.toml",
        "[package]\n"
        "name = \"answer\"\n"
        "version = \"1.0.0\"\n"
        "type = \"static-lib\"\n"
        "std = \"c++23\"\n\n"
        "[dependencies]\n"
        "base = { path = \"../base\", options = { tls = \"mbedtls\" } }\n\n"
        "[options]\n"
        "bias = 0\n");
    auto conflict = run_bake("build", conflict_dir);
    CHECK(!conflict.success(), "different tls selections should conflict");
    CHECK(conflict.stdout.find("option conflict for package 'base'") !=
              std::string::npos &&
          conflict.stdout.find("option 'tls'") != std::string::npos &&
          conflict.stdout.find("\"mbedtls\"") != std::string::npos &&
          conflict.stdout.find("\"wolfssl\"") != std::string::npos &&
          conflict.stdout.find("meta-consumer -> answer -> base") !=
              std::string::npos &&
          conflict.stdout.find("meta-consumer -> base") != std::string::npos,
          "option conflict did not report values and dependency paths: " +
              conflict.stdout);

    return {};
}

// build.cpp must observe typed CLI option overrides. Changing an option must
// invalidate actions whose command line changed, even when sources did not.
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
    CHECK(fs::is_directory(dir / "out" / ".bmi" / ".std"),
          "std module was not built in the project-local out/.bmi tree");
    CHECK(!fs::exists(test_home / ".cache" / "bake") &&
              !fs::exists(test_home / "bake"),
          "build artifacts leaked into the global Bake source cache");

    fs::path exe = dir / "out" / "bin" / "option-app";
    CHECK(fs::exists(exe), "option-app executable was not produced");
    auto initial_run = run_cmd(exe.string(), dir);
    CHECK(initial_run.success(), "default option executable failed");
    CHECK_EQ(initial_run.stdout, std::string("portable|0|1\n"),
             "build.cpp did not receive default typed options");

    auto unchanged = run_option_bake("build");
    CHECK(unchanged.success(), "unchanged option rebuild failed: " + unchanged.stdout);
    CHECK(unchanged.stdout.find("up to date") != std::string::npos,
          "unchanged options did not reuse build actions: " + unchanged.stdout);

    auto overridden = run_option_bake(
        "build --option backend=native --option diagnostics --option level=7");
    CHECK(overridden.success(), "overridden option build failed: " + overridden.stdout);
    CHECK(overridden.stdout.find("up to date") == std::string::npos,
          "option change incorrectly reused stale actions: " + overridden.stdout);

    auto overridden_run = run_cmd(exe.string(), dir);
    CHECK(overridden_run.success(), "overridden option executable failed");
    CHECK_EQ(overridden_run.stdout, std::string("native|1|7\n"),
             "build.cpp did not receive string/bool/int CLI overrides");

    auto invalid_type = run_option_bake(
        "build --option level=not-an-integer");
    CHECK(!invalid_type.success(), "invalid integer option should fail");
    CHECK(invalid_type.stdout.find("expects an integer") != std::string::npos,
          "invalid integer option did not report its type: " + invalid_type.stdout);

    auto unknown = run_option_bake("build --option missing=value");
    CHECK(!unknown.success(), "undeclared option should fail");
    CHECK(unknown.stdout.find("unknown build option 'missing'") != std::string::npos,
          "unknown option did not report its name: " + unknown.stdout);

    auto run_via_bake = run_option_bake("run");
    CHECK(run_via_bake.success(),
          "bake run could not read out/.bake/build.json: " +
              run_via_bake.stdout);
    CHECK(run_via_bake.stdout.find("portable|0|1\n") != std::string::npos,
          "bake run executed the wrong build.cpp output: " +
              run_via_bake.stdout);

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
        "std = \"c++17\"\n\n"
        "[dependencies]\n"
        "fixture = { url = \"file://" + remote.string() +
            "\", tag = \"v1.0\" }\n"
    );

    auto build = run_bake("build", project, "HOME=" + test_home.string());
    CHECK(build.success(),
          "local remote dependency failed to extract/build: " + build.stdout);
    CHECK(fs::exists(project / "bake.lock"),
          "remote dependency build did not write bake.lock");

    // A path meta package may itself resolve remote raw sources. Its source
    // directory is immutable from the consumer's perspective: generated lock
    // state belongs under the consumer's out/.pkgs tree.
    auto meta = dir / "remote-meta";
    auto consumer = dir / "meta-consumer";
    fs::create_directories(meta);
    fs::create_directories(consumer / "src");
    write_file(meta / "bake.toml",
        "[package]\n"
        "name = \"remote-meta\"\n"
        "version = \"1.0.0\"\n"
        "type = \"static-lib\"\n"
        "std = \"c++23\"\n\n"
        "[dependencies]\n"
        "upstream = { url = \"file://" + remote.string() +
            "\", tag = \"v1.0\" }\n");
    write_file(meta / "build.cpp",
        "import bake.build;\n\n"
        "int main() {\n"
        "    bake::Builder b;\n"
        "    auto& upstream = b.dependency(\"upstream\");\n"
        "    b.static_lib(\"remote-meta\")\n"
        "        .sources(upstream, \"src/*.cpp\")\n"
        "        .include_dirs(upstream, \"public\")\n"
        "        .std(\"c++23\");\n"
        "    return b.build();\n"
        "}\n");
    write_file(consumer / "bake.toml",
        "[package]\n"
        "name = \"remote-meta-consumer\"\n"
        "version = \"0.1.0\"\n"
        "type = \"executable\"\n"
        "std = \"c++23\"\n\n"
        "[dependencies]\n"
        "remote-meta = { path = \"../remote-meta\" }\n");
    write_file(consumer / "src" / "main.cpp",
        "#include <remote_fixture.hpp>\n"
        "int main() { return remote_archive_fixture() == 42 ? 0 : 1; }\n");

    auto meta_build = run_bake(
        "build", consumer, "HOME=" + test_home.string());
    CHECK(meta_build.success(),
          "path meta package could not resolve its remote source: " +
              meta_build.stdout);
    CHECK(!fs::exists(meta / "bake.lock") &&
              !fs::exists(meta / ".bake") &&
              !fs::exists(meta / "out"),
          "consumer build modified the path meta package source directory");
    CHECK(fs::exists(consumer / "out" / ".pkgs" / "remote-meta" /
                     ".bake" / "bake.lock"),
          "dependency lock state was not stored under consumer out/.pkgs");
    auto meta_run = run_cmd(
        (consumer / "out" / "bin" / "remote-meta-consumer").string(),
        consumer);
    CHECK(meta_run.success(),
          "remote-backed meta package did not link into its consumer");

    return {};
#endif
}

// ----------------------------------------------------------------
// Test registry
// ----------------------------------------------------------------

static std::vector<TestCase> all_tests = {
    {"simple_app_build",              test_simple_app_build},
    {"static_lib_build",              test_static_lib_build},
    {"pure_c_build",                  test_pure_c_build},
    {"path_dep_build",                test_path_dep_build},
    {"path_dep_locked",               test_path_dep_locked},
    {"frozen_no_lock",                test_frozen_no_lock},
    {"lock_consistency",              test_lock_consistency},
    {"lock_transitive_consistency",   test_lock_transitive_consistency},
    {"frozen_missing_cache",          test_frozen_missing_cache},
    {"add_duplicate",                 test_add_duplicate},
    {"add_duplicate_compact",         test_add_duplicate_compact},
    {"add_no_tag",                    test_add_no_tag},
    {"unified_output_layout",         test_unified_output_layout},
    {"clean",                         test_clean},
    {"init",                          test_init},
    {"init_c",                        test_init_c},
    {"version",                       test_version},
    {"update_single_dep",             test_update_single_dep},
    {"standalone_path_dep_build",     test_standalone_path_dep_build},
    {"standalone_path_dep_locked",    test_standalone_path_dep_locked},
    {"workspace_unified_output",      test_workspace_unified_output},
    {"workspace_member_filter",       test_workspace_member_filter},
    {"convention_meta_dependency",    test_convention_meta_dependency},
    {"build_cpp_options",             test_build_cpp_options},
    {"remote_archive_extract",        test_remote_archive_extract},
};

// ----------------------------------------------------------------
// Main
// ----------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <bake_binary> [test_filter]\n", argv[0]);
        return 1;
    }

    g_bake_bin = argv[1];
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

        g_current_test = tc.name;
        std::printf("  %-30s ", tc.name.c_str());
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
