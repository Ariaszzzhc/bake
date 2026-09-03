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

    // Allow overriding LLVM location via env (e.g. for cross-compilation).
    // Relative paths are resolved against the workspace root.
    const char* env_llvm = std::getenv("BAKE_LLVM_DIR");
    std::string llvm_dir = env_llvm
        ? (fs::path(ws) / env_llvm).lexically_normal().string()
        : ws + "/external/llvm-install";
    std::string llvm_lib = llvm_dir + "/lib";
    std::string llvm_include = llvm_dir + "/include";

    // Collect LLVM/Clang/LLD static libraries in dependency order:
    // Clang → LLD → LLVM (consumers before providers).
    auto clang_libs = glob_static_libs(llvm_lib, "libclang");
    auto lld_libs   = glob_static_libs(llvm_lib, "liblld");
    auto llvm_libs  = glob_static_libs(llvm_lib, "libLLVM");

    b.sources({
        "src/json.cppm",
        "src/json.cpp",
        "src/toml.cppm",
        "src/toml.cpp",
        "src/net.cppm",
        "src/net.cpp",
        "src/archive.cppm",
        "src/archive.cpp",
        "src/util.cppm",
        "src/buildsystem/project.cppm",
        "src/buildsystem/moid.cppm",
        "src/buildsystem/graph.cppm",
        "src/toolchain/lld.cppm",
        "src/toolchain/cc1.cppm",
        "src/toolchain/cc1as.cppm",
        "src/toolchain/target.cppm",
        "src/buildsystem/cmdgen.cppm",
        "src/toolchain/runtime.cppm",
        "src/toolchain/driver.cppm",
        "src/buildsystem/engine.cppm",
        "src/buildsystem/package.cppm",
    });
    b.sources("src/main.cpp");
    b.include_dirs({
        llvm_include,
    });

    // Prebuilt LLVM/Clang/LLD static libraries.
    for (auto& lib : clang_libs) b.prebuilt_lib(lib);
    for (auto& lib : lld_libs)   b.prebuilt_lib(lib);
    for (auto& lib : llvm_libs)  b.prebuilt_lib(lib);

    // Link zlib/zstd if present (absent in cross-compile builds where they're disabled).
    std::string zlib = ws + "/external/zlib-install/lib/libz.a";
    std::string zstd = ws + "/external/zstd-install/lib/libzstd.a";
    if (fs::exists(zlib)) b.prebuilt_lib(zlib);
    if (fs::exists(zstd)) b.prebuilt_lib(zstd);

    return b.build();
}
