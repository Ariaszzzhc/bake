import std.compat;

int main() {
    uintmax_t value = 42;
    size_t width = sizeof(value);
    return value == 42 && width > 0 ? 0 : 1;
}
