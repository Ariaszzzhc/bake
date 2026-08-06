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
        })
        .std("c++23");
    return b.build();
}
