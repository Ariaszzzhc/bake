// build.cpp — libbake custom build script (Stage 1 self-hosting).
// Replaces convention mode for libbake because it needs LLVM static linking
// and special include paths that bake.toml cannot express.
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

    // cwd is libbake/ (the member directory).
    namespace fs = std::filesystem;
    std::string here = fs::current_path().string();
    // Workspace root is one level up.
    std::string ws = (fs::path(here) / "..").lexically_normal().string();
    std::string llvm_dir = ws + "/external/llvm-install";
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
            "src/bake.util.cppm",
            "src/bake.project.cppm",
            "src/bake.compiler.cppm",
            "src/bake.engine.cppm",
            "src/bake.package.cppm",
            "src/bake.cli.cppm",
            "src/cabi/api.cpp",
        })
        // LLVM-interfacing sources: LLVM is built with -fno-rtti, these must match.
        // bake_clang_driver.cpp is excluded: it imports bake.util (which has RTTI
        // enabled), and doesn't use typeid/dynamic_cast on LLVM types.
        .sources("src/compiler/bake_llvm.cpp", { .flags = {"-fno-rtti"} })
        .sources("src/compiler/bake_clang_cc1_main.cpp", { .flags = {"-fno-rtti"} })
        .sources("src/compiler/bake_clang_driver.cpp")
        .include_dirs({
            "../external/llvm-install/include",
            "../third_party/tomlplusplus/public",
            "../third_party/nlohmann/public",
            "src/cabi",
        })
        .define("BAKE_VERSION", q("0.1.0").c_str());

    // Link all LLVM/Clang/LLD static libs in dependency order + zlib + zstd.
    for (auto& lib : clang_libs) libbake.link_system(lib.c_str());
    for (auto& lib : lld_libs)   libbake.link_system(lib.c_str());
    for (auto& lib : llvm_libs)  libbake.link_system(lib.c_str());
    libbake.link_system((ws + "/external/zlib-install/lib/libz.a").c_str());
    libbake.link_system((ws + "/external/zstd-install/lib/libzstd.a").c_str());

    return b.build();
}
