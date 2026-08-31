import bake.build;

int main() {
    bake::Builder b;

    // Top-level calls describe inputs for the main lib already defined by
    // bake.toml. They do not create, rename, or retype the main moid.
    b.sources("custom/src/value.cpp");
    b.public_headers("custom/include");

    b.binary("custom-tool-a")
        .sources("tools/tool_a.cpp");
    b.binary("custom-tool-b")
        .sources("tools/tool_b.cpp");

    return b.build();
}
