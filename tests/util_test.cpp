import std;
import bake.util;

namespace {

int checks = 0;

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) throw std::runtime_error(std::string(message));
}

struct Fixture {
    bake::Path root;

    Fixture() {
        root = bake::Path(std::filesystem::temp_directory_path() /
            ("bake-util-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())));
        root.mkdir_recursive();
    }

    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root.fs(), ignored);
    }
};

void digest_vectors() {
    const std::vector<std::pair<std::string, std::string>> vectors = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {std::string(55, 'a'), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {std::string(56, 'a'), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {std::string(63, 'a'), "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {std::string(64, 'a'), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {std::string(65, 'a'), "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
    };
    for (const auto& [input, expected] : vectors)
        check(bake::SHA256::hex(input) == expected,
              "SHA-256 known vector mismatch for length " +
                  std::to_string(input.size()));

    std::string binary(256, '\0');
    for (std::size_t i = 0; i < binary.size(); ++i)
        binary[i] = static_cast<char>(i);
    check(bake::SHA256::hex(binary) ==
              "40aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880",
          "SHA-256 binary vector mismatch");
}

void file_round_trip() {
    Fixture fixture;
    const auto empty = fixture.root / "empty.bin";
    check(bake::write_file(empty, ""), "empty file write failed");
    check(bake::read_file(empty) == std::optional<std::string>(""),
          "empty file read changed its content");

    std::string binary(1024 * 1024 + 65, '\0');
    for (std::size_t i = 0; i < binary.size(); ++i)
        binary[i] = static_cast<char>((i * 29 + 7) % 256);
    const auto path = fixture.root / "binary.bin";
    check(bake::write_file(path, binary), "binary file write failed");
    auto loaded = bake::read_file(path);
    check(loaded && *loaded == binary,
          "block file read changed embedded binary bytes");
    constexpr std::string_view expected =
        "8232c26d26c8bd37bc2b6e29e9e5b050fe78b321999c47523758c3b3001b75b8";
    check(bake::SHA256::hex(binary) == expected,
          "large binary memory digest mismatch");
    check(bake::SHA256::hex_file(path) == expected,
          "large binary file digest mismatch");
    check(!bake::read_file(fixture.root / "missing.bin"),
          "missing file produced empty success");
}

void throughput() {
    constexpr std::size_t bytes = 32 * 1024 * 1024;
    std::string payload(bytes, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>((i * 131 + 17) % 256);
    const auto started = std::chrono::steady_clock::now();
    const std::string digest = bake::SHA256::hex(payload);
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    check(digest ==
              "245e4be432961dc68bda4234abed2f922aff97629469db646108ef964b651491",
          "throughput payload digest mismatch");
    Fixture fixture;
    const auto path = fixture.root / "throughput.bin";
    check(bake::write_file(path, payload), "throughput file write failed");
    const auto read_started = std::chrono::steady_clock::now();
    auto loaded = bake::read_file(path);
    const auto read_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - read_started).count();
    check(loaded && *loaded == payload,
          "throughput file read changed its payload");
    std::println("PERF bytes={} sha_seconds={:.6f} sha_mib_per_second={:.3f} "
                 "read_seconds={:.6f} read_mib_per_second={:.3f} digest={}",
                 bytes, elapsed,
                 static_cast<double>(bytes) / (1024.0 * 1024.0) / elapsed,
                 read_elapsed,
                 static_cast<double>(bytes) / (1024.0 * 1024.0) /
                     read_elapsed,
                 digest);
}

}

int main(int argc, char** argv) {
    std::string selected = "all";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string_view(argv[i]) == "--case") selected = argv[i + 1];
    try {
        int cases = 0;
        if (selected == "all" || selected == "vectors") {
            digest_vectors();
            ++cases;
        }
        if (selected == "all" || selected == "file") {
            file_round_trip();
            ++cases;
        }
        if (selected == "throughput") {
            throughput();
            ++cases;
        }
        check(cases != 0, "unknown or empty case selection");
        std::println("PASS util_test case={} cases={} checks={}",
                     selected, cases, checks);
        return 0;
    } catch (const std::exception& error) {
        std::println(std::cerr, "FAIL util_test case={}: {}",
                     selected, error.what());
        return 1;
    }
}
