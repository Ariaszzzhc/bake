import std;

int main() {
    const char* path = std::getenv("BAKE_DECLARATION_PATH");
    if (path == nullptr) return 2;
    std::ofstream output(path);
    output << R"({"schema":999})";
    return output ? 0 : 3;
}
