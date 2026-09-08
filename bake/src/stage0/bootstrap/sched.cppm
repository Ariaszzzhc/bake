export module bake.stage0.sched;

import std;

// ============================================================
// bake.stage0.sched — bounded task-graph executor
//
// The scheduling core for parallel workloads over a static DAG;
// the build executor is its consumer. Policy — what a
// node does, when it is considered dirty, what progress to print
// — stays with the caller; this module owns only the mechanics:
//
//   - ready-queue worker pool over per-node dependency counts
//   - inactive nodes (up-to-date / cancelled) are skipped and
//     their edges ignored
//   - fail-fast: the first failed node stops scheduling; nodes
//     already in flight run to completion
//
// Semantics are deliberately identical to the historical inline
// executor in engine.cppm (execute_graph) — that executor is now
// a thin policy wrapper over this module.
// ============================================================

namespace bake {

export struct TaskGraphSpec {
    std::size_t node_count = 0;
    // Per-node dependency indices into the same node space.
    std::vector<std::vector<std::size_t>> deps;
    // Nodes to run this round; inactive nodes are skipped and
    // dependencies on them are ignored.
    std::vector<bool> active;
    // Worker count; <= 0 → hardware concurrency (fallback 4).
    int jobs = 0;
};

// Run every active node whose active dependencies have completed.
// node(i) executes node i and reports success. Returns the number
// of failed nodes (0 = all succeeded).
export int run_task_graph(const TaskGraphSpec& spec,
                          const std::function<bool(std::size_t)>& node) {
    const std::size_t n = spec.node_count;
    if (n == 0) return 0;

    int jobs = spec.jobs;
    if (jobs <= 0) {
        jobs = static_cast<int>(std::thread::hardware_concurrency());
        if (jobs == 0) jobs = 4;
    }

    int pending = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (i < spec.active.size() && spec.active[i]) ++pending;
    if (pending == 0) return 0;

    // Reverse edges for O(1) dependent lookup.
    std::vector<std::vector<std::size_t>> dependents(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (auto dep : (i < spec.deps.size() ? spec.deps[i]
                                              : std::vector<std::size_t>{})) {
            if (dep < n)
                dependents[dep].push_back(i);
        }
    }

    // Unresolved active-dependency counts per node.
    std::vector<int> pending_deps(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (!(i < spec.active.size() && spec.active[i])) continue;
        for (auto dep : (i < spec.deps.size() ? spec.deps[i]
                                              : std::vector<std::size_t>{})) {
            if (dep < n && dep < spec.active.size() && spec.active[dep])
                ++pending_deps[i];
        }
    }

    std::deque<std::size_t> ready_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::atomic<int> completed{0};
    std::atomic<int> failed{0};
    std::atomic<bool> stop{false};

    for (std::size_t i = 0; i < n; ++i) {
        if (i < spec.active.size() && spec.active[i] && pending_deps[i] == 0)
            ready_queue.push_back(i);
    }

    auto worker = [&]() {
        while (true) {
            std::size_t idx;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [&] {
                    return stop.load() || !ready_queue.empty();
                });
                if (stop.load()) return;
                idx = ready_queue.front();
                ready_queue.pop_front();
            }

            bool ok = node(idx);
            if (!ok) failed.fetch_add(1);

            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (ok) {
                    completed.fetch_add(1);
                    for (auto dep_idx : dependents[idx]) {
                        if (dep_idx < spec.active.size() &&
                            spec.active[dep_idx] &&
                            --pending_deps[dep_idx] == 0) {
                            ready_queue.push_back(dep_idx);
                        }
                    }
                }
                if (failed.load() > 0 || completed.load() >= pending) {
                    stop.store(true);
                }
                queue_cv.notify_all();
            }
        }
    };


    int n_workers = std::min(jobs, pending);
    std::vector<std::thread> pool;
    for (int t = 0; t < n_workers; ++t)
        pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    return failed.load();
}

} // namespace bake
