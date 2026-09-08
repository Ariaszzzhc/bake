#ifdef _WIN32
  #include <process.h>
  #include <windows.h>
#else
  #include <errno.h>
  #include <signal.h>
  #include <sys/resource.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

import std;
import bake.json;
import bake.util;
import bake.core.model;
import bake.core.state;

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

bake::json::Value payload(std::string_view value) {
    bake::json::Value object = bake::json::Value::object();
    object["value"] = value;
    return object;
}

struct Fixture {
    bake::Path root;
    bake::ProjectContextKey context{bake::SHA256::hex("state-test-context")};

    Fixture() {
#ifdef _WIN32
        const auto process = static_cast<std::uint64_t>(_getpid());
#else
        const auto process = static_cast<std::uint64_t>(getpid());
#endif
        root = bake::Path(std::filesystem::temp_directory_path() /
            ("bake-core-state-" + std::to_string(process) + "-" +
             std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())));
        root.mkdir_recursive();
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root.fs(), ignored);
    }
};

int writer_child(std::string_view root, std::string_view context, int index) {
    bake::ProjectStateStore store(bake::Path(std::string(root)),
                                  bake::ProjectContextKey{std::string(context)});
    auto snapshot = store.publish_snapshot("validation-shared", payload("shared"));
    if (!snapshot) return 10;
    auto result = store.publish_request_result(
        bake::BuildRequestId{"request-" + std::to_string(index)},
        bake::ResultStatus::ready, payload(std::to_string(index)));
    if (!result) return 11;
    auto observation = store.record_successful_observation(
        snapshot->first, "validation-shared");
    return observation ? 0 : 12;
}

int hold_temp_child(std::string_view root, std::string_view marker) {
    bake::Path snapshots = bake::Path(std::string(root)) / "snapshots";
    snapshots.mkdir_recursive();
    bake::write_file(snapshots / ".interrupted.publication.tmp", "{\"schema\":");
    bake::write_file(bake::Path(std::string(marker)), "ready");
    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 0;
}

#ifndef _WIN32
void stop_on_file_size_limit(int) {
    kill(getpid(), SIGSTOP);
}

int interrupted_writer_child(std::string_view root,
                             std::string_view context) {
    struct sigaction action {};
    action.sa_handler = stop_on_file_size_limit;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGXFSZ, &action, nullptr) != 0) return 20;

    constexpr rlim_t limit_bytes = 4096;
    const rlimit limit{.rlim_cur = limit_bytes, .rlim_max = limit_bytes};
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0) return 21;

    bake::ProjectStateStore store(bake::Path(std::string(root)),
                                  bake::ProjectContextKey{std::string(context)});
    const std::string value(64 * 1024, 'x');
    auto snapshot = store.publish_snapshot("interrupted", payload(value));
    return snapshot ? 22 : 23;
}

pid_t launch(const std::vector<std::string>& arguments) {
    pid_t pid = fork();
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

void identical_and_observation_case() {
    Fixture fixture;
    bake::ProjectStateStore store(fixture.root, fixture.context);
    auto first = store.publish_snapshot("mtime-a", payload("A"));
    check(first && first->second == bake::PublishDisposition::published,
          "first immutable snapshot is published");
    if (!first) return;
    bake::Path first_path = fixture.root / "snapshots" /
        (first->first.value + ".json");
    auto before = std::filesystem::last_write_time(first_path.fs());
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    auto repeated = store.publish_snapshot("mtime-a", payload("A"));
    auto after = std::filesystem::last_write_time(first_path.fs());
    check(repeated && repeated->second == bake::PublishDisposition::unchanged &&
              repeated->first == first->first,
          "byte-identical snapshot publish is an explicit no-op");
    check(before == after, "unchanged snapshot publish preserves file mtime");

    auto observation_a1 = store.record_successful_observation(first->first,
                                                               "mtime-a");
    auto second = store.publish_snapshot("mtime-b", payload("B"));
    const bool observation_b = second &&
        store.record_successful_observation(second->first, "mtime-b").has_value();
    auto observation_a2 = store.record_successful_observation(first->first,
                                                               "mtime-a");
    auto latest = store.latest_successful_observation();
    check(observation_a1 && observation_b && observation_a2 && latest,
          "successful observation sequence is durable");
    check(latest && latest->generation == 3 &&
              latest->snapshot == first->first &&
              latest->validation_id == "mtime-a",
          "A to B to A records A as the newest observation");
    check(std::filesystem::last_write_time(first_path.fs()) == before,
          "new A observation does not rewrite immutable A snapshot");

    bake::ContextPaths paths{.bake = fixture.root, .core = fixture.root};
    bake::ProjectPublicationGuard publication(paths);
    bake::Path shard = fixture.root / "analysis" / "shard.json";
    bake::CoreStateValidator validate = [shard](std::string_view content)
        -> std::expected<void, bake::StateError> {
        try {
            auto document = bake::json::Value::parse(content);
            const auto* value = document.find("value");
            if (document.is_object() && document.size() == 1 && value &&
                value->is_string())
                return {};
        } catch (const std::exception&) {
        }
        return std::unexpected(bake::StateError{
            bake::StateErrorCode::malformed, shard,
            "analysis shard is malformed",
        });
    };
    auto shard_first = bake::publish_immutable_file_recovering_corruption(
        shard, "{\"value\":\"A\"}", publication, validate);
    check(shard_first && shard_first == bake::PublishDisposition::published,
          "narrow immutable state primitive publishes validated content");
    bake::write_file(shard, "{broken");
    auto shard_repaired = bake::publish_immutable_file_recovering_corruption(
        shard, "{\"value\":\"A\"}", publication, validate);
    check(shard_repaired &&
              shard_repaired == bake::PublishDisposition::published &&
              bake::read_file(shard) == std::optional<std::string>(
                  "{\"value\":\"A\"}"),
          "narrow immutable state primitive replaces corrupt content");
    auto valid_conflict = bake::publish_immutable_file_recovering_corruption(
        shard, "{\"value\":\"B\"}", publication, validate);
    check(!valid_conflict &&
              valid_conflict.error().code == bake::StateErrorCode::conflict,
          "narrow immutable state primitive preserves valid differing content");
    auto replaced_shard = bake::replace_core_state_file(
        shard, "{\"value\":\"B\"}", publication, validate);
    check(replaced_shard && bake::read_file(shard) ==
              std::optional<std::string>("{\"value\":\"B\"}"),
          "guarded Core state replacement publishes new validated content");
    auto rejected_replacement = bake::replace_core_state_file(
        shard, "{broken", publication, validate);
    check(!rejected_replacement &&
              rejected_replacement.error().code ==
                  bake::StateErrorCode::malformed &&
              bake::read_file(shard) ==
                  std::optional<std::string>("{\"value\":\"B\"}"),
          "guarded Core state replacement rejects malformed new content");
}

void multiwriter_case(const std::string& self, int writers) {
    Fixture fixture;
#ifdef _WIN32
    std::vector<intptr_t> processes;
    for (int i = 0; i < writers; ++i) {
        std::vector<std::string> args = {
            self, "--writer", fixture.root.string(), fixture.context.value,
            std::to_string(i),
        };
        std::vector<const char*> raw;
        for (const auto& arg : args) raw.push_back(arg.c_str());
        raw.push_back(nullptr);
        processes.push_back(_spawnv(_P_NOWAIT, self.c_str(), raw.data()));
    }
    for (auto process : processes) {
        DWORD exit_code = 1;
        WaitForSingleObject(reinterpret_cast<HANDLE>(process), INFINITE);
        GetExitCodeProcess(reinterpret_cast<HANDLE>(process), &exit_code);
        CloseHandle(reinterpret_cast<HANDLE>(process));
        check(exit_code == 0, "writer subprocess exits successfully");
    }
#else
    std::vector<pid_t> processes;
    for (int i = 0; i < writers; ++i) {
        processes.push_back(launch({
            self, "--writer", fixture.root.string(), fixture.context.value,
            std::to_string(i),
        }));
    }
    for (pid_t process : processes)
        check(reap_success(process), "writer subprocess exits and is reaped");
#endif

    bake::ProjectStateStore store(fixture.root, fixture.context);
    auto ids = store.request_result_ids();
    check(ids.size() == static_cast<std::size_t>(writers),
          "every concurrent request result is retained");
    for (int i = 0; i < writers; ++i) {
        auto loaded = store.load_request_result(
            bake::BuildRequestId{"request-" + std::to_string(i)});
        check(loaded && loaded->status == bake::ResultStatus::ready,
              "concurrent request result strictly reloads");
    }
    auto scan = store.scan_snapshot_candidates();
    check(scan.valid.size() == 1 && scan.invalid.empty(),
          "concurrent identical snapshots produce one complete candidate");
    auto latest = store.latest_successful_observation();
    check(latest && latest->generation == static_cast<std::uint64_t>(writers),
          "cross-process successful observations serialize without loss");
}

bool has_invalid(const bake::SnapshotScan& scan, bake::StateErrorCode code) {
    return std::ranges::any_of(scan.invalid, [code](const auto& invalid) {
        return invalid.code == code;
    });
}

#ifndef _WIN32
void publication_interruption_case(const std::string& self) {
    Fixture fixture;
    bake::ProjectStateStore store(fixture.root, fixture.context);
    auto valid = store.publish_snapshot("last-good", payload("good"));
    check(valid.has_value(), "publication crash fixture has a last-good snapshot");
    if (!valid) return;
    auto observed = store.record_successful_observation(valid->first,
                                                         "last-good");
    check(observed.has_value(), "publication crash fixture records last-good");
    if (!observed) return;

    const bake::Path snapshots = fixture.root / "snapshots";
    const bake::Path valid_path = snapshots / (valid->first.value + ".json");
    const bake::Path latest_path = fixture.root / "latest.json";
    const auto valid_before = bake::read_file(valid_path);
    const auto latest_before = bake::read_file(latest_path);
    const auto valid_mtime = std::filesystem::last_write_time(valid_path.fs());
    const auto latest_mtime = std::filesystem::last_write_time(latest_path.fs());

    const pid_t child = launch({self, "--interrupt-publish",
                                fixture.root.string(), fixture.context.value});
    check(child > 0, "publication writer child starts");
    if (child <= 0) return;

    int status = 0;
    bool stopped = false;
    bool reaped = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        const pid_t waited = waitpid(child, &status, WNOHANG | WUNTRACED);
        if (waited == child) {
            stopped = WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP;
            reaped = !stopped;
            break;
        }
        if (waited < 0 && errno != EINTR) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(stopped, "real publication writer stops at the file-size boundary");

    std::vector<bake::Path> temporary_files;
    std::error_code iteration_error;
    for (const auto& entry :
         std::filesystem::directory_iterator(snapshots.fs(), iteration_error)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.starts_with(".") &&
            name.ends_with(".tmp"))
            temporary_files.emplace_back(entry.path());
    }
    const bool observed_temp = !iteration_error && temporary_files.size() == 1;
    std::uintmax_t temporary_size = 0;
    if (observed_temp)
        temporary_size = std::filesystem::file_size(temporary_files.front().fs());
    check(observed_temp && temporary_size > 0 && temporary_size <= 4096,
          "stopped writer leaves one bounded store-created publication temp");

    bool killed = false;
    if (!reaped) {
        const bool sent = kill(child, SIGKILL) == 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        reaped = WIFSIGNALED(status);
        killed = sent && reaped && WTERMSIG(status) == SIGKILL;
    }
    check(killed && reaped, "stopped publication writer is killed and reaped");

    const bool last_good_preserved =
        bake::read_file(valid_path) == valid_before &&
        std::filesystem::last_write_time(valid_path.fs()) == valid_mtime &&
        bake::read_file(latest_path) == latest_before &&
        std::filesystem::last_write_time(latest_path.fs()) == latest_mtime;
    check(last_good_preserved,
          "interruption preserves last-good snapshot and observation bytes and mtimes");

    const auto interrupted_scan = store.scan_snapshot_candidates();
    const bool no_partial_ready =
        interrupted_scan.valid.size() == 1 &&
        interrupted_scan.valid.front().id == valid->first &&
        interrupted_scan.invalid.empty();
    check(no_partial_ready,
          "store-created interrupted temp is neither partial nor ready state");

    auto recovered = store.publish_snapshot("recovered", payload("recovered"));
    const bool recovery_valid = recovered &&
        store.record_successful_observation(recovered->first, "recovered") &&
        store.load_snapshot(recovered->first, "recovered");
    check(recovery_valid, "normal publication succeeds after writer interruption");
    auto latest = store.latest_successful_observation();
    check(latest && recovered && latest->snapshot == recovered->first,
          "post-interruption publication becomes the successful observation");

    const std::string temp_path =
        observed_temp ? temporary_files.front().string() : "<missing>";
    std::println(
        "PUBLICATION_CRASH writer_pid={} temp={} temp_size={} stopped={} "
        "killed={} reaped={} last_good_preserved={} no_partial_ready={} "
        "recovery_valid={}",
        child, temp_path, temporary_size, stopped, killed, reaped,
        last_good_preserved, no_partial_ready, recovery_valid);
}
#endif

void crash_corrupt_case(const std::string& self) {
    Fixture fixture;
    bake::ProjectStateStore store(fixture.root, fixture.context);
    auto valid = store.publish_snapshot("last-good", payload("good"));
    check(valid.has_value(), "last good snapshot publishes");
    if (!valid) return;
    check(store.record_successful_observation(valid->first, "last-good").has_value(),
          "last good snapshot becomes the successful observation");

    bake::Path marker = fixture.root / "child-ready";
#ifdef _WIN32
    std::vector<std::string> args = {self, "--hold-temp", fixture.root.string(),
                                     marker.string()};
    std::vector<const char*> raw;
    for (const auto& arg : args) raw.push_back(arg.c_str());
    raw.push_back(nullptr);
    intptr_t child = _spawnv(_P_NOWAIT, self.c_str(), raw.data());
#else
    pid_t child = launch({self, "--hold-temp", fixture.root.string(),
                          marker.string()});
#endif
    for (int i = 0; i < 200 && !marker.is_regular_file(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(marker.is_regular_file(), "manual temp fixture reached reader phase");
#ifdef _WIN32
    TerminateProcess(reinterpret_cast<HANDLE>(child), 9);
    WaitForSingleObject(reinterpret_cast<HANDLE>(child), INFINITE);
    CloseHandle(reinterpret_cast<HANDLE>(child));
#else
    kill(child, SIGKILL);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    check(WIFSIGNALED(status), "manual temp fixture child is killed and reaped");
#endif

    bake::Path snapshots = fixture.root / "snapshots";
    auto write_hashed = [&](const bake::json::Value& document) {
        std::string content = document.dump();
        bake::write_file(snapshots / (bake::SHA256::hex(content) + ".json"),
                         content);
    };
    bake::json::Value unsupported = bake::json::Value::object();
    unsupported["schema"] = "bake-core-snapshot-v999";
    unsupported["context"] = fixture.context.value;
    unsupported["validation"] = "unsupported";
    unsupported["payload"] = payload("x");
    write_hashed(unsupported);

    bake::json::Value wrong_context = bake::json::Value::object();
    wrong_context["schema"] = "bake-core-snapshot-v1";
    wrong_context["context"] = bake::SHA256::hex("other-context");
    wrong_context["validation"] = "wrong";
    wrong_context["payload"] = payload("x");
    write_hashed(wrong_context);

    bake::write_file(snapshots /
        (std::string(64, '0') + ".json"), unsupported.dump());
    const std::string truncated = "{\"schema\":";
    bake::write_file(snapshots /
        (bake::SHA256::hex(truncated) + ".json"), truncated);

    auto scan = store.scan_snapshot_candidates();
    check(scan.valid.size() == 1 && scan.valid.front().id == valid->first,
          "corrupt candidates leave the last complete snapshot readable");
    check(has_invalid(scan, bake::StateErrorCode::unsupported_schema),
          "unsupported schema is reported explicitly");
    check(has_invalid(scan, bake::StateErrorCode::context_mismatch),
          "wrong context is reported explicitly");
    check(has_invalid(scan, bake::StateErrorCode::digest_mismatch),
          "bad digest is reported explicitly");
    check(has_invalid(scan, bake::StateErrorCode::malformed),
          "truncated JSON is reported explicitly");
    check(scan.valid.size() + scan.invalid.size() == 5,
          "temporary interrupted publication is never enumerated");
    auto latest = store.latest_successful_observation();
    check(latest && latest->snapshot == valid->first,
          "corruption cannot replace the successful observation");

    auto other = store.publish_snapshot("other", payload("other"));
    check(other.has_value(), "independent snapshot publishes before repair");
    bake::Path valid_path = fixture.root / "snapshots" /
        (valid->first.value + ".json");
    bake::write_file(valid_path, "corrupt");
    bake::ContextPaths paths{
        .bake = fixture.root,
        .core = fixture.root,
    };
    bake::ProjectPublicationGuard publication(paths);
    auto repaired = store.publish_snapshot_recovering_corruption(
        "last-good", payload("good"), publication);
    check(publication.locked() && repaired &&
              repaired->first == valid->first &&
              repaired->second == bake::PublishDisposition::published,
          "locked recomputation repairs a corrupt deterministic snapshot path");
    check(store.load_snapshot(valid->first, "last-good").has_value(),
          "repaired snapshot strictly reloads");
    check(other && store.load_snapshot(other->first, "other").has_value(),
          "repair preserves other immutable snapshot records");
}

}

int main(int argc, char** argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--writer")
        return writer_child(argv[2], argv[3], std::stoi(argv[4]));
    if (argc == 4 && std::string_view(argv[1]) == "--hold-temp")
        return hold_temp_child(argv[2], argv[3]);
#ifndef _WIN32
    if (argc == 4 && std::string_view(argv[1]) == "--interrupt-publish")
        return interrupted_writer_child(argv[2], argv[3]);
#endif

    std::string self = bake::get_self_exe_path();
    if (self.empty()) self = argv[0];
    try {
        if (argc >= 4 && std::string_view(argv[1]) == "--case" &&
            std::string_view(argv[2]) == "multiwriter") {
            int writers = 20;
            if (argc == 5 && std::string_view(argv[3]) == "--writers")
                writers = std::stoi(argv[4]);
            multiwriter_case(self, writers);
        } else if (argc == 3 && std::string_view(argv[1]) == "--case" &&
                   std::string_view(argv[2]) == "crash-corrupt") {
            crash_corrupt_case(self);
        } else if (argc == 3 && std::string_view(argv[1]) == "--case" &&
                   std::string_view(argv[2]) == "publication-crash") {
#ifdef _WIN32
            std::println(std::cerr,
                         "UNSUPPORTED core_state_test case=publication-crash "
                         "reason=RLIMIT_FSIZE-and-SIGXFSZ-are-POSIX-only");
            return 2;
#else
            publication_interruption_case(self);
            std::println("{} core_state_test case=publication-crash checks={} "
                         "failures={}", failures == 0 ? "PASS" : "FAIL",
                         checks, failures);
#endif
        } else {
            identical_and_observation_case();
            multiwriter_case(self, 20);
            crash_corrupt_case(self);
#ifndef _WIN32
            publication_interruption_case(self);
#else
            std::println("UNSUPPORTED core_state_test case=publication-crash "
                         "reason=RLIMIT_FSIZE-and-SIGXFSZ-are-POSIX-only");
#endif
        }
    } catch (const std::exception& exception) {
        std::println(std::cerr, "FAIL: unexpected exception: {}", exception.what());
        ++failures;
    }
    std::println("core_state_test: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
