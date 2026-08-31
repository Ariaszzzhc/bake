import bake.build;

int main() {
    bake::Builder builder;
    builder.sources("src/main.cpp");
    return builder.build();
}
