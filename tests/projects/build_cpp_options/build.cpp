import bake.build;

int main() {
    bake::Builder b;

    b.sources({
        "src/main.c",
        "src/left/value.c",
        "src/right/value.c",
    });

    if (b.option_bool("native-backend"))
        b.sources("src/backend/native.c");
    else
        b.sources("src/backend/portable.c");

    return b.build();
}
