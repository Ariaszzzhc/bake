import bake.build;

int main() {
    bake::Builder b;
    b.sources({
        "src/leaf.cppm",
        "src/middle.cppm",
        "src/top.cppm",
        "src/main.cpp",
    });
    return b.build();
}
