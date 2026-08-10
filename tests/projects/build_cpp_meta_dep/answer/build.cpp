import bake.build;

int main() {
    bake::Builder b;
    b.lib("answer")
        .sources("src/*.cpp")
        .public_headers("public");
    return b.build();
}
