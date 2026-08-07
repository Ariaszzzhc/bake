import bake.build;
import std;

int main() {
    bake::Builder builder;
    builder.sources("src/main.cpp").std("c++23");
    return builder.build();
}
