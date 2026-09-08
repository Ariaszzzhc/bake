import std;
import bake.util;
import bake.toolchain.target;
import bake.core.model;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println(std::cerr, "FAIL: {}", message);
    }
}

struct Fixture {
    bake::Path root;
    bake::CompilerIdentity compiler;

    Fixture() {
        root = bake::Path(std::filesystem::temp_directory_path() /
            ("bake-core-model-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())));
        (root / "member-a").mkdir_recursive();
        (root / "member-b").mkdir_recursive();
        root = bake::Path(std::filesystem::canonical(root.fs()));
        bake::Path compiler_path = root / "bake-test-bin";
        bake::write_file(compiler_path, "compiler-a");
        auto identified = bake::identify_compiler(
            compiler_path, "0.1.0-test", root / "identity-cache");
        if (!identified) throw std::runtime_error(identified.error().message);
        compiler = *identified;
    }

    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root.fs(), ignored);
    }

    bake::ProjectContext context(
            std::optional<std::string> member = "member-a",
            std::vector<std::string> features = {"json", "async"},
            std::string profile = "dev",
            bake::TargetSpec target =
                bake::parse_target("x86_64-linux-gnu.2.28"),
            std::optional<bake::CompilerIdentity> identity = {}) const {
        auto result = bake::make_project_context(
            root,
            member ? std::optional<bake::Path>(root / bake::Path(*member))
                   : std::nullopt,
            std::move(features),
            std::move(profile), std::move(target),
            identity.value_or(compiler));
        if (!result) throw std::runtime_error(result.error().message);
        return *result;
    }
};

bake::ProjectContextKey key(const bake::ProjectContext& context) {
    auto result = bake::context_key(context);
    if (!result) throw std::runtime_error(result.error().message);
    return *result;
}

void identity_and_path_case() {
    Fixture fixture;
    auto base = fixture.context();
    const auto base_key = key(base);

    check(base.features == std::vector<std::string>({"async", "json"}),
          "features are canonicalized independently of request order");
    check(key(fixture.context("member-a", {"async", "json"})) == base_key,
          "equivalent feature ordering has one context identity");
    check(key(fixture.context("member-b")) != base_key,
          "workspace member changes context identity");
    check(key(fixture.context(std::nullopt)) != base_key,
          "all-members selection differs from one selected member");
    check(key(fixture.context("member-a", {"async"})) != base_key,
          "effective feature set changes context identity");
    check(key(fixture.context("member-a", {"json", "async"}, "release")) !=
              base_key,
          "profile changes context identity");
    check(key(fixture.context("member-a", {"json", "async"}, "dev",
                              bake::parse_target("x86_64-linux-gnu.2.36"))) !=
              base_key,
          "full target ABI version changes context identity");

    auto other_identity = fixture.compiler;
    other_identity.content_digest = bake::SHA256::hex("different compiler");
    check(key(fixture.context("member-a", {"json", "async"}, "dev",
                              bake::parse_target("x86_64-linux-gnu.2.28"),
                              other_identity)) != base_key,
          "compiler content changes context identity");

    for (std::string profile : {"dev", "release", "bench"}) {
        auto paths = bake::context_paths(
            fixture.context("member-a", {"async"}, profile));
        check(paths.has_value(), "valid context produces paths");
        if (!paths) continue;
        const auto expected = fixture.root / "out" /
            "x86_64-linux-gnu.2.28" / bake::Path(profile);
        check(paths->root == expected, "profile is part of output root");
        check(paths->bin == expected / "bin" &&
              paths->lib == expected / "lib" &&
              paths->object == expected / ".obj" &&
              paths->bmi == expected / ".bmi" &&
              paths->bake == expected / ".bake",
              "all project paths share target/profile root");
        check(paths->pch == expected / ".bake" / "lsp-preamble" &&
              paths->index == expected / ".bake" / "lsp-index",
              "analysis caches are profile isolated");
    }

    bake::CompilationDescription description{
        .id = {"unit"},
        .context = base_key,
        .owner_moid = "app",
        .language = bake::SourceLanguage::cxx,
        .working_directory = fixture.root,
        .source = fixture.root / "main.cpp",
        .planned_output = fixture.root / "main.o",
        .driver_arguments = {"c++", "-c", "main.cpp"},
    };
    auto command = bake::materialize_compiler_command(description,
                                                       fixture.compiler);
    check(command.front() == fixture.compiler.executable.string() &&
          command[1] == "c++",
          "compile command binds the verified current executable at use time");
}

void compiler_cache_case(const std::string& self) {
    Fixture fixture;
    bake::Path cache_directory = fixture.root / "identity-cache";
    std::vector<std::filesystem::path> entries;
    for (const auto& entry :
         std::filesystem::directory_iterator(cache_directory.fs()))
        entries.push_back(entry.path());
    check(entries.size() == 1, "compiler identity creates one persistent cache record");
    if (entries.size() != 1) return;
    const auto before = std::filesystem::last_write_time(entries.front());
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const std::string command = std::format(
        "\"{}\" --compiler-cache-child \"{}\" \"{}\" {}",
        self, fixture.compiler.executable.string(), cache_directory.string(),
        fixture.compiler.content_digest);
    check(std::system(command.c_str()) == 0,
          "second process resolves the same compiler identity");
    const auto after = std::filesystem::last_write_time(entries.front());
    check(before == after,
          "second process reuses compiler identity without rewriting its cache");
}

void invalid_context_case() {
    Fixture fixture;
    auto target = bake::parse_target("x86_64-linux-gnu.2.28");

    auto empty_profile = bake::make_project_context(
        fixture.root, fixture.root / "member-a", {}, "", target,
        fixture.compiler);
    check(!empty_profile &&
              empty_profile.error().code == bake::ContextErrorCode::invalid_profile,
          "empty profile is rejected with a typed error");

    auto noncanonical = fixture.context();
    noncanonical.member_root =
        fixture.root / "member-a" / ".." / "member-a";
    auto noncanonical_key = bake::context_key(noncanonical);
    check(!noncanonical_key &&
              noncanonical_key.error().code == bake::ContextErrorCode::missing_member,
          "noncanonical member in a persisted context is rejected");

    bake::TargetSpec incomplete;
    auto incomplete_target = bake::make_project_context(
        fixture.root, fixture.root / "member-a", {}, "dev", incomplete,
        fixture.compiler);
    check(!incomplete_target &&
              incomplete_target.error().code == bake::ContextErrorCode::invalid_target,
          "incomplete target is rejected with a typed error");

    auto all_members = bake::make_project_context(
        fixture.root, std::nullopt, {}, "dev", target, fixture.compiler);
    check(all_members && !all_members->member_root,
          "all-members workspace selection is represented explicitly");
}

}

int main(int argc, char** argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--compiler-cache-child") {
        auto identity = bake::identify_compiler(argv[2], "0.1.0-test", argv[3]);
        return identity && identity->content_digest == argv[4] ? 0 : 2;
    }
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--case" &&
            std::string_view(argv[2]) == "invalid-context") {
            invalid_context_case();
        } else {
            identity_and_path_case();
            invalid_context_case();
            std::string self = bake::get_self_exe_path();
            if (self.empty()) self = argv[0];
            compiler_cache_case(self);
        }
    } catch (const std::exception& exception) {
        std::println(std::cerr, "FAIL: unexpected exception: {}", exception.what());
        ++failures;
    }
    std::println("core_model_test: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
