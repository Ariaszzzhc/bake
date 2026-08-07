// build.cpp — bake custom build script (Stage 1 self-hosting).
// Uses bake.build module (pure std, no C ABI, no library link).
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

int main() {
    bake::Builder b;

    // cwd is bake/ (the member directory).
    namespace fs = std::filesystem;
    std::string here = fs::current_path().string();
    std::string ws = (fs::path(here) / "..").lexically_normal().string();
    std::string llvm_dir = ws + "/external/llvm-install";
    std::string llvm_lib = llvm_dir + "/lib";

    // Collect LLVM/Clang/LLD static libraries in dependency order:
    // Clang → LLD → LLVM (consumers before providers).
    auto clang_libs = glob_static_libs(llvm_lib, "libclang");
    auto lld_libs   = glob_static_libs(llvm_lib, "liblld");
    auto llvm_libs  = glob_static_libs(llvm_lib, "libLLVM");

    b.executable("bake")
        .std("c++23")
        .sources({
            "src/bake.util.cppm",
            "src/bake.project.cppm",
            "src/bake.moid.cppm",
            "src/bake.graph.cppm",
            "src/bake.compiler.cppm",
            "src/bake.engine.cppm",
            "src/bake.package.cppm",
            "src/cli.cppm",
        })
        // LLVM-interfacing sources: LLVM is built with -fno-rtti, these must match.
        .sources("src/compiler/bake_llvm.cpp", { .flags = {"-fno-rtti"} })
        .sources("src/compiler/bake_clang_cc1_main.cpp", { .flags = {"-fno-rtti"} })
        .sources("src/compiler/bake_clang_driver.cpp")
        .sources("src/main.cpp")
        .include_dirs({
            "../external/llvm-install/include",
            "../third_party/tomlplusplus/public",
            "../third_party/nlohmann/public",
        })
        .define("BAKE_VERSION", "\"0.1.0\"");

    // Link all LLVM/Clang/LLD static libs + zlib + zstd.
    for (auto& lib : clang_libs) b.link_system(lib);
    for (auto& lib : lld_libs)   b.link_system(lib);
    for (auto& lib : llvm_libs)  b.link_system(lib);
    b.link_system(ws + "/external/zlib-install/lib/libz.a");
    b.link_system(ws + "/external/zstd-install/lib/libzstd.a");

    return b.build();
}
