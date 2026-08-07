import std;

int main() {
    const char* path = std::getenv("BAKE_DECLARATION_PATH");
    const char* id = std::getenv("BAKE_MOID_ID");
    const char* root = std::getenv("BAKE_SOURCE_DIR");
    if (path == nullptr || id == nullptr || root == nullptr) return 2;

    std::ifstream input("payload.json", std::ios::binary);
    if (!input) return 3;
    std::string payload(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());

    auto replace = [&](std::string_view marker, std::string_view value) {
        const auto position = payload.find(marker);
        if (position != std::string::npos)
            payload.replace(position, marker.size(), value);
    };
    replace("$BAKE_MOID_ID", id);
    replace("$BAKE_SOURCE_DIR", root);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return 4;
    output << payload;
    return output ? 0 : 5;
}
