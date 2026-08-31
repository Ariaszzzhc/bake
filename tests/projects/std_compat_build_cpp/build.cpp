import bake.build;
import std.compat;

int main() {
    size_t source_count = 1;
    bake::Builder builder;
    builder.sources({"src/main.cpp"});
    return source_count == 1 ? builder.build() : 1;
}
