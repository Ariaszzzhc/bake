import bake.build;
import std;

int main() {
    bake::Builder b;
    auto& answer = b.static_lib("answer")
        .sources("src/*.cpp")
        .include_dirs("public")
        .private_define(
            ("ANSWER_BIAS=" + std::to_string(b.option_int("bias"))).c_str())
        .std("c++23");
    b.dependency("base").link_to(answer);
    return b.build();
}
