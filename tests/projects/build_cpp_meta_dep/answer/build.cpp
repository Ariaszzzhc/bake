import bake.build;

int main() {
    bake::Builder b;
    b.sources("src/*.cpp")
        .public_headers("public");
    return b.build();
}
