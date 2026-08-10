import bake.build;

int main() {
    bake::Builder b;

    // This describes inputs for the main lib already defined by bake.toml.
    // It does not create, rename, or retype the main moid.
    b.lib("custom-input-lib")
        .sources("custom/src/value.cpp")
        .public_headers("custom/include");

    b.binary("custom-tool-a")
        .sources("tools/tool_a.cpp");
    b.binary("custom-tool-b")
        .sources("tools/tool_b.cpp");

    return b.build();
}
