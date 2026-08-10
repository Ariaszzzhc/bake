import bake.build;

int main() {
    bake::Builder b;

    b.binary("unit_string")
        .sources("tests/unit_string.cpp");
    b.binary("unit_parser")
        .sources("tests/unit_parser.cpp");
    b.binary("integration")
        .sources("tests/integration.cpp");

    b.add_test("string-suite", "unit_string")
        .set_default();
    b.add_test("parser-suite", "unit_parser")
        .set_default();
    b.add_test("integration", "integration");

    return b.build();
}
