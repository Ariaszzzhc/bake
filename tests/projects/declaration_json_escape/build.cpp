import bake.build;
import std;

int main() {
    std::string include_dir = "include";
    include_dir.push_back('\b');
    include_dir.push_back('\f');
    include_dir.push_back('\x01');

    bake::Builder builder;
    builder.sources("src/main.cpp")
        .include_dirs(include_dir)
        .std("c++23");
    return builder.build();
}
