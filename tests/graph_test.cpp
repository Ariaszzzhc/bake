import std;
import bake.util;
import bake.buildsystem.project;
import bake.buildsystem.graph;
import bake.buildsystem.moid;

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
    if (!moid_judgment_tests()) return 1;

    return 0;
}
