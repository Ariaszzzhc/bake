import std;
import bake.util;
import bake.stage0.project;
import bake.stage0.semver;
import bake.stage0.graph;
import bake.stage0.moid;

namespace {

bake::MoidNode make_node(std::string id) {
    bake::MoidNode node;
    node.id = bake::MoidId{id};
    node.declaration.id = id;
    node.declaration.name = id;
    return node;
}

bool reports_orphan(const bake::MoidGraph& graph, std::string_view id) {
    auto result = bake::topological_moids(graph);
    if (result) {
        std::println(std::cerr,
                     "topological_moids accepted orphan '{}'", id);
        return false;
    }
    if (result.error().find("orphan moid '" + std::string(id) + "'") ==
            std::string::npos ||
        result.error().find("not reachable from any graph root") ==
            std::string::npos) {
        std::println(std::cerr,
                     "unexpected orphan diagnostic: {}", result.error());
        return false;
    }
    return true;
}

// ===== Manifest tests (dependency forms, target scopes, effective set) =====

int temp_counter = 0;

bake::Path make_temp_project(std::string_view toml) {
    auto dir = std::filesystem::temp_directory_path() /
               ("bake_manifest_test_" + std::to_string(++temp_counter));
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / "bake.toml");
    out << toml;
    return bake::Path(dir);
}

void drop_temp_project(const bake::Path& dir) {
    std::error_code ignored;
    std::filesystem::remove_all(dir.fs(), ignored);
}

bool manifest_form_tests() {
    auto dir = make_temp_project(
        "[package]\n"
        "name = \"t\"\n"
        "version = \"0.1.0\"\n"
        "\n"
        "[dependencies]\n"
        "fmt = { url = \"https://github.com/fmtlib/fmt\", tag = \"11.1.4\" }\n"
        "dev = { url = \"https://example.com/dev\", branch = \"main\", features = [\"a\", \"b\"] }\n"
        "pin = { url = \"https://example.com/pin\", rev = \"deadbeef\" }\n"
        "head = { url = \"https://example.com/head\" }\n"
        "arc = { url = \"https://example.com/arc-1.0.tar.gz\" }\n"
        "loc = { path = \"../loc\" }\n");
    auto manifest = bake::Manifest::load(dir);
    if (!manifest) {
        std::println(std::cerr, "form manifest failed to load");
        drop_temp_project(dir);
        return false;
    }
    bool ok = true;
    auto check = [&](bool condition, std::string_view what) {
        if (!condition) {
            std::println(std::cerr, "form check failed: {}", what);
            ok = false;
        }
    };
    check(manifest->dependencies.size() == 6, "six deps parsed");
    auto& fmt = manifest->dependencies.at("fmt");
    check(fmt.git_ref() == std::pair<std::string, std::string>{"tag", "11.1.4"},
          "fmt ref is tag 11.1.4");
    check(manifest->dependencies.at("dev").git_ref() ==
              std::pair<std::string, std::string>{"branch", "main"},
          "dev ref is branch main");
    check(manifest->dependencies.at("dev").features ==
              std::vector<std::string>{"a", "b"},
          "dev features");
    check(manifest->dependencies.at("pin").git_ref() ==
              std::pair<std::string, std::string>{"rev", "deadbeef"},
          "pin ref is rev");
    check(manifest->dependencies.at("head").git_ref() ==
              std::pair<std::string, std::string>{"head", ""},
          "head ref defaults to head");
    check(manifest->dependencies.at("arc").is_archive(), "arc is archive");
    check(!fmt.is_archive() && !manifest->dependencies.at("head").is_archive(),
          "git urls are not archives");
    check(manifest->dependencies.at("loc").is_path_dep, "loc is path dep");

    // Validation failures.
    auto bad_refs = make_temp_project(
        "[package]\n"
        "name = \"t\"\n"
        "[dependencies]\n"
        "x = { url = \"https://example.com/x\", tag = \"v1\", branch = \"main\" }\n");
    check(!bake::Manifest::load(bad_refs), "two ref keys rejected");
    drop_temp_project(bad_refs);

    auto bad_archive = make_temp_project(
        "[package]\n"
        "name = \"t\"\n"
        "[dependencies]\n"
        "x = { url = \"https://example.com/x.tar.gz\", tag = \"v1\" }\n");
    check(!bake::Manifest::load(bad_archive), "archive with tag rejected");
    drop_temp_project(bad_archive);

    auto bad_bare = make_temp_project(
        "[package]\n"
        "name = \"t\"\n"
        "[dependencies]\n"
        "x = \"1.0.0\"\n");
    check(!bake::Manifest::load(bad_bare), "bare string dep rejected");
    drop_temp_project(bad_bare);

    drop_temp_project(dir);
    return ok;
}

bool effective_dependency_tests() {
    auto dir = make_temp_project(
        "[package]\n"
        "name = \"t\"\n"
        "\n"
        "[dependencies]\n"
        "fmt = { url = \"https://example.com/fmt\", tag = \"v1\" }\n"
        "\n"
        "[target.\"*-linux-musl\".dependencies]\n"
        "extra = { path = \"../extra\" }\n"
        "fmt = { url = \"https://example.com/fmt\", tag = \"v1\" }\n"
        "\n"
        "[target.\"*-linux-*\".dependencies]\n"
        "shared = { path = \"../shared\" }\n");
    auto manifest = bake::Manifest::load(dir);
    if (!manifest) {
        std::println(std::cerr, "effective manifest failed to load");
        drop_temp_project(dir);
        return false;
    }
    bool ok = true;
    auto check = [&](bool condition, std::string_view what) {
        if (!condition) {
            std::println(std::cerr, "effective check failed: {}", what);
            ok = false;
        }
    };

    auto musl = bake::effective_dependencies(*manifest, "x86_64-linux-musl", {});
    check(musl.has_value(), "musl resolves");
    if (musl) {
        check(musl->size() == 3, "musl set = fmt + extra + shared");
        check(musl->contains("extra") && musl->contains("shared"),
              "target deps present on matching triple");
    }

    auto darwin = bake::effective_dependencies(*manifest,
                                               "x86_64-apple-darwin", {});
    check(darwin.has_value(), "darwin resolves");
    if (darwin) {
        check(darwin->size() == 1 && darwin->contains("fmt"),
              "non-matching scopes invisible");
    }
    drop_temp_project(dir);

    auto conflict = make_temp_project(
        "[package]\n"
        "name = \"t\"\n"
        "\n"
        "[dependencies]\n"
        "dup = { url = \"https://example.com/a\", tag = \"v1\" }\n"
        "\n"
        "[target.\"*-linux-musl\".dependencies]\n"
        "dup = { url = \"https://example.com/b\", tag = \"v1\" }\n");
    auto cmanifest = bake::Manifest::load(conflict);
    check(cmanifest.has_value(), "conflict manifest loads");
    if (cmanifest) {
        auto result =
            bake::effective_dependencies(*cmanifest, "x86_64-linux-musl", {});
        check(!result.has_value(), "conflicting definition rejected");
        if (!result) {
            const std::string& error = result.error();
            check(error.find("[dependencies]") != std::string::npos &&
                      error.find("[target.\"*-linux-musl\".dependencies]") !=
                          std::string::npos &&
                      error.find("dup") != std::string::npos,
                  "conflict error names both scopes");
        }
        auto clean =
            bake::effective_dependencies(*cmanifest, "x86_64-apple-darwin", {});
        check(clean.has_value() && clean->size() == 1,
              "conflict invisible on non-matching triple");
    }
    drop_temp_project(conflict);
    return ok;
}

bool moid_judgment_tests() {
    auto native = make_temp_project("[package]\nname = \"n\"\n");
    auto loaded = bake::Manifest::load_moid(native);
    bool ok = loaded && loaded->moid->name == "n";
    if (!ok) std::println(std::cerr, "load_moid failed on native source");
    drop_temp_project(native);

    auto workspace_only =
        make_temp_project("[workspace]\nmembers = [\"x\"]\n");
    if (bake::Manifest::load_moid(workspace_only)) {
        std::println(std::cerr, "load_moid accepted non-moid source");
        ok = false;
    }
    drop_temp_project(workspace_only);

    auto empty = std::filesystem::temp_directory_path() /
                 ("bake_manifest_test_empty_" + std::to_string(++temp_counter));
    std::filesystem::create_directories(empty);
    if (bake::Manifest::load_moid(bake::Path(empty))) {
        std::println(std::cerr, "load_moid accepted missing bake.toml");
        ok = false;
    }
    std::error_code ignored;
    std::filesystem::remove_all(empty, ignored);

    auto path_only = make_temp_project(
        "[package]\n"
        "name = \"t\"\n"
        "[dependencies]\n"
        "loc = { path = \"../loc\" }\n");
    auto pmanifest = bake::Manifest::load(path_only);
    ok = ok && pmanifest &&
         bake::Manifest::has_only_path_deps(*pmanifest);
    if (!ok) std::println(std::cerr, "has_only_path_deps failed on path-only");
    auto remote = make_temp_project(
        "[package]\n"
        "name = \"t\"\n"
        "[dependencies]\n"
        "loc = { path = \"../loc\" }\n"
        "r = { url = \"https://example.com/r\", tag = \"v1\" }\n");
    auto rmanifest = bake::Manifest::load(remote);
    if (rmanifest && bake::Manifest::has_only_path_deps(*rmanifest)) {
        std::println(std::cerr, "has_only_path_deps accepted remote dep");
        ok = false;
    }
    drop_temp_project(path_only);
    drop_temp_project(remote);
    return ok;
}

// ===== glob tests (`*` / `**` / `?` semantics used by build.cpp patterns) =====

bool glob_tests() {
    auto dir = std::filesystem::temp_directory_path() /
               ("bake_glob_test_" + std::to_string(++temp_counter));
    std::filesystem::create_directories(dir / "src/a/b");
    std::ofstream(dir / "src/top.cpp") << "x";
    std::ofstream(dir / "src/a/mid.cpp") << "x";
    std::ofstream(dir / "src/a/b/leaf.cpp") << "x";
    std::ofstream(dir / "src/a/note.txt") << "x";

    auto names = [&](std::string_view pattern) {
        std::vector<std::string> out;
        for (auto& p : bake::glob(bake::Path(dir), pattern))
            out.push_back(p.filename_string());
        std::sort(out.begin(), out.end());
        return out;
    };
    auto expect = [&](std::string_view pattern,
                      const std::vector<std::string>& want) {
        auto got = names(pattern);
        bool ok = got == want;
        if (!ok) {
            std::println(std::cerr, "glob '{}' ->", pattern);
            for (auto& n : got) std::println(std::cerr, "  {}", n);
        }
        return ok;
    };

    bool ok = true;
    ok &= expect("src/*.cpp", {"top.cpp"});                          // `*` stays in one dir
    ok &= expect("src/**/*.cpp", {"leaf.cpp", "mid.cpp", "top.cpp"}); // `**` crosses, zero segments included
    ok &= expect("src/**", {"leaf.cpp", "mid.cpp", "note.txt", "top.cpp"});
    ok &= expect("src/a/m?d.cpp", {"mid.cpp"});                      // `?` is one char
    ok &= expect("src/a?mid.cpp", {});                               // `?` is not '/'
    ok &= expect("src/**/b/*.cpp", {"leaf.cpp"});
    if (!ok) std::println(std::cerr, "glob_tests failed");
    drop_temp_project(bake::Path(dir));
    return ok;
}

bool semver_tests() {
    using bake::semver::Req;
    using bake::semver::Version;
    bool ok = true;
    auto V = [&](const char* s) {
        auto v = Version::parse(s);
        if (!v) ok = false;
        return v.value_or(Version{});
    };
    auto R = [&](const char* s) {
        auto r = Req::parse(s);
        if (!r) ok = false;
        return r.value_or(Req{});
    };
    auto sat = [&](const char* r, const char* v) {
        std::vector<Req> set{R(r)};
        return bake::semver::satisfies_all(V(v), set);
    };

    // Parsing: tolerant 'v', strict otherwise.
    ok &= ((V("1.2.3").to_string() == "1.2.3"));
    ok &= ((V("v1.2.3").major == 1));
    ok &= (!Version::parse("1.2").has_value());         // missing patch
    ok &= (!Version::parse("1.2.3.4").has_value());     // quad
    ok &= (!Version::parse("01.2.3").has_value());      // leading zero
    ok &= (!Version::parse("1.2.3-").has_value());      // empty prerelease
    ok &= ((V("1.2.3-beta.2").pre.size() == 2));
    ok &= ((V("1.2.3+meta").build == "meta"));

    // Precedence: numeric fields, release > prerelease, numeric
    // identifiers < alphanumeric, longer identifier list wins.
    ok &= ((V("2.0.0") > V("1.99.99")));
    ok &= ((V("1.2.3") > V("1.2.3-rc.1")));
    ok &= ((V("1.2.3-alpha") < V("1.2.3-beta")));
    ok &= ((V("1.2.3-1") < V("1.2.3-alpha")));   // numeric < alpha
    ok &= ((V("1.2.3-rc.1") < V("1.2.3-rc.1.1")));
    ok &= ((V("1.2.3+build.1") == V("1.2.3")));  // build ignored

    // Requirement kinds.
    ok &= (sat("1.2.3", "1.2.3"));
    ok &= (!sat("1.2.3", "1.2.4"));
    ok &= (sat("^1.2.3", "1.2.3") && sat("^1.2.3", "1.9.9"));
    ok &= (!sat("^1.2.3", "2.0.0") && !sat("^1.2.3", "1.2.2"));
    ok &= (sat("^0.2.3", "0.2.9") && !sat("^0.2.3", "0.3.0"));
    ok &= (sat("^0.0.3", "0.0.3") && !sat("^0.0.3", "0.0.4"));
    ok &= (sat("~1.2.3", "1.2.9") && !sat("~1.2.3", "1.3.0"));
    ok &= (sat("~1.2", "1.2.0") && !sat("~1.2", "1.3.0"));
    ok &= (sat("~1", "1.9.9") && !sat("~1", "2.0.0"));
    ok &= (sat("1.2", "1.2.5") && !sat("1.2", "1.3.0"));
    ok &= (sat("1", "1.4.0") && !sat("1", "2.0.0"));
    ok &= (sat("1.x", "1.4.0") && !sat("1.2.x", "1.3.0"));
    ok &= (sat("*", "0.0.1") && sat("*", "99.0.0"));
    ok &= (sat("=1.2.3", "1.2.3"));
    ok &= (sat("^1.2.3-beta.1", "1.2.3-beta.4"));

    // Prerelease gating: ranges never match prereleases of other
    // triples, `*` never matches prereleases.
    ok &= (!sat("^1.2.3", "1.9.0-beta"));
    ok &= (!sat("*", "1.2.3-rc.1"));
    ok &= (!sat("1.2", "1.2.4-rc"));

    // Set-level prerelease rule: one admitting requirement unblocks the
    // whole set (node-semver semantics).
    {
        std::vector<Req> both{R("1.5.7-r1"), R("^1.5")};
        ok &= (bake::semver::satisfies_all(V("1.5.7-r1"), both));
        ok &= (!bake::semver::satisfies_all(V("1.5.8-beta"), both));
        std::vector<Req> star{R("*")};
        ok &= (!bake::semver::satisfies_all(V("1.2.3-rc.1"), star));
    }

    // Rejected syntax.
    ok &= (!Req::parse("").has_value());
    ok &= (!Req::parse(">=1.2").has_value());   // comparator lists unsupported
    ok &= (!Req::parse("^1.2.3 <2").has_value());
    ok &= (Req::parse("^x")->kind == Req::Kind::Any);  // npm: ^x ≡ *
    ok &= (sat("^0", "0.9.9") && !sat("^0", "1.0.0"));
    ok &= (sat("^0.0", "0.0.9") && !sat("^0.0", "0.1.0"));
    ok &= (!Req::parse("1.2.3.4").has_value());

    if (!ok) std::println(std::cerr, "semver_tests failed");
    return ok;
}

} // namespace

int main() {
    bake::MoidGraph graph;
    graph.nodes.emplace(bake::MoidId{"root"}, make_node("root"));
    graph.nodes.emplace(bake::MoidId{"orphan"}, make_node("orphan"));
    bake::MoidGraph rootless;
    rootless.nodes.emplace(bake::MoidId{"rootless"}, make_node("rootless"));
    if (!reports_orphan(rootless, "rootless")) return 1;

    if (!manifest_form_tests()) return 1;
    if (!effective_dependency_tests()) return 1;
    if (!glob_tests()) return 1;
    if (!semver_tests()) return 1;

    return 0;
}
