// build.cpp — bake self-hosting build script.
// This replaces CMakeLists.txt for Stage 1: bake builds itself.
import bake.build;
import std;

// Glob *.a files in a directory, filtered by prefix.
std::vector<std::string> glob_static_libs(const std::string& dir,
                                          const std::string& prefix = "") {
    std::vector<std::string> result;
    namespace fs = std::filesystem;
    if (!fs::exists(dir)) return result;
    for (auto& entry : fs::directory_iterator(dir)) {
        auto path = entry.path();
        if (path.extension() == ".a") {
            std::string name = path.filename().string();
            if (prefix.empty() || name.starts_with(prefix))
                result.push_back(fs::absolute(path).string());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

// Wrap a string value as a quoted C string for -D defines.
// The executor spawns commands directly (no shell), so the define value must
// contain literal quote characters for the macro to be a string literal.
std::string q(const std::string& s) {
    return "\"" + s + "\"";
}

int main() {
    bake::Builder b;

    // build_app runs with cwd = project root.
    std::string src = std::filesystem::current_path().string();
    std::string llvm_dir = src + "/external/llvm-install";
    std::string llvm_lib = llvm_dir + "/lib";

    // Collect LLVM/Clang/LLD static libraries in dependency order:
    // Clang → LLD → LLVM (consumers before providers).
    auto clang_libs = glob_static_libs(llvm_lib, "libclang");
    auto lld_libs   = glob_static_libs(llvm_lib, "liblld");
    auto llvm_libs  = glob_static_libs(llvm_lib, "libLLVM");

    // ── libbake (shared library) ──────────────────────────────────
    auto& libbake = b.shared_lib("libbake");
    libbake.std("c++23")
        .sources({
            "libbake/src/bake.util.cppm",
            "libbake/src/bake.project.cppm",
            "libbake/src/bake.compiler.cppm",
            "libbake/src/bake.engine.cppm",
            "libbake/src/bake.package.cppm",
            "libbake/src/bake.cli.cppm",
            "libbake/src/cabi/api.cpp",
        })
        // LLVM-interfacing sources: LLVM is built with -fno-rtti, these must match.
        .sources("libbake/src/compiler/*.cpp", { .flags = {"-fno-rtti"} })
        .include_dirs({
            "external/llvm-install/include",
            "third_party/tomlplusplus/public",
            "third_party/nlohmann/public",
            "libbake/src/cabi",
        })
        .define("BAKE_VERSION", q("0.1.0").c_str())
        .define("BAKE_SRC_DIR", q(src).c_str())
        .define("BAKE_LIB_DIR", q(b.build_dir()).c_str())
        .define("BAKE_LLVM_PREFIX", q(llvm_dir).c_str())
        .define("BAKE_RESOURCE_DIR", q(llvm_dir + "/lib/clang/22").c_str())
        .define("BAKE_DARWIN_INC", q(src + "/lib/bake/libc/darwin/include").c_str())
        .define("BAKE_DARWIN_LIB", q(src + "/lib/bake/libc/darwin").c_str())
        .define("BAKE_LIBCXX_INC", q(llvm_dir + "/include/c++/v1").c_str())
        .define("BAKE_LIBCXX_MODULES_DIR", q(src + "/external/llvm-project/libcxx/modules").c_str());

    // Link all LLVM/Clang/LLD static libs in dependency order + zlib + zstd.
    for (auto& lib : clang_libs) libbake.link_system(lib.c_str());
    for (auto& lib : lld_libs)   libbake.link_system(lib.c_str());
    for (auto& lib : llvm_libs)  libbake.link_system(lib.c_str());
    libbake.link_system((src + "/external/zlib-install/lib/libz.a").c_str());
    libbake.link_system((src + "/external/zstd-install/lib/libzstd.a").c_str());

    // ── bake (executable) ─────────────────────────────────────────
    auto& bake_exe = b.executable("bake");
    bake_exe.std("c++23")
        .sources("bake/src/main.cpp")
        .link(libbake);

    return b.build();
}
