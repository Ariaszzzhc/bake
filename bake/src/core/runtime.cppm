module;

#include <kota/async/io/loop.h>
#include <kota/async/io/process.h>
#include <kota/async/io/request.h>
#include <kota/async/runtime/sync.h>
#include <kota/async/runtime/task.h>

#include <csignal>
#ifndef _WIN32
#include <unistd.h>
extern char** environ;
#endif

export module bake.core.runtime;

import std;

export namespace bake {

enum class RuntimeLane { foreground, background };

enum class RuntimeRequestState {
    queued,
    cancelled_before_start,
    running,
    running_cancel_requested,
    cancelled_after_start,
    completed,
    failed,
};

struct RuntimeLimits {
    std::size_t max_concurrency = 1;
    std::size_t background_concurrency = 0;
};

class WorkerPoolId {
public:
    auto operator<=>(const WorkerPoolId&) const = default;

private:
    WorkerPoolId(std::uint64_t runtime, std::uint64_t value)
        : runtime(runtime), value(value) {}
    std::uint64_t runtime = 0;
    std::uint64_t value = 0;
    friend class CoreRuntime;
};

struct EnvironmentOverride {
    std::string name;
    std::optional<std::string> value;
};

struct ProcessSpec {
    std::string file;
    std::vector<std::string> args;
    std::string cwd;
    std::vector<EnvironmentOverride> environment;
    std::string stdin_data;
};

struct BlockingResult {
    bool ok = true;
    std::any value;
    std::string output;
    std::string error;
};

struct RuntimeResult {
    RuntimeRequestState state = RuntimeRequestState::failed;
    std::any value;
    std::string output;
    std::string error;
    std::string stdout_data;
    std::string stderr_data;
    int exit_code = -1;
    int term_signal = 0;
    int pid = -1;
    bool reaped = false;

    bool succeeded() const noexcept {
        return state == RuntimeRequestState::completed;
    }
};

class CancellationToken {
public:
    bool cancellation_requested() const noexcept;

private:
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> flag);
    std::shared_ptr<std::atomic<bool>> m_flag;
    friend class CoreRuntime;
};

class RuntimeRequest {
public:
    RuntimeRequest() = default;

    std::uint64_t id() const noexcept;
    RuntimeRequestState state() const noexcept;
    int pid() const noexcept;
    void cancel() const;
    kota::task<RuntimeResult> wait() const;
    explicit operator bool() const noexcept;

private:
    struct Shared;
    static kota::task<RuntimeResult> wait_shared(std::shared_ptr<Shared> shared);
    explicit RuntimeRequest(std::shared_ptr<Shared> shared);
    std::shared_ptr<Shared> m_shared;
    friend class CoreRuntime;
};

struct WorkerPoolOptions {
    ProcessSpec worker;
    std::size_t crash_limit = 3;
    std::chrono::milliseconds crash_window = std::chrono::seconds(10);
};

struct WorkerReply {
    bool ok = false;
    std::string payload;
    std::string stderr_data;
    std::string error;
    int pid = -1;
    bool reaped = false;
    std::uint64_t generation = 0;
};

struct RuntimeSubmitOptions {
    RuntimeLane lane = RuntimeLane::background;
    std::vector<RuntimeRequest> dependencies;
};

class CoreRuntime {
public:
    using BlockingWork = std::function<BlockingResult(const CancellationToken&)>;

    explicit CoreRuntime(kota::event_loop& loop, RuntimeLimits limits = {});
    CoreRuntime(const CoreRuntime&) = delete;
    CoreRuntime& operator=(const CoreRuntime&) = delete;
    ~CoreRuntime();

    RuntimeRequest submit_blocking(BlockingWork work,
                                   RuntimeSubmitOptions options = {});
    RuntimeRequest submit_process(ProcessSpec process,
                                  RuntimeSubmitOptions options = {});
    WorkerPoolId create_worker_pool(WorkerPoolOptions options);
    RuntimeRequest submit_worker(WorkerPoolId pool, std::string payload,
                                 RuntimeSubmitOptions options = {});
    kota::task<> shutdown();
    bool shutting_down() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

std::vector<std::string> inherited_environment(
    const std::vector<EnvironmentOverride>& overrides = {});

}  // namespace bake

namespace bake {
namespace {

bool terminal(RuntimeRequestState state) {
    return state == RuntimeRequestState::cancelled_before_start ||
           state == RuntimeRequestState::cancelled_after_start ||
           state == RuntimeRequestState::completed ||
           state == RuntimeRequestState::failed;
}

void terminate_owned_process(kota::process& process) {
    int pid = process.pid();
    if (pid <= 0) return;
#ifndef _WIN32
    // libuv's detached flag is platform-dependent. A negative pid is safe
    // only after verifying that the child leads a distinct process group.
    if (::getpgid(pid) == pid) {
        if (::kill(-pid, SIGKILL) != 0) process.kill(SIGKILL);
    } else {
        process.kill(SIGKILL);
    }
#else
    process.kill(SIGKILL);
#endif
}

kota::task<> capture_pipe(kota::pipe pipe, std::string& output,
                          kota::relay& completions, kota::event& done) {
    for (;;) {
        auto chunk = co_await pipe.read();
        if (chunk.has_error()) break;
        output += chunk.value();
    }
    completions.send([&done] { done.set(); });
}

std::string frame_payload(std::string_view payload) {
    const auto size = static_cast<std::uint32_t>(payload.size());
    std::string frame(4, '\0');
    frame[0] = static_cast<char>((size >> 24) & 0xff);
    frame[1] = static_cast<char>((size >> 16) & 0xff);
    frame[2] = static_cast<char>((size >> 8) & 0xff);
    frame[3] = static_cast<char>(size & 0xff);
    frame.append(payload);
    return frame;
}

std::optional<std::string> take_frame(std::string& buffer) {
    if (buffer.size() < 4) return std::nullopt;
    auto byte = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(
            static_cast<unsigned char>(buffer[offset]));
    };
    std::uint32_t size = (byte(0) << 24) | (byte(1) << 16) |
                         (byte(2) << 8) | byte(3);
    if (size > 512u * 1024 * 1024) {
        buffer.clear();
        return std::nullopt;
    }
    if (buffer.size() < static_cast<std::size_t>(size) + 4) return std::nullopt;
    std::string payload = buffer.substr(4, size);
    buffer.erase(0, static_cast<std::size_t>(size) + 4);
    return payload;
}

}  // namespace

class RuntimeWorkerPool;

struct RuntimeRequest::Shared {
    std::uint64_t id = 0;
    void* owner = nullptr;
    RuntimeLane lane = RuntimeLane::background;
    RuntimeRequestState state = RuntimeRequestState::queued;
    RuntimeResult result;
    kota::event done;
    std::shared_ptr<std::atomic<bool>> cancelled =
        std::make_shared<std::atomic<bool>>(false);
    std::vector<std::shared_ptr<Shared>> dependencies;
    CoreRuntime::BlockingWork blocking;
    std::optional<ProcessSpec> process;
    std::shared_ptr<RuntimeWorkerPool> worker_pool;
    std::string worker_payload;
    kota::process* active_process = nullptr;
    std::function<void()> active_cancel;
    std::function<void()> cancel_callback;
};

CancellationToken::CancellationToken(std::shared_ptr<std::atomic<bool>> flag)
    : m_flag(std::move(flag)) {}

bool CancellationToken::cancellation_requested() const noexcept {
    return m_flag && m_flag->load(std::memory_order_acquire);
}

RuntimeRequest::RuntimeRequest(std::shared_ptr<Shared> shared)
    : m_shared(std::move(shared)) {}

std::uint64_t RuntimeRequest::id() const noexcept {
    return m_shared ? m_shared->id : 0;
}

RuntimeRequestState RuntimeRequest::state() const noexcept {
    return m_shared ? m_shared->state : RuntimeRequestState::failed;
}

int RuntimeRequest::pid() const noexcept {
    return m_shared ? m_shared->result.pid : -1;
}

void RuntimeRequest::cancel() const {
    if (m_shared && m_shared->cancel_callback) m_shared->cancel_callback();
}

kota::task<RuntimeResult> RuntimeRequest::wait_shared(
    std::shared_ptr<Shared> shared) {
    if (!shared) {
        RuntimeResult result;
        result.error = "invalid runtime request";
        co_return result;
    }
    if (!terminal(shared->state)) co_await shared->done.wait();
    co_return shared->result;
}

kota::task<RuntimeResult> RuntimeRequest::wait() const {
    return wait_shared(m_shared);
}

RuntimeRequest::operator bool() const noexcept { return m_shared != nullptr; }

std::vector<std::string> inherited_environment(
    const std::vector<EnvironmentOverride>& overrides) {
    std::map<std::string, std::string> values;
#ifdef _WIN32
    char** current = _environ;
#else
    char** current = ::environ;
#endif
    if (current) {
        for (char** entry = current; *entry; ++entry) {
            std::string item(*entry);
            auto equals = item.find('=');
            if (equals != std::string::npos && equals != 0)
                values[item.substr(0, equals)] = item.substr(equals + 1);
        }
    }
    for (const auto& override : overrides) {
        if (override.name.empty() || override.name.find('=') != std::string::npos)
            continue;
        if (override.value)
            values[override.name] = *override.value;
        else
            values.erase(override.name);
    }
    std::vector<std::string> environment;
    environment.reserve(values.size());
    for (auto& [name, value] : values)
        environment.push_back(name + "=" + value);
    return environment;
}

struct CoreRuntime::Impl {
    kota::event_loop& loop;
    RuntimeLimits limits;
    std::deque<std::shared_ptr<RuntimeRequest::Shared>> foreground;
    std::deque<std::shared_ptr<RuntimeRequest::Shared>> background;
    std::unordered_map<std::uint64_t,
                       std::shared_ptr<RuntimeRequest::Shared>> requests;
    std::vector<std::shared_ptr<RuntimeWorkerPool>> worker_pools;
    std::uint64_t next_id = 0;
    std::uint64_t runtime_id = 0;
    std::size_t active = 0;
    std::size_t active_background = 0;
    bool stopping = false;
    bool settlement_posted = false;
    kota::event settled;
    kota::relay completions;

    explicit Impl(kota::event_loop& loop, RuntimeLimits configured)
        : loop(loop), limits(configured), completions(loop.create_relay()) {
        static std::atomic<std::uint64_t> next_runtime_id{0};
        runtime_id = ++next_runtime_id;
        limits.max_concurrency = std::max<std::size_t>(1, limits.max_concurrency);
        if (limits.max_concurrency == 1) {
            limits.background_concurrency = 1;
        } else if (limits.background_concurrency == 0 ||
                   limits.background_concurrency >= limits.max_concurrency) {
            limits.background_concurrency = limits.max_concurrency - 1;
        }
    }

    bool dependencies_ready(const std::shared_ptr<RuntimeRequest::Shared>& item) {
        for (const auto& dep : item->dependencies)
            if (!dep || !terminal(dep->state)) return false;
        return true;
    }

    bool dependency_failed(const std::shared_ptr<RuntimeRequest::Shared>& item) {
        for (const auto& dep : item->dependencies) {
            if (!dep || dep->state != RuntimeRequestState::completed) return true;
        }
        return false;
    }

    void finish(const std::shared_ptr<RuntimeRequest::Shared>& item,
                RuntimeResult result) {
        item->active_process = nullptr;
        item->active_cancel = {};
        item->state = result.state;
        item->result = std::move(result);
        item->result.state = item->state;
        item->blocking = {};
        item->process.reset();
        item->worker_pool.reset();
        item->worker_payload.clear();
        item->dependencies.clear();
        completions.send([item] { item->done.set(); });
        requests.erase(item->id);
    }

    void cancel(const std::shared_ptr<RuntimeRequest::Shared>& item) {
        if (terminal(item->state)) return;
        item->cancelled->store(true, std::memory_order_release);
        if (item->state == RuntimeRequestState::queued) {
            RuntimeResult result;
            result.state = RuntimeRequestState::cancelled_before_start;
            result.error = "cancelled before start";
            finish(item, std::move(result));
            dispatch();
            return;
        }
        item->state = RuntimeRequestState::running_cancel_requested;
        if (item->active_cancel)
            item->active_cancel();
        else if (item->active_process)
            terminate_owned_process(*item->active_process);
    }

    std::shared_ptr<RuntimeRequest::Shared> next_ready(
        std::deque<std::shared_ptr<RuntimeRequest::Shared>>& queue) {
        for (auto it = queue.begin(); it != queue.end();) {
            auto item = *it;
            if (item->state != RuntimeRequestState::queued) {
                it = queue.erase(it);
                continue;
            }
            if (!dependencies_ready(item)) {
                ++it;
                continue;
            }
            it = queue.erase(it);
            if (dependency_failed(item)) {
                RuntimeResult result;
                result.state = RuntimeRequestState::failed;
                result.error = "prerequisite did not complete successfully";
                finish(item, std::move(result));
                continue;
            }
            return item;
        }
        return {};
    }

    void dispatch() {
        while (active < limits.max_concurrency) {
            auto item = next_ready(foreground);
            if (!item && active_background < limits.background_concurrency)
                item = next_ready(background);
            if (!item) break;
            item->state = RuntimeRequestState::running;
            item->result.state = item->state;
            ++active;
            if (item->lane == RuntimeLane::background) ++active_background;
            loop.schedule(execute(item));
        }
        if (stopping && active == 0) {
            bool pending = false;
            for (const auto& [id, item] : requests)
                pending = pending || !terminal(item->state);
            if (!pending && !settlement_posted) {
                settlement_posted = true;
                completions.send([this] { settled.set(); });
            }
        }
    }

    kota::task<RuntimeResult> execute_blocking(
        const std::shared_ptr<RuntimeRequest::Shared>& item) {
        auto result = std::make_shared<BlockingResult>();
        auto exception = std::make_shared<std::string>();
        CancellationToken token(item->cancelled);
        auto queued = co_await kota::queue(
            [work = item->blocking, token, result, exception] {
                try {
                    *result = work(token);
                } catch (const std::exception& error) {
                    *exception = error.what();
                } catch (...) {
                    *exception = "blocking job threw a non-standard exception";
                }
            }, loop);
        RuntimeResult out;
        if (queued.has_error()) {
            out.state = RuntimeRequestState::failed;
            out.error = "could not run blocking job: " +
                        std::string(queued.error().message());
        } else if (item->cancelled->load(std::memory_order_acquire)) {
            out.state = RuntimeRequestState::cancelled_after_start;
            out.error = "running job cancelled; late result suppressed";
        } else if (!exception->empty()) {
            out.state = RuntimeRequestState::failed;
            out.error = *exception;
        } else {
            out.state = result->ok ? RuntimeRequestState::completed
                                   : RuntimeRequestState::failed;
            out.value = std::move(result->value);
            out.output = std::move(result->output);
            out.error = std::move(result->error);
        }
        co_return out;
    }

    kota::task<RuntimeResult> execute_process(
        const std::shared_ptr<RuntimeRequest::Shared>& item) {
        RuntimeResult out;
        const ProcessSpec& spec = *item->process;
        kota::process::options options;
        options.file = spec.file;
        options.args = spec.args;
        options.cwd = spec.cwd;
        options.env = inherited_environment(spec.environment);
#ifndef _WIN32
        options.creation.detached = true;
#endif
        options.streams = {
            kota::process::stdio::pipe(true, false),
            kota::process::stdio::pipe(false, true),
            kota::process::stdio::pipe(false, true),
        };
        auto spawned = kota::process::spawn(options, loop);
        if (!spawned) {
            out.state = RuntimeRequestState::failed;
            out.error = "process spawn failed: " +
                        std::string(spawned.error().message());
            co_return out;
        }
        out.pid = spawned->proc.pid();
        item->result.pid = out.pid;
        item->active_process = &spawned->proc;

        kota::event stdout_done;
        kota::event stderr_done;
        loop.schedule(capture_pipe(std::move(spawned->stdout_pipe),
                                   out.stdout_data, completions, stdout_done));
        loop.schedule(capture_pipe(std::move(spawned->stderr_pipe),
                                   out.stderr_data, completions, stderr_done));
        if (!spec.stdin_data.empty()) {
            auto write = co_await spawned->stdin_pipe.write(
                std::span<const char>(spec.stdin_data.data(), spec.stdin_data.size()));
            if (write.has_error()) terminate_owned_process(spawned->proc);
        }
        spawned->stdin_pipe = kota::pipe{};
        if (item->cancelled->load(std::memory_order_acquire))
            terminate_owned_process(spawned->proc);
        auto status = co_await spawned->proc.wait();
        out.reaped = status.has_value();
        item->active_process = nullptr;
        co_await stdout_done.wait();
        co_await stderr_done.wait();
        if (!status) {
            out.state = RuntimeRequestState::failed;
            out.error = "process wait failed: " +
                        std::string(status.error().message());
        } else {
            out.exit_code = static_cast<int>(status->status);
            out.term_signal = status->term_signal;
            if (item->cancelled->load(std::memory_order_acquire)) {
                out.state = RuntimeRequestState::cancelled_after_start;
                out.error = "running process cancelled and reaped";
            } else if (status->status != 0 || status->term_signal != 0) {
                out.state = RuntimeRequestState::failed;
                out.error = "child process failed";
            } else {
                out.state = RuntimeRequestState::completed;
            }
        }
        co_return out;
    }

    kota::task<RuntimeResult> execute_worker(
        const std::shared_ptr<RuntimeRequest::Shared>& item);

    kota::task<> execute(std::shared_ptr<RuntimeRequest::Shared> item) {
        RuntimeResult result;
        if (item->worker_pool)
            result = co_await execute_worker(item);
        else if (item->process)
            result = co_await execute_process(item);
        else
            result = co_await execute_blocking(item);
        finish(item, std::move(result));
        --active;
        if (item->lane == RuntimeLane::background) --active_background;
        dispatch();
    }
};

CoreRuntime::CoreRuntime(kota::event_loop& loop, RuntimeLimits limits)
    : m_impl(std::make_unique<Impl>(loop, limits)) {}

CoreRuntime::~CoreRuntime() {
    assert(!m_impl || (m_impl->stopping && m_impl->active == 0) ||
           (m_impl->requests.empty() && m_impl->worker_pools.empty()));
}

RuntimeRequest CoreRuntime::submit_blocking(BlockingWork work,
                                             RuntimeSubmitOptions options) {
    auto item = std::make_shared<RuntimeRequest::Shared>();
    item->id = ++m_impl->next_id;
    item->owner = m_impl.get();
    item->lane = options.lane;
    item->blocking = std::move(work);
    m_impl->requests.emplace(item->id, item);
    if (m_impl->stopping ||
        std::ranges::any_of(options.dependencies, [&](const auto& dep) {
            return !dep.m_shared || dep.m_shared->owner != m_impl.get();
        })) {
        RuntimeResult result;
        result.state = RuntimeRequestState::failed;
        result.error = m_impl->stopping ? "runtime is shutting down"
                                        : "invalid runtime dependency";
        m_impl->finish(item, std::move(result));
        return RuntimeRequest(item);
    }
    for (const auto& dep : options.dependencies)
        item->dependencies.push_back(dep.m_shared);
    std::weak_ptr<RuntimeRequest::Shared> weak = item;
    item->cancel_callback = [impl = m_impl.get(), weak] {
        if (auto locked = weak.lock()) impl->cancel(locked);
    };
    (item->lane == RuntimeLane::foreground ? m_impl->foreground
                                           : m_impl->background).push_back(item);
    m_impl->dispatch();
    return RuntimeRequest(item);
}

RuntimeRequest CoreRuntime::submit_process(ProcessSpec process,
                                            RuntimeSubmitOptions options) {
    auto item = std::make_shared<RuntimeRequest::Shared>();
    item->id = ++m_impl->next_id;
    item->owner = m_impl.get();
    item->lane = options.lane;
    item->process = std::move(process);
    m_impl->requests.emplace(item->id, item);
    if (m_impl->stopping ||
        std::ranges::any_of(options.dependencies, [&](const auto& dep) {
            return !dep.m_shared || dep.m_shared->owner != m_impl.get();
        })) {
        RuntimeResult result;
        result.state = RuntimeRequestState::failed;
        result.error = m_impl->stopping ? "runtime is shutting down"
                                        : "invalid runtime dependency";
        m_impl->finish(item, std::move(result));
        return RuntimeRequest(item);
    }
    for (const auto& dep : options.dependencies)
        item->dependencies.push_back(dep.m_shared);
    std::weak_ptr<RuntimeRequest::Shared> weak = item;
    item->cancel_callback = [impl = m_impl.get(), weak] {
        if (auto locked = weak.lock()) impl->cancel(locked);
    };
    (item->lane == RuntimeLane::foreground ? m_impl->foreground
                                           : m_impl->background).push_back(item);
    m_impl->dispatch();
    return RuntimeRequest(item);
}

class RuntimeWorkerPool {
public:
    struct Worker {
        kota::process process;
        kota::pipe input;
        kota::pipe output;
        kota::pipe errors;
        std::string read_buffer;
        std::string stderr_data;
        kota::event stderr_done;
        bool alive = false;
        bool busy = false;
        std::uint64_t generation = 0;
    };

    kota::event_loop& loop;
    WorkerPoolOptions options;
    std::vector<std::unique_ptr<Worker>> foreground;
    std::vector<std::unique_ptr<Worker>> background;
    std::deque<std::chrono::steady_clock::time_point> crashes;
    bool stopping = false;
    bool settled = false;
    kota::relay completions;

    RuntimeWorkerPool(kota::event_loop& loop, WorkerPoolOptions options,
                      RuntimeLimits limits)
        : loop(loop), options(std::move(options)),
          completions(loop.create_relay()) {
        for (std::size_t i = 0; i < limits.max_concurrency; ++i)
            foreground.push_back(std::make_unique<Worker>());
        for (std::size_t i = 0; i < limits.background_concurrency; ++i)
            background.push_back(std::make_unique<Worker>());
    }

    bool crash_limited() {
        auto now = std::chrono::steady_clock::now();
        while (!crashes.empty() && now - crashes.front() > options.crash_window)
            crashes.pop_front();
        return crashes.size() >= options.crash_limit;
    }

    kota::task<> capture_stderr(Worker& worker) {
        for (;;) {
            auto chunk = co_await worker.errors.read();
            if (chunk.has_error()) break;
            worker.stderr_data += chunk.value();
        }
        completions.send([&worker] { worker.stderr_done.set(); });
    }

    static void interrupt(Worker& worker) {
        worker.input = kota::pipe{};
        worker.output.stop();
        worker.errors.stop();
        terminate_owned_process(worker.process);
    }

    bool start(Worker& worker) {
        if (stopping || crash_limited()) return false;
        kota::process::options spawn;
        spawn.file = options.worker.file;
        spawn.args = options.worker.args;
        spawn.cwd = options.worker.cwd;
        spawn.env = inherited_environment(options.worker.environment);
#ifndef _WIN32
        spawn.creation.detached = true;
#endif
        spawn.streams = {
            kota::process::stdio::pipe(true, false),
            kota::process::stdio::pipe(false, true),
            kota::process::stdio::pipe(false, true),
        };
        auto child = kota::process::spawn(spawn, loop);
        if (!child) return false;
        worker.process = std::move(child->proc);
        worker.input = std::move(child->stdin_pipe);
        worker.output = std::move(child->stdout_pipe);
        worker.errors = std::move(child->stderr_pipe);
        worker.read_buffer.clear();
        worker.stderr_data.clear();
        worker.alive = true;
        ++worker.generation;
        worker.stderr_done.reset();
        loop.schedule(capture_stderr(worker));
        return true;
    }

    kota::task<WorkerReply> call_one(
        Worker& worker, std::string payload,
        const std::function<void(int, std::function<void()>)>& set_cancel) {
        WorkerReply reply;
        if (stopping) {
            reply.error = "worker pool is shutting down";
            co_return reply;
        }
        if (!worker.alive && !start(worker)) {
            reply.error = crash_limited() ? "worker crash limit reached"
                                          : "worker spawn failed";
            co_return reply;
        }
        worker.busy = true;
        struct Idle {
            Worker& worker;
            ~Idle() { worker.busy = false; }
        } idle{worker};
        reply.pid = worker.process.pid();
        const auto generation = worker.generation;
        reply.generation = generation;
        set_cancel(reply.pid, [&worker, generation] {
            if (worker.busy && worker.generation == generation)
                interrupt(worker);
        });
        auto frame = frame_payload(payload);
        auto written = co_await std::move(worker.input.write(
            std::span<const char>(frame.data(), frame.size()))).catch_cancel();
        const bool write_cancelled = written.is_cancelled();
        if (!write_cancelled && !written.has_error()) {
            for (;;) {
                if (auto response = take_frame(worker.read_buffer)) {
                    if (worker.generation == generation) {
                        reply.ok = true;
                        reply.payload = std::move(*response);
                    }
                    co_return reply;
                }
                auto chunk = co_await worker.output.read();
                if (chunk.has_error()) break;
                worker.read_buffer += chunk.value();
            }
        }
        worker.alive = false;
        worker.input = kota::pipe{};
        auto status = co_await worker.process.wait();
        reply.reaped = status.has_value();
        co_await worker.stderr_done.wait();
        reply.stderr_data = worker.stderr_data;
        if (write_cancelled)
            reply.error = "worker pipe write cancelled";
        else if (written.has_error())
            reply.error = "worker pipe write failed";
        else
            reply.error = "worker exited before replying";
        if (!write_cancelled)
            crashes.push_back(std::chrono::steady_clock::now());
        set_cancel(-1, {});
        co_return reply;
    }

    kota::task<WorkerReply> call(
        std::string payload, RuntimeLane lane,
        const std::function<void(int, std::function<void()>)>& set_cancel) {
        auto& workers = lane == RuntimeLane::foreground ? foreground : background;
        auto found = std::ranges::find_if(workers, [](const auto& worker) {
            return !worker->busy;
        });
        WorkerReply reply;
        if (found == workers.end()) {
            reply.error = "runtime worker lane capacity invariant failed";
            co_return reply;
        }
        co_return co_await call_one(**found, std::move(payload), set_cancel);
    }

    kota::task<> stop_one(Worker& worker) {
        if (!worker.alive) co_return;
        assert(!worker.busy && "worker teardown started while a call still owns it");
        worker.busy = true;
        struct Idle {
            Worker& worker;
            ~Idle() { worker.busy = false; }
        } idle{worker};
        worker.alive = false;
        interrupt(worker);
        co_await worker.process.wait();
        co_await worker.stderr_done.wait();
    }

    kota::task<bool> cancel_and_reap(const WorkerReply& reply) {
        auto matches = [&](const auto& worker) {
            return worker->alive && worker->process.pid() == reply.pid &&
                   worker->generation == reply.generation;
        };
        auto foreground_match = std::ranges::find_if(foreground, matches);
        if (foreground_match != foreground.end()) {
            co_await stop_one(**foreground_match);
            co_return true;
        }
        auto background_match = std::ranges::find_if(background, matches);
        if (background_match != background.end()) {
            co_await stop_one(**background_match);
            co_return true;
        }
        co_return reply.reaped;
    }

    void begin_shutdown() {
        if (stopping) return;
        stopping = true;
        auto stop_io = [](Worker& worker) {
            if (!worker.alive && !worker.busy) return;
            interrupt(worker);
        };
        for (auto& worker : foreground) stop_io(*worker);
        for (auto& worker : background) stop_io(*worker);
    }

    kota::task<> shutdown() {
        if (settled) co_return;
        begin_shutdown();
        for (auto& worker : foreground) co_await stop_one(*worker);
        for (auto& worker : background) co_await stop_one(*worker);
        settled = true;
    }
};

kota::task<RuntimeResult> CoreRuntime::Impl::execute_worker(
    const std::shared_ptr<RuntimeRequest::Shared>& item) {
    auto set_cancel = [item](int pid, std::function<void()> cancel) {
        if (pid > 0) item->result.pid = pid;
        item->active_cancel = std::move(cancel);
    };
    WorkerReply reply = co_await item->worker_pool->call(
        std::move(item->worker_payload), item->lane, set_cancel);
    RuntimeResult out;
    out.pid = reply.pid;
    out.reaped = reply.reaped;
    out.stderr_data = std::move(reply.stderr_data);
    if (item->cancelled->load(std::memory_order_acquire)) {
        out.reaped = co_await item->worker_pool->cancel_and_reap(reply);
        item->active_cancel = {};
        out.state = RuntimeRequestState::cancelled_after_start;
        out.error = "running worker request cancelled and reaped";
    } else if (!reply.ok) {
        item->active_cancel = {};
        out.state = RuntimeRequestState::failed;
        out.error = std::move(reply.error);
    } else {
        item->active_cancel = {};
        out.state = RuntimeRequestState::completed;
        out.value = std::move(reply);
    }
    co_return out;
}

WorkerPoolId CoreRuntime::create_worker_pool(WorkerPoolOptions options) {
    assert(!m_impl->stopping && "cannot create a worker pool during shutdown");
    m_impl->worker_pools.push_back(std::make_shared<RuntimeWorkerPool>(
        m_impl->loop, std::move(options), m_impl->limits));
    return WorkerPoolId(m_impl->runtime_id, m_impl->worker_pools.size());
}

RuntimeRequest CoreRuntime::submit_worker(WorkerPoolId pool,
                                           std::string payload,
                                           RuntimeSubmitOptions options) {
    auto item = std::make_shared<RuntimeRequest::Shared>();
    item->id = ++m_impl->next_id;
    item->owner = m_impl.get();
    item->lane = options.lane;
    m_impl->requests.emplace(item->id, item);
    if (m_impl->stopping || pool.runtime != m_impl->runtime_id ||
        pool.value == 0 ||
        pool.value > m_impl->worker_pools.size() ||
        std::ranges::any_of(options.dependencies, [&](const auto& dep) {
            return !dep.m_shared || dep.m_shared->owner != m_impl.get();
        })) {
        RuntimeResult result;
        result.state = RuntimeRequestState::failed;
        if (m_impl->stopping)
            result.error = "runtime is shutting down";
        else if (pool.runtime != m_impl->runtime_id || pool.value == 0 ||
                 pool.value > m_impl->worker_pools.size())
            result.error = "unknown runtime worker pool";
        else
            result.error = "invalid runtime dependency";
        m_impl->finish(item, std::move(result));
        return RuntimeRequest(item);
    }
    item->worker_pool = m_impl->worker_pools[pool.value - 1];
    item->worker_payload = std::move(payload);
    for (const auto& dep : options.dependencies)
        item->dependencies.push_back(dep.m_shared);
    std::weak_ptr<RuntimeRequest::Shared> weak = item;
    item->cancel_callback = [impl = m_impl.get(), weak] {
        if (auto locked = weak.lock()) impl->cancel(locked);
    };
    (item->lane == RuntimeLane::foreground ? m_impl->foreground
                                           : m_impl->background).push_back(item);
    m_impl->dispatch();
    return RuntimeRequest(item);
}

kota::task<> CoreRuntime::shutdown() {
    if (!m_impl->stopping) {
        m_impl->stopping = true;
        for (auto& pool : m_impl->worker_pools) pool->begin_shutdown();
        std::vector<std::shared_ptr<RuntimeRequest::Shared>> pending;
        pending.reserve(m_impl->requests.size());
        for (const auto& [id, item] : m_impl->requests)
            pending.push_back(item);
        for (const auto& item : pending) m_impl->cancel(item);
        m_impl->dispatch();
    }
    co_await m_impl->settled.wait();
    for (auto& pool : m_impl->worker_pools) co_await pool->shutdown();
}

bool CoreRuntime::shutting_down() const noexcept { return m_impl->stopping; }

}  // namespace bake
