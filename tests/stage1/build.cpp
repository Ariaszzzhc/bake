import bake.build;
import std;

std::vector<std::string> static_libraries(const std::filesystem::path& dir,
                                          std::string_view prefix) {
    std::vector<std::string> result;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const auto name = entry.path().filename().string();
        if (entry.path().extension() == ".a" && name.starts_with(prefix)) {
            result.push_back(entry.path().string());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

int main() {
    bake::Builder builder;
    const auto root = (std::filesystem::current_path() / "../..").lexically_normal();
    const char* configured_llvm = std::getenv("BAKE_LLVM_DIR");
    const auto llvm = configured_llvm
        ? (root / configured_llvm).lexically_normal()
        : root / "external/llvm-install";
    builder.include_dirs((llvm / "include").string());
    for (const auto& prefix : {"libclang", "liblld", "libLLVM"}) {
        for (const auto& library : static_libraries(llvm / "lib", prefix)) {
            builder.prebuilt_lib(library);
        }
    }
    for (const auto& library : {
             root / "external/zlib-install/lib/libz.a",
             root / "external/zstd-install/lib/libzstd.a",
         }) {
        if (std::filesystem::exists(library)) builder.prebuilt_lib(library.string());
    }
    builder.public_modules({
        "../../bake/src/json.cppm",
        "../../bake/src/toml.cppm",
        "../../bake/src/net.cppm",
        "../../bake/src/archive.cppm",
        "../../bake/src/util.cppm",
        "../../bake/src/util/filetable.cppm",
        "../../bake/src/util/cachestore.cppm",
        "../../bake/src/toolchain/target.cppm",
        "../../bake/src/toolchain/lld.cppm",
        "../../bake/src/toolchain/runtime.cppm",
        "../../bake/src/toolchain/cc1.cppm",
        "../../bake/src/toolchain/cc1as.cppm",
        "../../bake/src/toolchain/driver.cppm",
        "../../bake/src/toolchain/compilation.cppm",
        "../../bake/src/buildsystem/project.cppm",
        "../../bake/src/buildsystem/semver.cppm",
        "../../bake/src/buildsystem/moid.cppm",
        "../../bake/src/buildsystem/graph.cppm",
        "../../bake/src/buildsystem/cmdgen.cppm",
        "../../bake/src/buildsystem/source.cppm",
        "../../bake/src/buildsystem/declaration.cppm",
        "../../bake/src/buildsystem/plan.cppm",
        "../../bake/src/buildsystem/package.cppm",
        "../../bake/src/core/model.cppm",
        "../../bake/src/core/state.cppm",
        "../../bake/src/core/runtime.cppm",
        "../../bake/src/core/snapshot.cppm",
        "../../bake/src/core/project.cppm",
        "../../bake/src/core/build.cppm",
        "../../bake/src/core/occurrences.cppm",
        "../../bake/src/core/analysis_proto.cppm",
        "../../bake/src/core/index.cppm",
        "../../bake/src/core/analysis.cppm",
    });
    builder.sources({
        "../../bake/src/json.cpp",
        "../../bake/src/toml.cpp",
        "../../bake/src/net.cpp",
        "../../bake/src/archive.cpp",
    });
    builder.binary("parser-contract").sources("../parser_test.cpp");
    builder.binary("graph-contract").sources("../graph_test.cpp");
    builder.binary("util-contract").sources("../util_test.cpp");
    builder.add_test("stage1-parser", "parser-contract");
    builder.add_test("stage1-graph", "graph-contract");
    builder.add_test("stage1-util", "util-contract");
    builder.binary("core-model-contract").sources("../core_model_test.cpp");
    builder.binary("core-state-contract").sources("../core_state_test.cpp");
    builder.binary("core-runtime-contract").sources("../core_runtime_test.cpp");
    builder.binary("core-snapshot-contract").sources("../core_snapshot_test.cpp");
    builder.binary("core-project-contract").sources("../core_project_test.cpp");
    builder.binary("core-analysis-contract").sources("../core_analysis_test.cpp");
    builder.binary("core-build-contract").sources("../core_build_test.cpp");
    builder.add_test("core-model", "core-model-contract");
    builder.add_test("core-state", "core-state-contract");
    builder.add_test("core-runtime", "core-runtime-contract");
    builder.add_test("core-snapshot", "core-snapshot-contract");
    builder.add_test("core-project", "core-project-contract");
    builder.add_test("core-analysis", "core-analysis-contract");
    builder.add_test("core-build", "core-build-contract");
    return builder.build();
}
