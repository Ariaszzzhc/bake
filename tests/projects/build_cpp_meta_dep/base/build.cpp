import bake.build;
import std;

int main() {
    bake::Builder b;
    const auto upstream = std::string(b.dep_src_dir("upstream"));
    b.sources(upstream + (b.feature("wolfssl")
            ? "/src/base_wolfssl.cpp"
            : "/src/base_mbedtls.cpp"))
        .public_headers(upstream + "/include");
    return b.build();
}
