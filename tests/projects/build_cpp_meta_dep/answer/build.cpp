import bake.build;
import std;

int main() {
    bake::Builder b;
    b.lib("answer")
        .sources("src/*.cpp")
        .include_dirs("public")
        .define(("ANSWER_BIAS=" + std::to_string(b.option_int("bias"))).c_str())
        .std("c++23");
    return b.build();
}
