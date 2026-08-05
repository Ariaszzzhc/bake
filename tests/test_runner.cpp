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

static CmdResult run_bake(const std::string& args, const fs::path& cwd) {
    std::string env_prefix;
#ifdef __APPLE__
    // Ensure the test binary can find libbake at runtime.
    fs::path lib_dir = fs::path(g_bake_bin).parent_path();
    env_prefix = "DYLD_LIBRARY_PATH=" + lib_dir.string() + " ";
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
    CHECK(!fs::exists(dir / ".bake" / "obj"), "old .bake/obj/ should not exist");

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
    CHECK(fs::is_directory(dir / "out" / "obj" / "mylib"),
          "out/obj/mylib/ should exist for workspace member");
    CHECK(fs::is_directory(dir / "out" / "obj" / "app"),
          "out/obj/app/ should exist for workspace member");

    // Verify executable runs
    fs::path exe = dir / "out" / "bin" / "app";
    CHECK(fs::exists(exe), "workspace exe not found");
    auto run = run_cmd(exe.string(), dir);
    CHECK_EQ(run.exit_code, 0,
             "workspace exe returned non-zero: " + std::to_string(run.exit_code));

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

// ----------------------------------------------------------------
// Phase 4: CMake bridge tests
// ----------------------------------------------------------------

// build.cpp mode: CMake dep is built via the bridge, usage requirements
// (includes + lib) are applied to the target.
TestResult test_cmake_dep_build() {
    auto dir = make_temp_dir("cmake_dep");
    copy_fixture("cmake_dep", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "bake build failed for cmake_dep:\n" + r.stdout);

    // Verify the executable exists and runs correctly
    fs::path exe = dir / "out" / "bin" / "cmake-test";
    CHECK(fs::exists(exe), "executable not found at out/bin/cmake-test");

    auto run = run_cmd(exe.string(), dir);
    CHECK_EQ(run.exit_code, 0,
             "cmake-test returned non-zero: " + std::to_string(run.exit_code) + "\n" + run.stdout);

    return {};
}

// Convention mode: no build.cpp — bake auto-detects CMakeLists.txt in
// path dep and runs the bridge.
TestResult test_cmake_dep_convention() {
    auto dir = make_temp_dir("cmake_dep_conv");
    copy_fixture("cmake_dep_convention", dir);

    auto r = run_bake("build", dir);
    CHECK(r.success(), "bake build failed for cmake_dep_convention:\n" + r.stdout);

    // Verify the executable exists and runs correctly
    fs::path exe = dir / "out" / "bin" / "cmake-conv-test";
    CHECK(fs::exists(exe), "executable not found at out/bin/cmake-conv-test");

    auto run = run_cmd(exe.string(), dir);
    CHECK_EQ(run.exit_code, 0,
             "cmake-conv-test returned non-zero: " + std::to_string(run.exit_code) + "\n" + run.stdout);

    return {};
}

// ----------------------------------------------------------------
// Test registry
// ----------------------------------------------------------------

static std::vector<TestCase> all_tests = {
    {"simple_app_build",              test_simple_app_build},
    {"static_lib_build",              test_static_lib_build},
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
    {"version",                       test_version},
    {"update_single_dep",             test_update_single_dep},
    {"standalone_path_dep_build",     test_standalone_path_dep_build},
    {"standalone_path_dep_locked",    test_standalone_path_dep_locked},
    {"workspace_unified_output",      test_workspace_unified_output},
    {"workspace_member_filter",       test_workspace_member_filter},
    {"cmake_dep_build",               test_cmake_dep_build},
    {"cmake_dep_convention",          test_cmake_dep_convention},
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
