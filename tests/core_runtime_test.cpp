#include <kota/async/io/loop.h>
#include <kota/async/io/watcher.h>
#include <kota/async/runtime/task.h>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

import std;
import bake.core.runtime;

namespace {

std::string executable_path;
int background_count = 64;
int concurrency_limit = 3;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

int update_max(std::atomic<int>& active, std::atomic<int>& maximum) {
    int now = active.fetch_add(1) + 1;
    int observed = maximum.load();
    while (observed < now && !maximum.compare_exchange_weak(observed, now)) {}
    return now;
}

bake::ProcessSpec child(std::string mode,
                        std::vector<bake::EnvironmentOverride> environment = {}) {
    bake::ProcessSpec spec;
    spec.file = executable_path;
    spec.args = {executable_path, "--child", std::move(mode)};
    spec.environment = std::move(environment);
    return spec;
}

kota::task<> delayed_wake(kota::event_loop& loop,
                          std::shared_ptr<std::atomic<bool>> completed) {
    co_await kota::sleep(1500, loop);
    completed->store(true, std::memory_order_release);
}

kota::task<bool> fairness_case() {
    kota::event_loop& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(
        loop, {.max_concurrency = static_cast<std::size_t>(concurrency_limit),
               .background_concurrency =
                   static_cast<std::size_t>(std::max(1, concurrency_limit - 1))});
    std::atomic<int> active = 0;
    std::atomic<int> maximum = 0;
    std::atomic<int> completed_background = 0;
    std::vector<bake::RuntimeRequest> background;
    for (int i = 0; i < background_count; ++i) {
        background.push_back(runtime.submit_blocking(
            [&](const bake::CancellationToken& token) {
                update_max(active, maximum);
                for (int tick = 0; tick < 20 && !token.cancellation_requested(); ++tick)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                active.fetch_sub(1);
                completed_background.fetch_add(1);
                return bake::BlockingResult{};
            }, {.lane = bake::RuntimeLane::background}));
    }
    auto foreground = runtime.submit_blocking(
        [&](const bake::CancellationToken&) {
            update_max(active, maximum);
            active.fetch_sub(1);
            bake::BlockingResult result;
            result.value = completed_background.load();
            return result;
        }, {.lane = bake::RuntimeLane::foreground});
    auto foreground_result = co_await foreground.wait();
    check(foreground_result.succeeded(), "foreground request failed");
    check(std::any_cast<int>(foreground_result.value) < background_count,
          "foreground did not progress until the background queue drained");
    for (const auto& request : background) {
        auto result = co_await request.wait();
        check(result.succeeded(), "background request failed");
    }
    check(maximum.load() == concurrency_limit,
          "configured concurrency bound was not observed");
    co_await runtime.shutdown();

    bake::CoreRuntime single(loop, {.max_concurrency = 1,
                                    .background_concurrency = 1});
    auto one = single.submit_blocking(
        [](const bake::CancellationToken&) { return bake::BlockingResult{}; },
        {.lane = bake::RuntimeLane::foreground});
    check((co_await one.wait()).succeeded(),
          "single-slot foreground request deadlocked");
    co_await single.shutdown();
    co_return true;
}

kota::task<bool> dependencies_case() {
    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(loop, {.max_concurrency = 2,
                                     .background_concurrency = 1});
    std::mutex order_mutex;
    std::vector<int> order;
    auto first = runtime.submit_blocking([&](const bake::CancellationToken&) {
        std::lock_guard lock(order_mutex);
        order.push_back(1);
        return bake::BlockingResult{};
    });
    auto second = runtime.submit_blocking([&](const bake::CancellationToken&) {
        std::lock_guard lock(order_mutex);
        order.push_back(2);
        return bake::BlockingResult{};
    }, {.dependencies = {first}});
    auto failed = runtime.submit_process(child("fail"));
    auto blocked = runtime.submit_blocking(
        [&](const bake::CancellationToken&) {
            std::lock_guard lock(order_mutex);
            order.push_back(3);
            return bake::BlockingResult{};
        }, {.dependencies = {failed}});
    check((co_await second.wait()).succeeded(), "dependent request failed");
    check(!(co_await failed.wait()).succeeded(), "failing child reported success");
    auto blocked_result = co_await blocked.wait();
    check(!blocked_result.succeeded(), "failed prerequisite ran its dependent");
    check(order == std::vector<int>({1, 2}), "dependency execution order was wrong");
    co_await runtime.shutdown();
    co_return true;
}

kota::task<bool> environment_case() {
    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(loop, {.max_concurrency = 2,
                                     .background_concurrency = 2});
    auto first = runtime.submit_process(child(
        "env", {{.name = "BAKE_RUNTIME_PROBE", .value = "first"}}));
    auto second = runtime.submit_process(child(
        "env", {{.name = "BAKE_RUNTIME_PROBE", .value = "second"}}));
    auto first_result = co_await first.wait();
    auto second_result = co_await second.wait();
    check(first_result.succeeded() && second_result.succeeded(),
          "parallel environment child failed");
    check(first_result.stdout_data == "first\n", "first override leaked");
    check(second_result.stdout_data == "second\n", "second override leaked");
    check(std::getenv("BAKE_RUNTIME_PROBE") == nullptr,
          "runtime mutated the parent environment");
    check(first_result.stderr_data == "env-log\n" &&
          second_result.stderr_data == "env-log\n",
          "stderr was not captured separately");
    co_await runtime.shutdown();
    co_return true;
}

kota::task<bool> cancel_shutdown_case() {
    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(loop, {.max_concurrency = 1,
                                     .background_concurrency = 1});
    auto running = runtime.submit_process(child("sleep"));
    while (running.pid() <= 0) co_await kota::sleep(1, loop);
    int pid = running.pid();
    auto queued = runtime.submit_process(child("env"));
    queued.cancel();
    auto queued_result = co_await queued.wait();
    check(queued_result.state == bake::RuntimeRequestState::cancelled_before_start,
          "queued cancellation did not prevent start");
    check(queued_result.pid == -1, "cancelled queued process acquired a pid");
    running.cancel();
    auto running_result = co_await running.wait();
    check(!running_result.succeeded(), "cancelled running process succeeded");
    check(running_result.reaped, "running child was killed but not reaped");
#ifndef _WIN32
    errno = 0;
    check(::kill(pid, 0) == -1 && errno == ESRCH,
          "reaped process pid remains alive");
#endif

    std::atomic<bool> returned = false;
    auto blocking = runtime.submit_blocking([&](const bake::CancellationToken&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        returned.store(true);
        bake::BlockingResult result;
        result.value = 42;
        return result;
    });
    while (blocking.state() == bake::RuntimeRequestState::queued)
        co_await kota::sleep(1, loop);
    blocking.cancel();
    auto blocking_result = co_await blocking.wait();
    check(returned.load(), "running blocking callback did not settle");
    check(!blocking_result.succeeded() && !blocking_result.value.has_value(),
          "late blocking result was published after cancellation");
    co_await runtime.shutdown();

    bake::CoreRuntime wakeup(loop, {.max_concurrency = 4,
                                    .background_concurrency = 3});
    auto completion_watchdog = std::make_shared<std::atomic<bool>>(false);
    loop.schedule(delayed_wake(loop, completion_watchdog));
    auto completion_started = std::chrono::steady_clock::now();
    auto completion = wakeup.submit_process(child("env"));
    check((co_await completion.wait()).succeeded(),
          "request completion wakeup probe failed");
    check(std::chrono::steady_clock::now() - completion_started <
              std::chrono::seconds(1),
          "runtime request waited for an unrelated event-loop wakeup");
    while (!completion_watchdog->load(std::memory_order_acquire))
        co_await kota::sleep(1, loop);

    bake::WorkerPoolOptions wakeup_options;
    wakeup_options.worker = child("framed");
    auto wakeup_pool = wakeup.create_worker_pool(std::move(wakeup_options));
    auto idle_worker_a = wakeup.submit_worker(
        wakeup_pool, "idle-a", {.lane = bake::RuntimeLane::foreground});
    auto idle_worker_b = wakeup.submit_worker(
        wakeup_pool, "idle-b", {.lane = bake::RuntimeLane::foreground});
    check((co_await idle_worker_a.wait()).succeeded() &&
              (co_await idle_worker_b.wait()).succeeded(),
          "shutdown wakeup control workers failed");
    std::vector<bake::RuntimeRequest> wakeup_processes;
    for (int index = 0; index < 3; ++index)
        wakeup_processes.push_back(wakeup.submit_process(
            child("sleep"), {.lane = bake::RuntimeLane::background}));
    wakeup_processes.push_back(wakeup.submit_process(
        child("sleep"), {.lane = bake::RuntimeLane::foreground}));
    for (;;) {
        if (std::ranges::all_of(wakeup_processes, [](const auto& request) {
                return request.pid() > 0;
            }))
            break;
        co_await kota::sleep(1, loop);
    }
    check(std::ranges::all_of(wakeup_processes, [](const auto& request) {
              return request.state() == bake::RuntimeRequestState::running;
          }),
          "shutdown probe did not reach four concurrent running processes");
    auto shutdown_watchdog = std::make_shared<std::atomic<bool>>(false);
    loop.schedule(delayed_wake(loop, shutdown_watchdog));
    auto wakeup_started = std::chrono::steady_clock::now();
    co_await wakeup.shutdown();
    auto wakeup_elapsed = std::chrono::steady_clock::now() - wakeup_started;
    check(wakeup_elapsed < std::chrono::seconds(1),
          "settled runtime waited for an unrelated event-loop wakeup");
    for (const auto& request : wakeup_processes)
        check((co_await request.wait()).reaped,
              "wakeup regression process was not reaped");
    while (!shutdown_watchdog->load(std::memory_order_acquire))
        co_await kota::sleep(1, loop);
    co_return true;
}

kota::task<bool> backpressure_cancel_case() {
    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(loop, {.max_concurrency = 1,
                                     .background_concurrency = 1});
    bake::WorkerPoolOptions options;
    options.worker = child("no-read");
    auto pool = runtime.create_worker_pool(std::move(options));
    std::string payload(8 * 1024 * 1024, 'x');
    auto request = runtime.submit_worker(
        pool, std::move(payload), {.lane = bake::RuntimeLane::foreground});
    while (request.pid() <= 0) co_await kota::sleep(1, loop);
    co_await kota::sleep(100, loop);
    check(request.state() == bake::RuntimeRequestState::running,
          "backpressure request did not remain in flight");
    const int pid = request.pid();
    std::println(std::cerr,
                 "BACKPRESSURE pending request={} worker_pid={} payload_bytes={}",
                 request.id(), pid, 8 * 1024 * 1024);
    std::cerr.flush();
    co_await runtime.shutdown();
    auto result = co_await request.wait();
    std::println(std::cerr,
                 "BACKPRESSURE settled request={} state={} reaped={}",
                 request.id(), static_cast<int>(result.state), result.reaped);
    std::cerr.flush();
    check(result.state == bake::RuntimeRequestState::cancelled_after_start,
          "backpressured worker request did not settle as cancelled");
    check(result.reaped, "backpressured worker was killed but not reaped");
#ifndef _WIN32
    errno = 0;
    check(::kill(pid, 0) == -1 && errno == ESRCH,
          "backpressured worker pid remains alive");
#endif
    co_return true;
}

kota::task<bool> worker_crash_case() {
    auto& loop = kota::event_loop::current();
    {
        bake::CoreRuntime runtime(loop, {.max_concurrency = 2,
                                         .background_concurrency = 1});
        bake::WorkerPoolOptions options;
        options.worker = child("framed");
        auto pool = runtime.create_worker_pool(std::move(options));
        auto crashed = co_await runtime.submit_worker(
            pool, "crash", {.lane = bake::RuntimeLane::foreground}).wait();
        check(!crashed.succeeded() && crashed.reaped,
              "crashed worker was not reaped");
        auto response = co_await runtime.submit_worker(
            pool, "hello", {.lane = bake::RuntimeLane::foreground}).wait();
        check(response.succeeded(), "replacement worker request failed");
        auto reply = std::any_cast<bake::WorkerReply>(response.value);
        check(reply.ok && reply.payload == "reply:hello",
              "worker did not respawn with a fresh response generation");
        check(reply.stderr_data.empty(), "protocol stdout mixed with stderr");
        int pid = reply.pid;
        auto in_flight = runtime.submit_worker(
            pool, "sleep", {.lane = bake::RuntimeLane::foreground});
        while (in_flight.pid() <= 0) co_await kota::sleep(1, loop);
        co_await runtime.shutdown();
        auto in_flight_result = co_await in_flight.wait();
        check(!in_flight_result.succeeded() && in_flight_result.reaped,
              "shutdown did not settle and reap the in-flight worker call");
#ifndef _WIN32
        errno = 0;
        check(::kill(pid, 0) == -1 && errno == ESRCH,
              "worker pool shutdown did not reap the child");
#endif
    }

#ifndef _WIN32
    bake::CoreRuntime draining(loop, {.max_concurrency = 1,
                                      .background_concurrency = 1});
    auto helper_path = std::filesystem::temp_directory_path() /
        ("bake-runtime-helper-" + std::to_string(::getpid()) + ".pid");
    std::error_code helper_error;
    std::filesystem::remove(helper_path, helper_error);
    bake::WorkerPoolOptions draining_options;
    draining_options.worker = child(
        "framed", {{.name = "BAKE_RUNTIME_HELPER_PID_PATH",
                     .value = helper_path.string()}});
    auto draining_pool = draining.create_worker_pool(std::move(draining_options));
    auto held_stderr = draining.submit_worker(
        draining_pool, "crash-stderr", {.lane = bake::RuntimeLane::foreground});
    while (held_stderr.pid() <= 0) co_await kota::sleep(1, loop);
    const int held_pid = held_stderr.pid();
    for (int attempt = 0; attempt < 100 && !std::filesystem::is_regular_file(helper_path);
         ++attempt)
        co_await kota::sleep(10, loop);
    check(std::filesystem::is_regular_file(helper_path),
          "stderr holder pid file was not created");
    std::ifstream helper_input(helper_path);
    int helper = -1;
    helper_input >> helper;
    check(helper > 0, "stderr holder pid file was invalid");
    struct HelperCleanup {
        int& pid;
        const std::filesystem::path& path;
        ~HelperCleanup() {
            if (pid > 0) (void)::kill(pid, SIGKILL);
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup_helper{helper, helper_path};
    bool worker_reaped = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        errno = 0;
        if (::kill(held_pid, 0) == -1 && errno == ESRCH) {
            worker_reaped = true;
            break;
        }
        co_await kota::sleep(10, loop);
    }
    check(worker_reaped &&
              held_stderr.state() == bake::RuntimeRequestState::running,
          "probe did not reach dead-worker stderr-drain state");
    auto shutdown_started = std::chrono::steady_clock::now();
    co_await draining.shutdown();
    auto shutdown_elapsed = std::chrono::steady_clock::now() - shutdown_started;
    auto held_result = co_await held_stderr.wait();
    check(shutdown_elapsed < std::chrono::seconds(1),
          "shutdown waited for a dead worker's inherited stderr handle");
    check(!held_result.succeeded() && held_result.reaped,
          "dead worker with inherited stderr was not reaped");
    auto marker = held_result.stderr_data.find("helper:");
    check(marker != std::string::npos,
          "stderr holder pid was not captured");
    check(std::stoi(held_result.stderr_data.substr(marker + 7)) == helper,
          "captured stderr holder pid did not match the child record");
    (void)::kill(helper, SIGKILL);
    for (int attempt = 0; attempt < 100 && ::kill(helper, 0) == 0; ++attempt)
        co_await kota::sleep(10, loop);
    errno = 0;
    check(::kill(helper, 0) == -1 && errno == ESRCH,
          "stderr holder cleanup did not settle");
    helper = -1;
    std::filesystem::remove(helper_path, helper_error);
#endif
    co_return true;
}

kota::task<bool> integrated_lanes_case() {
    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(loop, {.max_concurrency = 3,
                                     .background_concurrency = 2});
    bake::WorkerPoolOptions options;
    options.worker = child("framed");
    auto pool = runtime.create_worker_pool(std::move(options));

    auto build_process = runtime.submit_process(
        child("sleep"), {.lane = bake::RuntimeLane::background});
    auto background_analysis = runtime.submit_worker(
        pool, "sleep", {.lane = bake::RuntimeLane::background});
    while (build_process.pid() <= 0 || background_analysis.pid() <= 0)
        co_await kota::sleep(1, loop);
    auto queued_analysis = runtime.submit_worker(
        pool, "queued", {.lane = bake::RuntimeLane::background});
    auto foreground_analysis = runtime.submit_worker(
        pool, "foreground", {.lane = bake::RuntimeLane::foreground});
    auto foreground_result = co_await foreground_analysis.wait();
    check(foreground_result.succeeded(),
          "foreground worker did not progress under mixed saturation");
    auto foreground_reply =
        std::any_cast<bake::WorkerReply>(foreground_result.value);
    check(foreground_reply.payload == "reply:foreground",
          "foreground worker returned the wrong generation payload");
    check(queued_analysis.state() == bake::RuntimeRequestState::queued,
          "background worker escaped the shared lane budget");

    queued_analysis.cancel();
    build_process.cancel();
    background_analysis.cancel();
    auto queued_result = co_await queued_analysis.wait();
    auto build_result = co_await build_process.wait();
    auto background_result = co_await background_analysis.wait();
    check(queued_result.state ==
              bake::RuntimeRequestState::cancelled_before_start &&
              queued_result.pid == -1,
          "queued mixed worker started during cancellation");
    check(build_result.state == bake::RuntimeRequestState::cancelled_after_start &&
              build_result.reaped,
          "mixed build process cancellation did not reap");
    check(background_result.state ==
              bake::RuntimeRequestState::cancelled_after_start &&
              background_result.reaped && !background_result.value.has_value(),
          "mixed worker cancellation published stale output or did not reap");
    co_await runtime.shutdown();
    co_return true;
}

kota::task<bool> lifecycle_case() {
    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(loop, {.max_concurrency = 1,
                                     .background_concurrency = 1});
    bake::WorkerPoolOptions options;
    options.worker = child("framed");
    auto pool = runtime.create_worker_pool(std::move(options));

    auto captured = std::make_shared<int>(7);
    std::weak_ptr<int> released = captured;
    auto ancestor = runtime.submit_blocking(
        [captured](const bake::CancellationToken&) {
            bake::BlockingResult result;
            result.output = std::to_string(*captured);
            return result;
        });
    check((co_await ancestor.wait()).succeeded(), "ancestor request failed");
    captured.reset();
    check(released.expired(), "completed request retained its work closure");

    auto blocker = runtime.submit_process(child("sleep"));
    while (blocker.pid() <= 0) co_await kota::sleep(1, loop);
    auto dependent = runtime.submit_blocking(
        [](const bake::CancellationToken&) { return bake::BlockingResult{}; },
        {.dependencies = {ancestor}});
    ancestor = {};
    blocker.cancel();
    check((co_await blocker.wait()).reaped, "dependency blocker was not reaped");
    check((co_await dependent.wait()).succeeded(),
          "queued dependency lost its completed ancestor ownership");

    bake::RuntimeRequest invalid;
    auto invalid_dependent = runtime.submit_blocking(
        [](const bake::CancellationToken&) { return bake::BlockingResult{}; },
        {.dependencies = {invalid}});
    auto invalid_result = co_await invalid_dependent.wait();
    check(!invalid_result.succeeded() && invalid_result.pid == -1,
          "invalid dependency queued instead of settling");

    bake::CoreRuntime foreign(loop, {.max_concurrency = 1,
                                     .background_concurrency = 1});
    bake::WorkerPoolOptions foreign_options;
    foreign_options.worker = child("framed");
    auto foreign_pool = foreign.create_worker_pool(std::move(foreign_options));
    auto foreign_request = foreign.submit_blocking(
        [](const bake::CancellationToken&) { return bake::BlockingResult{}; });
    auto cross_runtime_dependency = runtime.submit_blocking(
        [](const bake::CancellationToken&) { return bake::BlockingResult{}; },
        {.dependencies = {foreign_request}});
    auto cross_runtime_pool = runtime.submit_worker(
        foreign_pool, "wrong-runtime", {.lane = bake::RuntimeLane::foreground});
    check(!(co_await cross_runtime_dependency.wait()).succeeded() &&
              !(co_await cross_runtime_pool.wait()).succeeded(),
          "foreign runtime ownership queued indefinitely");
    check((co_await foreign_request.wait()).succeeded(),
          "foreign runtime control request failed");
    co_await foreign.shutdown();

    co_await runtime.shutdown();
    std::atomic<bool> late_callback_ran = false;
    auto late_blocking = runtime.submit_blocking(
        [&](const bake::CancellationToken&) {
            late_callback_ran.store(true);
            return bake::BlockingResult{};
        });
    auto late_process = runtime.submit_process(child("env"));
    auto late_worker = runtime.submit_worker(
        pool, "late", {.lane = bake::RuntimeLane::foreground});
    check(!(co_await late_blocking.wait()).succeeded() &&
              !(co_await late_process.wait()).succeeded() &&
              !(co_await late_worker.wait()).succeeded(),
          "late submission succeeded after shutdown");
    check(!late_callback_ran.load() && late_process.pid() == -1 &&
              late_worker.pid() == -1,
          "late submission started work after shutdown");
    co_return true;
}

kota::task<bool> response_cancel_race_case() {
    auto& loop = kota::event_loop::current();
    {
        bake::CoreRuntime runtime(loop, {.max_concurrency = 1,
                                         .background_concurrency = 1});
        bake::WorkerPoolOptions options;
        options.worker = child("framed");
        auto pool = runtime.create_worker_pool(std::move(options));
        auto request = runtime.submit_worker(
            pool, "already-replied", {.lane = bake::RuntimeLane::foreground});
        check(request.state() == bake::RuntimeRequestState::running,
              "worker request was not assigned to the runtime slot");
        request.cancel();
        auto result = co_await request.wait();
        check(result.state == bake::RuntimeRequestState::cancelled_after_start,
              "response cancellation did not remain explicit");
        check(result.reaped && !result.value.has_value(),
              "response cancellation retained a worker or published stale payload");
        co_await runtime.shutdown();
    }

    bake::CoreRuntime runtime(loop, {.max_concurrency = 2,
                                     .background_concurrency = 1});
    bake::WorkerPoolOptions options;
    options.worker = child("framed");
    auto pool = runtime.create_worker_pool(std::move(options));
    auto cancelled = runtime.submit_worker(
        pool, "sleep", {.lane = bake::RuntimeLane::foreground});
    auto completing = runtime.submit_worker(
        pool, "short", {.lane = bake::RuntimeLane::foreground});
    while (cancelled.pid() <= 0 || completing.pid() <= 0)
        co_await kota::sleep(1, loop);
    const int cancelled_pid = cancelled.pid();
    auto queued = runtime.submit_worker(
        pool, "after-cancel", {.lane = bake::RuntimeLane::foreground});
    cancelled.cancel();
    check((co_await completing.wait()).succeeded(),
          "control worker did not release a runtime slot");
    auto queued_result = co_await queued.wait();
    auto cancelled_result = co_await cancelled.wait();
    check(queued_result.succeeded(),
          "queued request reused a worker generation during cancel-reap");
    check(queued_result.pid != cancelled_pid,
          "queued request replaced the worker being reaped");
    check(cancelled_result.reaped && !cancelled_result.value.has_value(),
          "cancel-reap overlap retained the old worker generation");
    co_await runtime.shutdown();
    co_return true;
}

bool read_exact(std::istream& input, char* data, std::size_t size) {
    input.read(data, static_cast<std::streamsize>(size));
    return input.good() || input.gcount() == static_cast<std::streamsize>(size);
}

int framed_child() {
    for (;;) {
        char header[4];
        if (!read_exact(std::cin, header, 4)) return 0;
        auto byte = [&](int i) {
            return static_cast<std::uint32_t>(
                static_cast<unsigned char>(header[i]));
        };
        std::uint32_t size = (byte(0) << 24) | (byte(1) << 16) |
                             (byte(2) << 8) | byte(3);
        std::string payload(size, '\0');
        if (!read_exact(std::cin, payload.data(), payload.size())) return 2;
        if (payload == "crash") return 7;
#ifndef _WIN32
        if (payload == "crash-stderr") {
            pid_t helper = ::fork();
            if (helper == 0) {
                ::close(STDIN_FILENO);
                ::close(STDOUT_FILENO);
                std::this_thread::sleep_for(std::chrono::seconds(3));
                ::_exit(0);
            }
            if (helper < 0) return 8;
            if (const char* path = std::getenv("BAKE_RUNTIME_HELPER_PID_PATH")) {
                std::ofstream output(path);
                output << helper << '\n';
            }
            std::println(std::cerr, "helper:{}", helper);
            std::cerr.flush();
            return 7;
        }
#endif
        if (payload == "sleep")
            std::this_thread::sleep_for(std::chrono::seconds(30));
        if (payload == "short")
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::string response = "reply:" + payload;
        std::uint32_t response_size = static_cast<std::uint32_t>(response.size());
        char response_header[4] = {
            static_cast<char>((response_size >> 24) & 0xff),
            static_cast<char>((response_size >> 16) & 0xff),
            static_cast<char>((response_size >> 8) & 0xff),
            static_cast<char>(response_size & 0xff),
        };
        std::cout.write(response_header, 4);
        std::cout.write(response.data(), static_cast<std::streamsize>(response.size()));
        std::cout.flush();
    }
}

int child_main(std::string_view mode) {
    if (mode == "framed") return framed_child();
    if (mode == "no-read") {
#ifndef _WIN32
        for (;;) ::pause();
#else
        std::mutex mutex;
        std::condition_variable wake;
        std::unique_lock lock(mutex);
        wake.wait(lock, [] { return false; });
#endif
        return 0;
    }
    if (mode == "env") {
        const char* value = std::getenv("BAKE_RUNTIME_PROBE");
        std::cout << (value ? value : "missing") << '\n' << std::flush;
        std::cerr << "env-log\n" << std::flush;
        return 0;
    }
    if (mode == "sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    if (mode == "fail") return 9;
    return 2;
}

}

int main(int argc, char** argv) {
    executable_path = std::filesystem::absolute(argv[0]).string();
    if (argc == 3 && std::string_view(argv[1]) == "--child")
        return child_main(argv[2]);

    std::string selected = "all";
    for (int i = 1; i + 1 < argc; ++i) {
        std::string_view argument(argv[i]);
        if (argument == "--case") selected = argv[i + 1];
        if (argument == "--background") background_count = std::stoi(argv[i + 1]);
        if (argument == "--limit") concurrency_limit = std::stoi(argv[i + 1]);
    }
    if (background_count < 1 || concurrency_limit < 1) {
        std::println(std::cerr, "background and limit must be positive");
        return 2;
    }
    try {
        int case_count = 0;
        auto run = [&](auto factory) {
            auto [passed] = kota::run(factory());
            check(passed.has_value() && *passed, "scenario returned false");
            ++case_count;
        };
        if (selected == "all" || selected == "fairness") run(fairness_case);
        if (selected == "all" || selected == "dependencies") run(dependencies_case);
        if (selected == "all" || selected == "environment") run(environment_case);
        if (selected == "all" || selected == "cancel-shutdown")
            run(cancel_shutdown_case);
        if (selected == "all" || selected == "worker-crash")
            run(worker_crash_case);
        if (selected == "all" || selected == "integrated-lanes")
            run(integrated_lanes_case);
        if (selected == "all" || selected == "lifecycle")
            run(lifecycle_case);
        if (selected == "all" || selected == "response-cancel-race")
            run(response_cancel_race_case);
        if (selected == "all" || selected == "backpressure-cancel")
            run(backpressure_cancel_case);
        check(case_count != 0, "unknown or empty case selection");
        std::println("PASS core_runtime_test case={} cases={}", selected,
                     case_count);
        return 0;
    } catch (const std::exception& error) {
        std::println(std::cerr, "FAIL core_runtime_test case={}: {}", selected,
                     error.what());
        return 1;
    }
}
