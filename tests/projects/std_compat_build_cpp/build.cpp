import bake.build;
import std.compat;

int main() {
    size_t source_count = 1;
    bake::Builder builder;
    builder.executable("std-compat-build-cpp")
        .sources({"src/main.cpp"});
    return source_count == 1 ? builder.build() : 1;
}
