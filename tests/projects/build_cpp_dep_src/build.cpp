import bake.build;

int main() {
    bake::Builder b;

    auto& answer = b.dependency("answer");
    b.executable("build-cpp-dep-src")
        .sources("src/*.cpp")
        .include_dirs(answer.src_dir())
        .std("c++23");

    return b.build();
}
