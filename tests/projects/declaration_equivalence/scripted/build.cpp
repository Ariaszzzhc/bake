import bake.build;
import std;

int main() {
    bake::Builder builder;
    builder.executable("declaration-equivalence")
        .sources("src/main.cpp");
    return builder.build();
}
