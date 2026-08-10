import bake.build;
import std;

int main() {
    bake::Builder b;
    b.executable("transitive-modules")
        .sources({
            "src/leaf.cppm",
            "src/middle.cppm",
            "src/top.cppm",
            "src/main.cpp",
        });
    return b.build();
}
