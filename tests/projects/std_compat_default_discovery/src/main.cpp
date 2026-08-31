import std.compat;

int main() {
    size_t width = sizeof(uintmax_t);
    return width > 0 ? 0 : 1;
}
