#include "core/profiler.h"
#include "core/thread_pool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// Deterministic manual clock satisfying core::ProfileClock so profiler stats
// are tested to exact nanosecond precision with zero sleeps or wall-clock flake.
struct ManualClock {
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<ManualClock, duration>;

    static inline std::atomic<std::int64_t> ticks_ns{0};
    static inline std::atomic<std::uint64_t> now_calls{0};

    static void reset(std::int64_t initial_ns = 0) noexcept {
        ticks_ns.store(initial_ns, std::memory_order_relaxed);
        now_calls.store(0, std::memory_order_relaxed);
    }

    static void advance(std::int64_t delta_ns) noexcept {
        ticks_ns.fetch_add(delta_ns, std::memory_order_relaxed);
    }

    [[nodiscard]] static time_point now() noexcept {
        now_calls.fetch_add(1, std::memory_order_relaxed);
        return time_point{duration{ticks_ns.load(std::memory_order_relaxed)}};
    }
};

static_assert(core::ProfileClock<ManualClock>);
static_assert(core::ProfileClock<std::chrono::steady_clock>);

}  // namespace

// ---------------------------------------------------------------------------
// core::Profiler & core::BasicScopedProfile contract tests
// ---------------------------------------------------------------------------

TEST(ProfilerTest, DisabledByDefaultAndZeroClockCallsWhenDisabled) {
    ManualClock::reset(1'000);
    core::Profiler profiler;

    EXPECT_FALSE(profiler.is_enabled());
    EXPECT_TRUE(profiler.snapshot().empty());
    EXPECT_EQ(profiler.stage_stats("decode"), std::nullopt);
    EXPECT_EQ(profiler.format_json(), "{\"stages\":[]}");

    {
        core::BasicScopedProfile<ManualClock> scope("decode", profiler);
        EXPECT_FALSE(scope.is_active());
        ManualClock::advance(500'000);
    }

    // Disabled profiler must not touch Clock::now() or record direct samples.
    EXPECT_EQ(ManualClock::now_calls.load(), 0u);
    profiler.record("direct", std::chrono::nanoseconds{42'000});
    EXPECT_TRUE(profiler.snapshot().empty());
    EXPECT_EQ(profiler.stage_stats("decode"), std::nullopt);
    EXPECT_EQ(profiler.stage_stats("direct"), std::nullopt);
}

TEST(ProfilerTest, ExactStatsAccumulationOrderingClampAndReset) {
    ManualClock::reset(10'000);
    core::Profiler profiler(true);
    ASSERT_TRUE(profiler.is_enabled());

    // Hand-calculated samples for "score": 250 ns, 50 ns, 100 ns ->
    // calls=3, total=400, min=50, max=250, avg=400/3.
    {
        core::BasicScopedProfile<ManualClock> s("score", profiler);
        EXPECT_TRUE(s.is_active());
        ManualClock::advance(250);
    }
    {
        core::BasicScopedProfile<ManualClock> s("score", profiler);
        ManualClock::advance(50);
        s.stop();
        EXPECT_FALSE(s.is_active());
        ManualClock::advance(99'999);  // post-stop advance must be ignored
        s.stop();                      // idempotent second stop
    }
    {
        core::BasicScopedProfile<ManualClock> s("score", profiler);
        ManualClock::advance(100);
    }

    // Samples for "decode": 1,000,000 ns (1.0 ms) -> must sort before "score".
    profiler.record("decode", std::chrono::nanoseconds{1'000'000});

    // Tie-break check: "alpha" and "beta" both have 400 ns total -> "alpha" < "beta".
    profiler.record("beta", std::chrono::nanoseconds{400});
    profiler.record("alpha", std::chrono::nanoseconds{400});

    // Boundary cases: negative duration clamps to 0; empty stage name is ignored.
    profiler.record("clamped", std::chrono::nanoseconds{-500});
    profiler.record("", std::chrono::nanoseconds{999});

    const auto snap = profiler.snapshot();
    ASSERT_EQ(snap.size(), 5u);

    EXPECT_EQ(snap[0].name, "decode");
    EXPECT_EQ(snap[0].call_count, 1u);
    EXPECT_EQ(snap[0].total_ns, 1'000'000u);
    EXPECT_DOUBLE_EQ(snap[0].total_ms(), 1.0);
    EXPECT_DOUBLE_EQ(snap[0].avg_ms(), 1.0);
    EXPECT_DOUBLE_EQ(snap[0].min_ms(), 1.0);
    EXPECT_DOUBLE_EQ(snap[0].max_ms(), 1.0);

    // Equal total_ns (400): alphabetical order alpha, beta, score.
    EXPECT_EQ(snap[1].name, "alpha");
    EXPECT_EQ(snap[2].name, "beta");
    EXPECT_EQ(snap[3].name, "score");
    EXPECT_EQ(snap[3].call_count, 3u);
    EXPECT_EQ(snap[3].total_ns, 400u);
    EXPECT_EQ(snap[3].min_ns, 50u);
    EXPECT_EQ(snap[3].max_ns, 250u);
    EXPECT_NEAR(snap[3].avg_ns(), 400.0 / 3.0, 1e-9);

    EXPECT_EQ(snap[4].name, "clamped");
    EXPECT_EQ(snap[4].call_count, 1u);
    EXPECT_EQ(snap[4].total_ns, 0u);
    EXPECT_EQ(snap[4].min_ns, 0u);
    EXPECT_EQ(snap[4].max_ns, 0u);

    const auto looked_up = profiler.stage_stats("score");
    ASSERT_TRUE(looked_up.has_value());
    EXPECT_EQ(*looked_up, snap[3]);
    EXPECT_EQ(profiler.stage_stats(""), std::nullopt);
    EXPECT_EQ(profiler.stage_stats("missing"), std::nullopt);

    // Reset clears accumulated stats without disabling the profiler.
    profiler.reset();
    EXPECT_TRUE(profiler.is_enabled());
    EXPECT_TRUE(profiler.snapshot().empty());

    // Valid -> reset -> valid seam: new samples start fresh (min/max not poisoned by prior runs).
    profiler.record("score", std::chrono::nanoseconds{777});
    const auto after_reset = profiler.stage_stats("score");
    ASSERT_TRUE(after_reset.has_value());
    EXPECT_EQ(after_reset->call_count, 1u);
    EXPECT_EQ(after_reset->total_ns, 777u);
    EXPECT_EQ(after_reset->min_ns, 777u);
    EXPECT_EQ(after_reset->max_ns, 777u);
}

TEST(ProfilerTest, ScopedEnableRestoresPreviousStateAndFormatsSummaryAndEscapedJson) {
    core::Profiler profiler(false);
    ASSERT_FALSE(profiler.is_enabled());

    {
        const core::ScopedProfilerEnable guard(profiler, true);
        EXPECT_TRUE(profiler.is_enabled());

        {
            const core::ScopedProfilerEnable inner_off(profiler, false);
            EXPECT_FALSE(profiler.is_enabled());
            profiler.record("ignored_inner", std::chrono::nanoseconds{123});
        }

        EXPECT_TRUE(profiler.is_enabled());
        // Stage name containing JSON metacharacters (quote, backslash, newline, tab, control 0x01).
        const std::string tricky_name = std::string("a\"b\\c\nd\te") + static_cast<char>(0x01);
        profiler.record(tricky_name, std::chrono::nanoseconds{2'500'000});
    }

    EXPECT_FALSE(profiler.is_enabled());
    EXPECT_EQ(profiler.stage_stats("ignored_inner"), std::nullopt);

    const std::string json = profiler.format_json();
    EXPECT_NE(json.find("\"stages\":["), std::string::npos);
    EXPECT_NE(json.find("\"name\":\"a\\\"b\\\\c\\nd\\te\\u0001\""), std::string::npos);
    EXPECT_NE(json.find("\"calls\":1"), std::string::npos);
    EXPECT_NE(json.find("\"total_ns\":2500000"), std::string::npos);
    EXPECT_NE(json.find("\"min_ns\":2500000"), std::string::npos);
    EXPECT_NE(json.find("\"max_ns\":2500000"), std::string::npos);

    const std::string summary = profiler.format_summary();
    EXPECT_NE(summary.find("2.500"), std::string::npos);
    EXPECT_NE(summary.find("Calls"), std::string::npos);
}

TEST(ProfilerTest, ConcurrentJThreadRecordingPreservesExactCountsTotalsAndBounds) {
    constexpr std::size_t kThreadCount = 8;
    constexpr std::size_t kSamplesPerThread = 2'000;
    constexpr std::uint64_t kTotalExpectedCalls = kThreadCount * kSamplesPerThread;

    core::Profiler profiler(true);
    std::latch start_gate(static_cast<std::ptrdiff_t>(kThreadCount));

    std::vector<std::jthread> threads;
    threads.reserve(kThreadCount);
    for (std::size_t tid = 0; tid < kThreadCount; ++tid) {
        threads.emplace_back([&profiler, &start_gate, tid]() {
            start_gate.arrive_and_wait();
            const std::string per_thread_stage = "worker_" + std::to_string(tid);
            for (std::size_t i = 0; i < kSamplesPerThread; ++i) {
                // Deterministic duration in [10, 10 + (kThreadCount-1) + (kSamplesPerThread-1)] ns.
                const auto ns = static_cast<std::int64_t>(10 + tid + i);
                profiler.record("shared_hot_stage", std::chrono::nanoseconds{ns});
                profiler.record(per_thread_stage, std::chrono::nanoseconds{ns});
            }
        });
    }
    threads.clear();  // joins all jthreads

    // Exact arithmetic series sum over tid in [0, 8), i in [0, 2000):
    // sum_{tid,i}(10 + tid + i) = 8 * (2000*10 + 2000*tid + 2000*1999/2)
    std::uint64_t expected_shared_total = 0;
    for (std::size_t tid = 0; tid < kThreadCount; ++tid) {
        for (std::size_t i = 0; i < kSamplesPerThread; ++i) {
            expected_shared_total += static_cast<std::uint64_t>(10 + tid + i);
        }
    }

    const auto snap = profiler.snapshot();
    ASSERT_EQ(snap.size(), kThreadCount + 1);

    const auto shared = profiler.stage_stats("shared_hot_stage");
    ASSERT_TRUE(shared.has_value());
    EXPECT_EQ(shared->call_count, kTotalExpectedCalls);
    EXPECT_EQ(shared->total_ns, expected_shared_total);
    EXPECT_EQ(shared->min_ns, 10u);
    EXPECT_EQ(shared->max_ns, static_cast<std::uint64_t>(10 + (kThreadCount - 1) + (kSamplesPerThread - 1)));

    for (std::size_t tid = 0; tid < kThreadCount; ++tid) {
        const auto s = profiler.stage_stats("worker_" + std::to_string(tid));
        ASSERT_TRUE(s.has_value());
        EXPECT_EQ(s->call_count, kSamplesPerThread);
        EXPECT_EQ(s->min_ns, static_cast<std::uint64_t>(10 + tid));
        EXPECT_EQ(s->max_ns, static_cast<std::uint64_t>(10 + tid + kSamplesPerThread - 1));
    }
}

// ---------------------------------------------------------------------------
// core::ThreadPool contract tests
// ---------------------------------------------------------------------------

TEST(ThreadPoolTest, ExecutesTasksMoveOnlyPayloadsAndPropagatesExceptions) {
    core::ThreadPool pool(4);
    EXPECT_EQ(pool.worker_count(), 4u);

    // Move-only callable + move-only argument -> exact computed result.
    auto payload = std::make_unique<int>(21);
    auto f_move = pool.submit(
        [captured = std::make_unique<int>(2)](std::unique_ptr<int> arg) {
            return (*captured) * (*arg);
        },
        std::move(payload));
    EXPECT_EQ(f_move.get(), 42);

    // Exception in one task must propagate via its future without killing workers.
    auto f_throw = pool.submit([]() -> int {
        throw std::invalid_argument("synthetic task failure");
    });
    EXPECT_THROW((void)f_throw.get(), std::invalid_argument);

    // Valid -> failing -> valid seam: pool remains healthy after task exception.
    constexpr std::size_t kBatchSize = 64;
    std::vector<std::future<std::size_t>> futures;
    futures.reserve(kBatchSize);
    for (std::size_t i = 0; i < kBatchSize; ++i) {
        futures.push_back(pool.submit([i]() noexcept { return i * i; }));
    }

    ASSERT_EQ(futures.size(), kBatchSize);
    for (std::size_t i = 0; i < kBatchSize; ++i) {
        EXPECT_EQ(futures[i].get(), i * i);
    }

    pool.wait_idle();
}

TEST(ThreadPoolTest, ZeroThreadsDefaultsToHardwareConcurrencyAndDestructorDrainsQueuedTasks) {
    EXPECT_GE(core::ThreadPool::default_thread_count(), 1u);

    constexpr int kQueuedTasks = 200;
    std::atomic<int> completed{0};
    {
        core::ThreadPool pool(0);
        EXPECT_EQ(pool.worker_count(), core::ThreadPool::default_thread_count());
        for (int i = 0; i < kQueuedTasks; ++i) {
            (void)pool.submit([&completed]() noexcept {
                completed.fetch_add(1, std::memory_order_relaxed);
            });
        }
    }  // Destructor must drain every already-submitted task before returning.

    EXPECT_EQ(completed.load(), kQueuedTasks);
}

TEST(ThreadPoolTest, ParallelForCoversBoundaryCountsAndPropagatesExceptionsSafely) {
    core::ThreadPool pool(4);

    // Boundary counts: 0, 1, worker_count - 1, worker_count, worker_count + 1, large N.
    constexpr std::array<std::size_t, 6> kCounts{{0, 1, 3, 4, 5, 1'025}};
    for (const std::size_t n : kCounts) {
        SCOPED_TRACE(n);
        std::vector<std::size_t> visited(n, 0);
        pool.parallel_for(n, [&visited](std::size_t idx) noexcept {
            visited[idx] = idx + 1;
        });

        ASSERT_EQ(visited.size(), n);
        for (std::size_t i = 0; i < n; ++i) {
            EXPECT_EQ(visited[i], i + 1);
        }
    }

    // Exception during parallel_for must still wait for all workers before rethrowing.
    std::atomic<int> finished_chunks{0};
    EXPECT_THROW(
        pool.parallel_for(64, [&finished_chunks](std::size_t idx) {
            if (idx == 17) {
                throw std::runtime_error("boom at 17");
            }
            finished_chunks.fetch_add(1, std::memory_order_relaxed);
        }),
        std::runtime_error);
    // All non-throwing indices in non-failing chunks (and indices before throw) finished cleanly
    // before parallel_for returned; pool remains usable afterward.
    EXPECT_GT(finished_chunks.load(), 0);
    EXPECT_EQ(pool.submit([]() noexcept { return 99; }).get(), 99);
}

TEST(ThreadPoolTest, CancelPendingDeliversStopTokenAndRejectsSubsequentSubmissions) {
    core::ThreadPool pool(2);
    std::latch worker_started(1);
    std::atomic<bool> observed_stop{false};

    auto stoppable_future = pool.submit_stoppable([&](std::stop_token st) {
        worker_started.count_down();
        // Wait cooperatively on stop_token via condition variable/callback (zero sleep).
        std::mutex m;
        std::condition_variable_any cv;
        std::unique_lock lk(m);
        cv.wait(lk, st, [] { return false; });
        observed_stop.store(st.stop_requested(), std::memory_order_release);
        return st.stop_requested();
    });

    worker_started.wait();
    pool.cancel_pending();

    EXPECT_TRUE(observed_stop.load(std::memory_order_acquire));
    EXPECT_TRUE(stoppable_future.get());
    EXPECT_THROW((void)pool.submit([]() noexcept { return 1; }), std::runtime_error);
}
