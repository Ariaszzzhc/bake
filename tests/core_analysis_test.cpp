#include <kota/async/io/loop.h>
#include <kota/async/runtime/sync.h>
#include <kota/async/runtime/task.h>

#ifdef _WIN32
  #include <process.h>
#else
  #include <errno.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

import std;
import bake.core.analysis;
import bake.core.analysis.proto;
import bake.core.build;
import bake.core.index;
import bake.core.model;
import bake.core.occurrences;
import bake.core.project;
import bake.core.runtime;
import bake.core.snapshot;
import bake.json;
import bake.toolchain.compilation;
import bake.toolchain.target;
import bake.util;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, std::string_view message) {
    ++checks;
    if (condition) return;
    std::println(std::cerr, "FAIL: {}", message);
    ++failures;
}

bake::SymbolOccurrence occurrence(std::string usr, std::string name,
                                  unsigned line,
                                  std::string file = "/project/main.cpp") {
    bake::SymbolOccurrence value;
    value.usr = std::move(usr);
    value.name = std::move(name);
    value.kind = "Function";
    value.is_def = true;
    value.file = std::move(file);
    value.line = line;
    value.column = 5;
    return value;
}

bake::SymbolOccurrence declaration(std::string usr, std::string name,
                                   unsigned line) {
    auto value = occurrence(std::move(usr), std::move(name), line);
    value.is_def = false;
    value.is_decl = true;
    return value;
}

bake::ContextPaths index_paths(const bake::Path& root) {
    return {root, root / "bin", root / "lib", root / "obj", root / "bmi",
            root / ".bake", root / ".bake" / "core",
            root / ".bake" / "core" / "snapshots",
            root / ".bake" / "core" / "results",
            root / ".bake" / "core" / "work",
            root / ".bake" / "pch", root / ".bake" / "index"};
}

std::string concurrent_writer_key(int writer) {
    return bake::SHA256::hex("concurrent-index-key-" +
                             std::to_string(writer));
}

bool wait_for_file(const bake::Path& path) {
    for (int attempt = 0; attempt < 2'000; ++attempt) {
        if (path.is_regular_file()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

int concurrent_index_writer(std::string_view root_text,
                            std::string_view barrier_text, int writer) {
    bake::Path root{std::string(root_text)};
    bake::Path barrier{std::string(barrier_text)};
    if (!bake::write_file(
            barrier / ("arrived-" + std::to_string(writer)), "ready"))
        return 10;
    if (!wait_for_file(barrier / "release")) return 11;

    bake::ProjectIndex index(root / "concurrent-index", index_paths(root));
    const auto key = concurrent_writer_key(writer);
    const auto expected = occurrence(
        "concurrent-usr-" + std::to_string(writer),
        "concurrent-symbol-" + std::to_string(writer),
        static_cast<unsigned>(writer + 1),
        "/project/concurrent-" + std::to_string(writer) + ".cpp");
    if (!index.adopt(bake::CompilationUnitId{"concurrent-unit-" +
                                                std::to_string(writer)},
                     key, {expected}).has_value())
        return 12;
    return bake::write_file(
               barrier / ("completed-" + std::to_string(writer)), "adopted")
        ? 0 : 13;
}

#ifndef _WIN32
pid_t launch(const std::vector<std::string>& arguments) {
    const pid_t pid = fork();
    if (pid != 0) return pid;
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
}

bool reap_success(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

void occurrence_codec_case() {
    auto value = occurrence("usr", "value", 7, "/project/value.cpp");
    value.is_decl = true;
    value.is_ref = true;
    value.is_call = true;
    value.column = 9;
    value.decl_file = "/project/value.hpp";
    value.decl_line = 3;
    value.decl_column = 4;
    auto encoded = bake::encode_occurrence_batch({value});
    check(encoded.has_value(), "a valid occurrence batch is encoded");
    if (!encoded) return;
    auto decoded = bake::decode_occurrence_batch(*encoded);
    check(decoded && decoded->size() == 1 &&
              decoded->front().usr == value.usr &&
              decoded->front().name == value.name &&
              decoded->front().kind == value.kind &&
              decoded->front().is_decl && decoded->front().is_def &&
              decoded->front().is_ref && decoded->front().is_call &&
              decoded->front().file == value.file &&
              decoded->front().line == value.line &&
              decoded->front().column == value.column &&
              decoded->front().decl_file == value.decl_file &&
              decoded->front().decl_line == value.decl_line &&
              decoded->front().decl_column == value.decl_column,
          "occurrence batch roundtrip preserves every field");

    auto extra = *encoded;
    extra["extra"] = true;
    check(!bake::decode_occurrence_batch(extra),
          "occurrence batch rejects unexpected fields");
    bake::json::Value missing = bake::json::Value::object();
    missing["strings"] = bake::json::Value::array();
    check(!bake::decode_occurrence_batch(missing),
          "occurrence batch rejects a missing field");
    auto duplicate_string = *encoded;
    duplicate_string["strings"][1] =
        duplicate_string["strings"][0].get<std::string>();
    check(!bake::decode_occurrence_batch(duplicate_string),
          "occurrence batch rejects a duplicate string table entry");
    auto unsorted_strings = *encoded;
    auto first_string = unsorted_strings["strings"][0].get<std::string>();
    auto second_string = unsorted_strings["strings"][1].get<std::string>();
    unsorted_strings["strings"][0] = std::move(second_string);
    unsorted_strings["strings"][1] = std::move(first_string);
    check(!bake::decode_occurrence_batch(unsorted_strings),
          "occurrence batch rejects an unsorted string table");
    auto string_type = *encoded;
    string_type["strings"][0] = std::int64_t(0);
    check(!bake::decode_occurrence_batch(string_type),
          "occurrence batch rejects a non-string table entry");
    auto width = *encoded;
    bake::json::Value short_row = bake::json::Value::array();
    for (std::size_t i = 0; i < 9; ++i)
        short_row.push_back(width["rows"][0][i]);
    width["rows"][0] = std::move(short_row);
    check(!bake::decode_occurrence_batch(width),
          "occurrence batch rejects a row with the wrong width");
    auto type = *encoded;
    type["rows"][0][3] = "flags";
    check(!bake::decode_occurrence_batch(type),
          "occurrence batch rejects a non-integer field");
    auto index = *encoded;
    index["rows"][0][0] = std::int64_t(index["strings"].size());
    check(!bake::decode_occurrence_batch(index),
          "occurrence batch rejects an invalid string index");
    auto negative_index = *encoded;
    negative_index["rows"][0][0] = std::int64_t(-1);
    check(!bake::decode_occurrence_batch(negative_index),
          "occurrence batch rejects a negative string index");
    auto flags = *encoded;
    flags["rows"][0][3] = std::int64_t(16);
    check(!bake::decode_occurrence_batch(flags),
          "occurrence batch rejects invalid flags");
    auto position = *encoded;
    position["rows"][0][5] = std::int64_t(0);
    check(!bake::decode_occurrence_batch(position),
          "occurrence batch rejects an invalid source position");
    auto declaration_position = *encoded;
    declaration_position["rows"][0][8] = std::int64_t(-1);
    check(!bake::decode_occurrence_batch(declaration_position),
          "occurrence batch rejects a negative declaration position");
    auto overflow = *encoded;
    overflow["rows"][0][5] =
        std::int64_t(std::numeric_limits<unsigned>::max()) + 1;
    check(!bake::decode_occurrence_batch(overflow),
          "occurrence batch rejects numeric overflow");
    value.line = 0;
    check(!bake::encode_occurrence_batch({value}),
          "occurrence encoder rejects invalid typed input");

    value.line = 7;
    bake::analysis_proto::WireResponse response;
    response.id = 41;
    response.ok = true;
    response.occurrences.push_back(value);
    std::string frame = bake::analysis_proto::encode_response(response);
    auto wire = bake::analysis_proto::decode_response(
        std::string_view(frame).substr(4));
    check(wire && wire->id == 41 && wire->occurrences.size() == 1 &&
              wire->occurrences.front().usr == value.usr,
          "worker wire roundtrip uses the shared occurrence batch");
    std::string old_wire = frame.substr(4);
    auto identity = old_wire.find("bake-analysis-wire-v2");
    if (identity != std::string::npos)
        old_wire.replace(identity, std::string("bake-analysis-wire-v2").size(),
                         "bake-analysis-wire-v1");
    check(!bake::analysis_proto::decode_response(old_wire),
          "worker wire rejects an old protocol identity");
}

void source_view_case(const bake::Path& root) {
    bake::Path main = root / "main.cpp";
    bake::Path header = root / "value.hpp";
    bake::write_file(main, "int disk;\n");
    bake::write_file(header, "#define VALUE 1\n");

    bake::SourceViewStore views(
        bake::ProjectContextKey{"context"},
        bake::ProjectSnapshotId{"snapshot"});
    auto first = views.open_session();
    auto second = views.open_session();
    check(views.update(first, main, "int first;\n", 1).has_value(),
          "first session accepts main overlay");
    check(views.update(first, header, "#define VALUE 11\n", 1).has_value(),
          "first session accepts header overlay");
    check(views.update(second, main, "int second;\n", 1).has_value(),
          "second session accepts main overlay");
    check(views.update(second, header, "#define VALUE 22\n", 1).has_value(),
          "second session accepts header overlay");

    auto first_view = views.capture(first, {main, header});
    auto second_view = views.capture(second, {main, header});
    check(first_view.has_value() && second_view.has_value(),
          "both sessions capture source views");
    if (!first_view || !second_view) return;
    check(first_view->id != second_view->id,
          "different overlays have different source view identities");
    check(first_view->find(header)->content == "#define VALUE 11\n",
          "first session sees its header overlay");
    check(second_view->find(header)->content == "#define VALUE 22\n",
          "second session sees its header overlay");
    check(first_view->remapped_files().at(
              std::filesystem::weakly_canonical(main.fs()).generic_string()) ==
              "int first;\n",
          "worker remaps include captured main bytes");
    check(bake::read_file(header) == std::optional<std::string>(
              "#define VALUE 1\n"),
          "session overlays do not modify disk input");

    check(views.update(first, main, "int newer;\n", 2).has_value(),
          "newer document version is accepted");
    check(!views.is_current(*first_view),
          "an older source view becomes stale after an edit");
    check(views.is_current(*second_view),
          "one session edit does not invalidate another session");

    check(views.close_document(first, header).has_value(),
          "closing an overlay succeeds");
    auto disk_view = views.capture(first, {header});
    check(disk_view && disk_view->find(header) &&
              disk_view->find(header)->content == "#define VALUE 1\n",
          "closed overlay restores disk semantics");
    if (disk_view) {
        bake::write_file(header, "#define VALUE 2\n");
        check(!views.is_current(*disk_view),
              "captured disk bytes become stale after a content change");
        bake::write_file(header, "#define VALUE 1\n");
    }
}

void concurrent_index_writer_case(const std::string& self,
                                  const bake::Path& root) {
    constexpr int writer_count = 12;
    bake::Path barrier = root / "concurrent-index-barrier";
    barrier.mkdir_recursive();
    std::println("index concurrency root={} barrier={} writers={}",
                 root.string(), barrier.string(), writer_count);

#ifdef _WIN32
    std::vector<intptr_t> children;
#else
    std::vector<pid_t> children;
#endif
    std::vector<std::string> writer_pids;
    for (int writer = 0; writer < writer_count; ++writer) {
        std::vector<std::string> arguments = {
            self, "--index-writer", root.string(), barrier.string(),
            std::to_string(writer),
        };
#ifdef _WIN32
        std::vector<const char*> raw;
        raw.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) raw.push_back(argument.c_str());
        raw.push_back(nullptr);
        const intptr_t child = _spawnv(_P_NOWAIT, self.c_str(), raw.data());
        check(child != -1, "concurrent index writer process launches");
        if (child != -1) {
            children.push_back(child);
            writer_pids.push_back(std::to_string(child));
        }
#else
        const pid_t child = launch(arguments);
        check(child > 0, "concurrent index writer process launches");
        if (child > 0) {
            children.push_back(child);
            writer_pids.push_back(std::to_string(child));
        }
#endif
    }
    std::string pid_text;
    for (const auto& pid : writer_pids) {
        if (!pid_text.empty()) pid_text += ",";
        pid_text += pid;
    }
    std::println("index concurrency writer-pids={}", pid_text);

    bool all_arrived = children.size() == writer_count;
    for (int writer = 0; writer < writer_count; ++writer)
        all_arrived = all_arrived && wait_for_file(
            barrier / ("arrived-" + std::to_string(writer)));
    check(all_arrived,
          "all concurrent index writers reach the release barrier");
    check(bake::write_file(barrier / "release", "release"),
          "concurrent index writer barrier releases every child");

    std::size_t successful_reaps = 0;
    for (std::size_t writer = 0; writer < children.size(); ++writer) {
#ifdef _WIN32
        int status = 0;
        const intptr_t reaped = _cwait(&status, children[writer], 0);
        const bool reaped_cleanly = reaped == children[writer] && status == 0;
        check(reaped_cleanly,
              "concurrent index writer exits cleanly");
        successful_reaps += reaped_cleanly;
#else
        const bool reaped_cleanly = reap_success(children[writer]);
        check(reaped_cleanly,
              "concurrent index writer exits cleanly");
        successful_reaps += reaped_cleanly;
#endif
    }

    bool all_completed = true;
    for (int writer = 0; writer < writer_count; ++writer)
        all_completed = all_completed && (barrier /
            ("completed-" + std::to_string(writer))).is_regular_file();
    check(all_completed,
          "all released concurrent index writers report completed adoption");

    const bake::Path shards = root / "concurrent-index" / "shards";
    std::size_t shard_files = 0;
    std::size_t temporary_files = 0;
    std::error_code directory_error;
    std::filesystem::directory_iterator entries(shards.fs(), directory_error);
    check(!directory_error, "concurrent writer shard directory is readable");
    if (!directory_error) {
        for (const auto& entry : entries) {
            if (!entry.is_regular_file()) continue;
            ++shard_files;
            if (entry.path().filename().string().find(".tmp") !=
                std::string::npos)
                ++temporary_files;
        }
    }
    check(shard_files == writer_count && temporary_files == 0,
          "concurrent immutable shard publication leaves every complete shard");

    std::size_t strict_reopens = 0;
    for (int writer = 0; writer < writer_count; ++writer) {
        bake::ProjectIndex reopened(root / "concurrent-index", index_paths(root));
        const auto shard = reopened.read_shard(concurrent_writer_key(writer));
        const auto expected_usr = "concurrent-usr-" + std::to_string(writer);
        const auto expected_name = "concurrent-symbol-" + std::to_string(writer);
        const auto expected_file = "/project/concurrent-" +
            std::to_string(writer) + ".cpp";
        const bool exact = shard && *shard && (*shard)->key ==
                concurrent_writer_key(writer) &&
            (*shard)->occurrences.size() == 1 &&
            (*shard)->occurrences.front().usr == expected_usr &&
            (*shard)->occurrences.front().name == expected_name &&
            (*shard)->occurrences.front().is_def &&
            !(*shard)->occurrences.front().is_decl &&
            !(*shard)->occurrences.front().is_ref &&
            !(*shard)->occurrences.front().is_call &&
            (*shard)->occurrences.front().file == expected_file &&
            (*shard)->occurrences.front().line ==
                static_cast<unsigned>(writer + 1) &&
            (*shard)->occurrences.front().column == 5;
        check(exact,
              "independent index reopen strictly restores an exact writer shard");
        strict_reopens += exact;
    }
    std::println(
        "index concurrency arrived={} released={} reaped={} completed={} shards={} "
        "temporary-files={} strict-reopens={}",
        all_arrived ? writer_count : 0, (barrier / "release").is_regular_file(),
        successful_reaps, all_completed ? writer_count : 0, shard_files,
        temporary_files, strict_reopens);
}

void index_replacement_case(const bake::Path& root) {
    const bake::ContextPaths paths = index_paths(root);
    bake::ProjectIndex index(root / "index", paths);
    bake::CompilationUnitId unit{"unit"};
    const std::string foo_key = bake::SHA256::hex("foo-key");
    const std::string bar_key = bake::SHA256::hex("bar-key");
    check(index.adopt(unit, foo_key, {occurrence("foo-usr", "foo", 1)})
              .has_value(),
          "initial index shard is adopted");
    check(!index.find_definitions("foo-usr").empty(),
          "initial definition is visible");
    bake::ProjectIndex reopened(root / "index", paths);
    auto restored = reopened.read_shard(foo_key);
    check(restored && *restored &&
              reopened.install(unit, std::move(**restored)).has_value() &&
              reopened.find_definitions("foo-usr").size() == 1,
          "an existing valid shard restores its symbols");
    check(reopened.adopt(unit, bake::SHA256::hex("empty-key"), {})
              .has_value() &&
              reopened.find_definitions("foo-usr").empty(),
          "a newly collected empty shard removes old unit symbols");

    bake::Path shared_header = root / "shared.hpp";
    check(index.adopt(
              unit, bar_key, {occurrence("bar-usr", "bar", 2)},
              {root / "main.cpp", shared_header})
              .has_value(),
          "replacement index shard is adopted");
    check(index.find_definitions("foo-usr").empty(),
          "replacing a unit removes its old occurrences");
    auto definitions = index.find_definitions("bar-usr");
    check(definitions.size() == 1 && definitions.front().line == 2,
          "replacement definition is visible");
    check(index.occurrence_count() == 1,
          "replacement does not append historical occurrences");
    index.invalidate(shared_header);
    check(index.find_definitions("bar-usr").empty(),
          "header changes invalidate dependent unit contributions");

    bake::Path corrupt_root = root / "corrupt-index";
    bake::ProjectIndex corrupt(corrupt_root, paths);
    const std::string broken_key = bake::SHA256::hex("broken");
    bake::write_file(
        corrupt_root / "shards" / (broken_key + ".json"), "{}");
    check(corrupt.adopt(
              unit, broken_key,
              {occurrence("recovered", "recovered", 3)})
              .has_value(),
          "corrupt immutable shard is atomically recovered");
    check(corrupt.find_definitions("recovered").size() == 1,
          "recovered shard contributes its valid occurrences");

    bake::ProjectIndex cross_tu(root / "cross-index", paths);
    check(cross_tu.adopt(
              bake::CompilationUnitId{"caller"}, bake::SHA256::hex("caller"),
              {declaration("value-usr", "value", 2)})
              .has_value(),
          "caller declaration shard is adopted");
    check(cross_tu.adopt(
              bake::CompilationUnitId{"provider"},
              bake::SHA256::hex("provider"),
              {occurrence("value-usr", "value", 1)})
              .has_value(),
          "provider definition shard is adopted");
    auto cross_definitions = cross_tu.find_definitions("value-usr");
    check(cross_definitions.size() == 2 &&
              cross_definitions.front().line == 1,
          "cross-TU definition precedes a local forward declaration");
    check(cross_tu.has_definition("value-usr") &&
              !cross_tu.has_definition("missing-usr"),
          "index distinguishes a real definition from declarations");
    auto sources = cross_tu.definition_sources("value-usr");
    check(sources.size() == 1 &&
              sources.front().string() == "/project/main.cpp",
          "index exposes the source of real definitions for revalidation");

    bake::ProjectIndex multi(root / "multi-index", paths);
    auto caller = declaration("shared-usr", "shared", 4);
    caller.file = "/project/caller.cpp";
    auto provider = occurrence(
        "shared-usr", "shared", 8, "/project/provider.cpp");
    check(multi.adopt(
              bake::CompilationUnitId{"caller"}, bake::SHA256::hex("multi-caller"),
              {caller}, {root / "caller.cpp"})
              .has_value() &&
              multi.adopt(
                  bake::CompilationUnitId{"provider"},
                  bake::SHA256::hex("multi-provider"), {provider},
                  {root / "provider.cpp", shared_header})
                  .has_value(),
          "multiple unit contributions are adopted");
    check(multi.adopt(
              bake::CompilationUnitId{"provider"},
              bake::SHA256::hex("multi-provider-empty"), {})
              .has_value() &&
              multi.find_definitions("shared-usr").size() == 1 &&
              multi.find_references("shared-usr", true).size() == 1 &&
              multi.occurrence_count() == 1,
          "empty replacement removes only the replaced unit contribution");
    check(multi.adopt(
              bake::CompilationUnitId{"provider"},
              bake::SHA256::hex("multi-provider-restored"), {provider},
              {root / "provider.cpp", shared_header})
              .has_value(),
          "provider contribution can be restored after an empty result");
    multi.invalidate(shared_header);
    auto after_invalidation = multi.find_references("shared-usr", true);
    check(after_invalidation.size() == 1 &&
              after_invalidation.front().file == "/project/caller.cpp" &&
              multi.occurrence_count() == 1,
          "dependency invalidation removes only dependent unit contributions");

    bake::ProjectIndex conflict(root / "conflict-index", paths);
    const auto conflict_key = bake::SHA256::hex("same-key");
    check(conflict.adopt(
              bake::CompilationUnitId{"unit"}, conflict_key,
              {occurrence("conflict-usr", "conflict", 1)})
              .has_value(),
          "initial immutable occurrence document is published");
    check(!conflict.adopt(
               bake::CompilationUnitId{"unit"}, conflict_key,
               {occurrence("conflict-usr", "conflict", 2)})
               .has_value() &&
              conflict.find_definitions("conflict-usr").front().line == 1,
          "same-key conflicting adoption is rejected without replacing views");
}

void preamble_identity_case(const bake::Path& root) {
    bake::CompilationDescription description;
    description.id = bake::CompilationUnitId{"main"};
    description.context = bake::ProjectContextKey{"compiler-context-a"};
    description.working_directory = root;
    description.source = root / "main.cpp";
    description.driver_arguments = {"c++", "-c", description.source.string()};
    bake::CompilerIdentity compiler{
        root / "bake", "1", std::string(64, 'a')};

    bake::SourceView first;
    first.id = bake::SourceViewId{"first"};
    first.session = bake::AnalysisSessionId{"session"};
    std::string first_text = "#define VALUE 1\nint first;\n";
    first.files.emplace(
        std::filesystem::weakly_canonical(description.source.fs())
            .generic_string(),
        bake::SourceFileView{description.source, first_text,
                             bake::SHA256::hex(first_text), 1});
    bake::SourceView body = first;
    body.files.begin()->second.content =
        "#define VALUE 1\nint second;\n";
    body.files.begin()->second.digest = bake::SHA256::hex(
        body.files.begin()->second.content);
    bake::SourceView macro = first;
    macro.files.begin()->second.content =
        "#define VALUE x\nint first;\n";
    macro.files.begin()->second.digest = bake::SHA256::hex(
        macro.files.begin()->second.content);

    const std::size_t preamble_size = std::string("#define VALUE 1\n").size();
    auto first_key = bake::preamble_identity(
        description, compiler, first, preamble_size, {});
    auto body_key = bake::preamble_identity(
        description, compiler, body, preamble_size, {});
    auto macro_key = bake::preamble_identity(
        description, compiler, macro, preamble_size, {});
    check(first_key == body_key,
          "body-only edits preserve the exact preamble identity");
    check(first_key != macro_key,
          "same-size macro edits invalidate the exact preamble identity");
    description.context = bake::ProjectContextKey{"compiler-context-b"};
    check(first_key != bake::preamble_identity(
              description, compiler, first, preamble_size, {}),
          "compiler context participates in preamble identity");
}

struct FairnessState {
    bool background_complete = false;
    bool foreground_observed_pending = false;
    bool foreground_definition_ready = false;
    kota::event background_done;
    kota::event foreground_done;
};

kota::task<> observe_foreground(bake::CoreAnalysisService& analysis,
                                bake::AnalysisSessionId session,
                                bake::Path source, FairnessState& state);

kota::task<> run_background_index(bake::CoreAnalysisService& analysis,
                                  bake::AnalysisSessionId session,
                                  bake::Path foreground_source,
                                  FairnessState& state) {
    kota::event_loop::current().schedule(observe_foreground(
        analysis, session, std::move(foreground_source), state));
    co_await analysis.background_index(session);
    state.background_complete = true;
    state.background_done.set();
}

kota::task<> observe_foreground(bake::CoreAnalysisService& analysis,
                                bake::AnalysisSessionId session,
                                bake::Path source, FairnessState& state) {
    auto definitions = co_await analysis.definitions(
        session, source, 1, 5, "fairness_symbol_0");
    state.foreground_definition_ready = definitions &&
        definitions->locations.size() == 1 &&
        definitions->locations.front().file == source.string();
    state.foreground_observed_pending = !state.background_complete;
    state.foreground_done.set();
    co_return;
}

struct QueryState {
    bool ready = false;
    std::optional<bake::AnalysisErrorCode> error;
    kota::event done;
};

kota::task<> run_definition(bake::CoreAnalysisService& analysis,
                            bake::AnalysisSessionId session,
                            bake::Path source, QueryState& state) {
    auto result = co_await analysis.definitions(
        session, source, 1, 5, "fairness_symbol_0");
    state.ready = result.has_value();
    if (!result) state.error = result.error().code;
    state.done.set();
}

kota::task<> update_and_release(bake::CoreAnalysisService& analysis,
                                bake::AnalysisSessionId session,
                                bake::Path source,
                                std::atomic<bool>& release) {
    (void)analysis.update_document(
        session, source, "int dirty_overlay_symbol();\n", 1);
    release.store(true, std::memory_order_release);
    co_return;
}

kota::task<> close_all_and_release(bake::CoreAnalysisService& analysis,
                                   bake::AnalysisSessionId first,
                                   bake::AnalysisSessionId second,
                                   std::atomic<bool>& release) {
    analysis.close_session(first);
    analysis.close_session(second);
    release.store(true, std::memory_order_release);
    co_return;
}

bake::RuntimeRequest occupy_runtime(bake::CoreRuntime& runtime,
                                    std::atomic<bool>& release) {
    return runtime.submit_blocking(
        [&release](const bake::CancellationToken& token) {
            while (!release.load(std::memory_order_acquire) &&
                   !token.cancellation_requested())
                std::this_thread::yield();
            return bake::BlockingResult{};
        }, {.lane = bake::RuntimeLane::foreground});
}

kota::task<bool> warm_restore_fairness_case(const bake::Path& root) {
    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(
        loop, {.max_concurrency = 3, .background_concurrency = 2});
    bake::CoreBuildService builds(runtime);
    auto workspace = bake::Path(std::filesystem::canonical(root.fs()));
    bake::CompilerIdentity compiler{
        bake::Path("/usr/bin/false"), "index-fairness", std::string(64, 'a')};
    auto context = bake::make_project_context(
        workspace, std::nullopt, {}, "dev",
        bake::parse_target("aarch64-apple-darwin"), compiler);
    check(context.has_value(), "fairness context is valid");
    if (!context) co_return false;
    auto context_key = bake::context_key(*context);
    auto paths = bake::context_paths(*context);
    check(context_key.has_value() && paths.has_value(),
          "fairness context paths are valid");
    if (!context_key || !paths) co_return false;

    auto snapshot = std::make_shared<bake::ProjectSnapshot>();
    snapshot->context = *context;
    constexpr int unit_count = 8;
    std::vector<std::string> keys;
    keys.reserve(unit_count);
    for (int index = 0; index < unit_count; ++index) {
        const std::string symbol = "fairness_symbol_" + std::to_string(index);
        const std::string content = "int " + symbol + "();\n";
        const bake::Path source = workspace /
            ("fairness_" + std::to_string(index) + ".cpp");
        check(bake::write_file(source, content), "fairness source is written");
        bake::InputIdentity input{
            source, bake::SHA256::hex(content), content.size()};
        bake::CompilationDescription compilation;
        compilation.id = {"fairness-unit-" + std::to_string(index)};
        compilation.context = *context_key;
        compilation.owner_moid = "fairness";
        compilation.working_directory = workspace;
        compilation.source = source;
        compilation.driver_arguments = {"c++", "-c", source.string()};
        snapshot->source_graph.units.push_back(
            {compilation.id, "fairness", input, {}, std::nullopt, {}});
        std::map<std::string, std::string> captured{
            {std::filesystem::weakly_canonical(source.fs()).generic_string(),
             content}};
        keys.push_back(bake::ProjectIndex::shard_key(
            compilation, compiler, captured, {input, input}));
        snapshot->compilations.push_back(std::move(compilation));
    }

    bake::ProjectIndex populated(paths->index, *paths);
    for (int index = 0; index < unit_count; ++index) {
        const auto& compilation = snapshot->compilations[index];
        auto value = occurrence(
            "fairness-usr-" + std::to_string(index),
            "fairness_symbol_" + std::to_string(index), 1,
            compilation.source.string());
        check(populated.adopt(
                  compilation.id, keys[index], {value}, {compilation.source})
                  .has_value(),
              "warm fairness shard is populated");
    }

    bake::ProjectHandle handle;
    handle.context = *context;
    handle.paths = *paths;
    handle.snapshot_id = {bake::SHA256::hex("index-fairness-snapshot")};
    handle.snapshot = snapshot;
    {
        bake::CoreAnalysisService analysis(runtime, builds, std::move(handle));
        auto cancelled_session = analysis.open_session();
        auto live_session = analysis.open_session();
        FairnessState cancelled_state;
        FairnessState live_state;
        loop.schedule(run_background_index(
            analysis, cancelled_session, snapshot->compilations.front().source,
            cancelled_state));
        loop.schedule(run_background_index(
            analysis, live_session, snapshot->compilations.front().source,
            live_state));
        co_await cancelled_state.foreground_done.wait();
        co_await live_state.foreground_done.wait();
        check(cancelled_state.foreground_observed_pending &&
                  live_state.foreground_observed_pending &&
                  cancelled_state.foreground_definition_ready &&
                  live_state.foreground_definition_ready,
              "foreground Core queries progress while shared warm shards restore");
        analysis.close_session(cancelled_session);
        co_await cancelled_state.background_done.wait();
        co_await live_state.background_done.wait();

        for (int index = 0; index < unit_count; ++index) {
            const auto& compilation = snapshot->compilations[index];
            auto definitions = co_await analysis.definitions(
                live_session, compilation.source, 1, 5, {});
            check(definitions && definitions->locations.size() == 1 &&
                      definitions->locations.front().file ==
                          compilation.source.string(),
                  "warm restore preserves each indexed definition");
        }
        analysis.close_session(live_session);
    }
    co_await runtime.shutdown();

    auto make_handle = [&] {
        bake::ProjectHandle next;
        next.context = *context;
        next.paths = *paths;
        next.snapshot_id = {bake::SHA256::hex("index-fairness-snapshot")};
        next.snapshot = snapshot;
        return next;
    };
    const auto source = snapshot->compilations.front().source;

    {
        bake::CoreRuntime serial(loop, {
            .max_concurrency = 1, .background_concurrency = 1});
        bake::CoreBuildService serial_builds(serial);
        bake::CoreAnalysisService dirty(
            serial, serial_builds, make_handle());
        auto session = dirty.open_session();
        std::atomic<bool> release = false;
        auto blocker = occupy_runtime(serial, release);
        QueryState query;
        loop.schedule(run_definition(dirty, session, source, query));
        loop.schedule(update_and_release(
            dirty, session, source, release));
        co_await query.done.wait();
        check(!query.ready && query.error == bake::AnalysisErrorCode::cancelled,
              "dirty update cancels a queued warm restore without a stale result");
        (void)co_await blocker.wait();
        dirty.close_session(session);
        co_await serial.shutdown();
    }

    {
        bake::CoreRuntime serial(loop, {
            .max_concurrency = 1, .background_concurrency = 1});
        bake::CoreBuildService serial_builds(serial);
        bake::CoreAnalysisService closing(
            serial, serial_builds, make_handle());
        auto first = closing.open_session();
        auto second = closing.open_session();
        std::atomic<bool> release = false;
        auto blocker = occupy_runtime(serial, release);
        QueryState first_query;
        QueryState second_query;
        loop.schedule(run_definition(closing, first, source, first_query));
        loop.schedule(run_definition(closing, second, source, second_query));
        loop.schedule(close_all_and_release(
            closing, first, second, release));
        co_await first_query.done.wait();
        co_await second_query.done.wait();
        check(!first_query.ready && !second_query.ready &&
                  first_query.error == bake::AnalysisErrorCode::cancelled &&
                  second_query.error == bake::AnalysisErrorCode::cancelled,
              "closing every joined waiter cancels queued warm restoration");
        (void)co_await blocker.wait();
        co_await serial.shutdown();
    }

    {
        check(bake::write_file(
                  paths->index / "shards" / (keys.front() + ".json"), "{}"),
              "malformed warm shard is written");
        bake::CoreRuntime malformed_runtime(loop, {
            .max_concurrency = 1, .background_concurrency = 1});
        bake::CoreBuildService malformed_builds(malformed_runtime);
        bake::CoreAnalysisService malformed(
            malformed_runtime, malformed_builds, make_handle());
        auto session = malformed.open_session();
        auto result = co_await malformed.definitions(
            session, source, 1, 5, "fairness_symbol_0");
        check(!result,
              "malformed warm shard recollects instead of returning partial success");
        malformed.close_session(session);
        co_await malformed_runtime.shutdown();
    }
    co_return true;
}

}

int main(int argc, char** argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--index-writer")
        return concurrent_index_writer(argv[2], argv[3], std::stoi(argv[4]));

    auto nonce = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    bake::Path root(std::filesystem::temp_directory_path() /
                    ("bake-core-analysis-" + nonce));
    root.mkdir_recursive();
    occurrence_codec_case();
    source_view_case(root);
    std::string self = bake::get_self_exe_path();
    if (self.empty()) self = argv[0];
    concurrent_index_writer_case(self, root);
    index_replacement_case(root);
    preamble_identity_case(root);
    auto [fairness] = kota::run(warm_restore_fairness_case(root));
    check(fairness && *fairness, "warm restore fairness scenario completes");
    root.remove_all();
    if (failures != 0) return 1;
    std::println("core analysis contract passed: {} checks", checks);
    return 0;
}
