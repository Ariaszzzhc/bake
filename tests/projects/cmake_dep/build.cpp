import bake.build;

int main() {
    bake::Builder b;

    auto& app = b.executable("cmake-test")
        .sources("src/*.cpp")
        .std("c++17");

    // Build cmathlib via CMake bridge
    auto& lib = b.dependency("cmathlib");
    lib.build_with_cmake().expose_to(app);

    return b.build();
}
