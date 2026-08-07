import bake.build;
import std;

int main() {
    bake::Builder b;
    const std::string_view tls = b.option_str("tls");
    if (tls != "mbedtls" && tls != "wolfssl") {
        std::println(std::cerr, "base: unsupported tls backend '{}'", tls);
        return 1;
    }
    std::string upstream{b.dep_src_dir("upstream")};
    b.lib("base")
        .sources(upstream + "/src/*.cpp")
        .include_dirs(upstream + "/include")
        .define(tls == "wolfssl" ? "BASE_VALUE=41" : "BASE_VALUE=42")
        .std("c++23");
    return b.build();
}
