#pragma once

// ---------------------------------------------------------------------------
// core/thread_pool.h – modern C++20 fixed-size worker pool built on
// std::jthread, std::stop_token, std::condition_variable_any, and a move-only
// task wrapper supporting std::packaged_task / move-only callables.
//
// Destruction guarantees:
// - ~ThreadPool() calls shutdown(), stopping new submissions, draining all
//   already-queued tasks cleanly, and joining every worker before tearing down
//   synchronization primitives.
// - cancel_pending() discards unstarted queued tasks, signals std::stop_token
//   on every worker for cooperative cancellation, and joins cleanly.
// ---------------------------------------------------------------------------

#include "core/scoped_handle.h"

#include <algorithm>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace core {

class ThreadPool {
public:
    [[nodiscard]] static std::size_t default_thread_count() noexcept {
        const unsigned int hw = std::thread::hardware_concurrency();
        return (hw > 0U) ? static_cast<std::size_t>(hw) : std::size_t{1};
    }

    explicit ThreadPool(std::size_t thread_count = default_thread_count()) {
        const std::size_t count =
            (thread_count > 0) ? thread_count : default_thread_count();
        workers_.reserve(count);
        try {
            for (std::size_t i = 0; i < count; ++i) {
                workers_.emplace_back([this](std::stop_token st) {
                    worker_loop(std::move(st));
                });
            }
        } catch (...) {
            cancel_pending();
            throw;
        }
    }

    ~ThreadPool() noexcept { shutdown(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    [[nodiscard]] std::size_t worker_count() const noexcept {
        return workers_.size();
    }

    // Submits any move-only or copyable callable + arguments, returning a
    // std::future carrying the result or exception. Throws std::runtime_error
    // if shutdown() or cancel_pending() has already been called.
    template <typename F, typename... Args>
        requires std::invocable<std::decay_t<F>, std::decay_t<Args>...>
    [[nodiscard]] auto submit(F&& func, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        using Result = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

        auto bound = [fn = std::forward<F>(func),
                      arg_tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Result {
            return std::apply(std::move(fn), std::move(arg_tuple));
        };

        std::packaged_task<Result()> packaged(std::move(bound));
        std::future<Result> fut = packaged.get_future();
        enqueue(MoveOnlyTask{[task = std::move(packaged)](std::stop_token) mutable {
            task();
        }});
        return fut;
    }

    // Submits a cooperative stop-aware callable accepting a std::stop_token.
    template <typename F>
        requires std::invocable<std::decay_t<F>, std::stop_token>
    [[nodiscard]] auto submit_stoppable(F&& func)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::stop_token>> {
        using Result = std::invoke_result_t<std::decay_t<F>, std::stop_token>;

        std::packaged_task<Result(std::stop_token)> packaged(std::forward<F>(func));
        std::future<Result> fut = packaged.get_future();
        enqueue(MoveOnlyTask{[task = std::move(packaged)](std::stop_token st) mutable {
            task(std::move(st));
        }});
        return fut;
    }

    // Blocks until the queue is empty and no tasks are currently executing.
    void wait_idle() {
        std::unique_lock lock(mutex_);
        idle_cv_.wait(lock, [this]() noexcept {
            return tasks_.empty() && active_tasks_ == 0;
        });
    }

    // Stops accepting new tasks, drains all already-queued tasks, and joins workers.
    void shutdown() noexcept {
        {
            std::scoped_lock lock(mutex_);
            accepting_ = false;
        }
        work_cv_.notify_all();

        // Join explicitly before clearing so jthread destructors do not trigger
        // request_stop() prior to draining already-queued tasks.
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        idle_cv_.notify_all();
    }

    // Stops accepting new tasks, discards unstarted queued tasks, signals
    // cooperative stop to running workers, and joins all workers.
    void cancel_pending() noexcept {
        {
            std::scoped_lock lock(mutex_);
            accepting_ = false;
            cancel_requested_ = true;
            tasks_.clear();
        }
        for (auto& worker : workers_) {
            worker.request_stop();
        }
        work_cv_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        idle_cv_.notify_all();
    }

    // Partitions [0, count) across pool workers + the calling thread.
    // Waits for all chunks to complete before rethrowing the first exception
    // so caller stack references never dangle.
    template <typename Fn>
        requires std::invocable<Fn&, std::size_t>
    void parallel_for(std::size_t count, Fn&& fn) {
        if (count == 0) {
            return;
        }
        if (count == 1 || worker_count() <= 1) {
            for (std::size_t i = 0; i < count; ++i) {
                std::invoke(fn, i);
            }
            return;
        }

        const std::size_t num_chunks = std::min(count, worker_count());
        const std::size_t base_chunk = count / num_chunks;
        const std::size_t remainder = count % num_chunks;

        std::vector<std::future<void>> futures;
        futures.reserve(num_chunks - 1);

        std::exception_ptr first_exception;
        std::size_t begin = 0;

        try {
            for (std::size_t chunk = 0; chunk < num_chunks - 1; ++chunk) {
                const std::size_t len = base_chunk + (chunk < remainder ? 1 : 0);
                const std::size_t end = begin + len;
                futures.push_back(submit([&fn, begin, end]() {
                    for (std::size_t idx = begin; idx < end; ++idx) {
                        std::invoke(fn, idx);
                    }
                }));
                begin = end;
            }

            // Execute the final chunk inline on the calling thread.
            for (std::size_t idx = begin; idx < count; ++idx) {
                std::invoke(fn, idx);
            }
        } catch (...) {
            if (!first_exception) {
                first_exception = std::current_exception();
            }
        }

        // Always drain every submitted chunk before propagating any exception.
        for (auto& fut : futures) {
            try {
                fut.get();
            } catch (...) {
                if (!first_exception) {
                    first_exception = std::current_exception();
                }
            }
        }

        if (first_exception) {
            std::rethrow_exception(first_exception);
        }
    }

private:
    // Move-only type-erased task wrapper accepting std::stop_token.
    class MoveOnlyTask {
    public:
        MoveOnlyTask() noexcept = default;

        template <typename Fn>
            requires(!std::same_as<std::decay_t<Fn>, MoveOnlyTask>) &&
                    std::invocable<std::decay_t<Fn>&, std::stop_token>
        explicit MoveOnlyTask(Fn&& fn)
            : impl_(std::make_unique<Model<std::decay_t<Fn>>>(std::forward<Fn>(fn))) {}

        MoveOnlyTask(MoveOnlyTask&&) noexcept = default;
        MoveOnlyTask& operator=(MoveOnlyTask&&) noexcept = default;
        MoveOnlyTask(const MoveOnlyTask&) = delete;
        MoveOnlyTask& operator=(const MoveOnlyTask&) = delete;

        void operator()(std::stop_token st) {
            if (impl_) {
                impl_->invoke(std::move(st));
            }
        }

    private:
        struct Concept {
            virtual ~Concept() = default;
            virtual void invoke(std::stop_token st) = 0;
        };

        template <typename Fn>
        struct Model final : Concept {
            Fn fn;
            explicit Model(Fn&& f) : fn(std::move(f)) {}
            void invoke(std::stop_token st) override { fn(std::move(st)); }
        };

        std::unique_ptr<Concept> impl_;
    };

    void enqueue(MoveOnlyTask task) {
        {
            std::scoped_lock lock(mutex_);
            if (!accepting_) {
                throw std::runtime_error("ThreadPool::submit called after shutdown");
            }
            tasks_.push_back(std::move(task));
        }
        work_cv_.notify_one();
    }

    void worker_loop(std::stop_token st) {
        for (;;) {
            MoveOnlyTask task;
            {
                std::unique_lock lock(mutex_);
                work_cv_.wait(lock, st, [this]() noexcept {
                    return !tasks_.empty() || !accepting_ || cancel_requested_;
                });
                if (cancel_requested_ || st.stop_requested()) {
                    return;
                }
                if (tasks_.empty()) {
                    if (!accepting_) {
                        return;
                    }
                    continue;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
                ++active_tasks_;
            }

            const auto finish_guard = make_scope_guard([this]() noexcept {
                std::scoped_lock lock(mutex_);
                --active_tasks_;
                if (active_tasks_ == 0 && tasks_.empty()) {
                    idle_cv_.notify_all();
                }
            });

            task(st);
        }
    }

    // Declaration order is load-bearing: workers_ MUST be last so all threads
    // join before mutex_, condition variables, or tasks_ are destroyed.
    mutable std::mutex mutex_;
    std::condition_variable_any work_cv_;
    std::condition_variable idle_cv_;
    std::deque<MoveOnlyTask> tasks_;
    std::size_t active_tasks_ = 0;
    bool accepting_ = true;
    bool cancel_requested_ = false;
    std::vector<std::jthread> workers_;
};

}  // namespace core
