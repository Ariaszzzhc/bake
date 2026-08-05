import bake.build;
import std;

int main() {
    bake::Builder b;

    const char* configured_backend = b.option_str("backend");
    const std::string backend = configured_backend ? configured_backend : "";
    if (backend != "portable" && backend != "native") {
        std::println(std::cerr,
                     "option-app: backend must be 'portable' or 'native', got '{}'",
                     backend);
        return 2;
    }

    const std::string backend_define = "\"" + backend + "\"";
    const std::string diagnostics_define =
        b.option_bool("diagnostics") ? "1" : "0";
    const std::string level_define = std::to_string(b.option_int("level"));

    b.executable("option-app")
        .sources({
            "src/main.c",
            "src/left/value.c",
            "src/right/value.c",
        })
        .private_define("SELECTED_BACKEND", backend_define.c_str())
        .private_define("DIAGNOSTICS_ENABLED", diagnostics_define.c_str())
        .private_define("OPTION_LEVEL", level_define.c_str())
        .std("c17");

    return b.build();
}
