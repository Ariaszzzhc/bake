import bake.build;
import std.compat;

int main() {
    size_t source_count = 1;
    bake::Builder builder;
    builder.executable("std-compat-build-cpp")
        .sources({"src/main.cpp"})
        .std("c++23");
    return source_count == 1 ? builder.build() : 1;
}
