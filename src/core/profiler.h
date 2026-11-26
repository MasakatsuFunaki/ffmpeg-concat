#pragma once

// ---------------------------------------------------------------------------
// core/profiler.h – modern C++20 RAII scoped profiler and thread-safe stage
// registry.
//
// Design:
// - Disabled by default: when disabled, BasicScopedProfile performs a single
//   atomic load and skips Clock::now(), locking, and allocations completely.
// - Thread-safe under concurrent std::jthread recording: hot-path record()
//   takes a std::shared_lock and updates per-stage std::atomic counters so
//   recording threads never mutually exclude one another; snapshot() and
//   reset() take a std::unique_lock for multi-field consistency.
// - Templated on Clock (ProfileClock concept) so unit tests can inject a
//   deterministic manual clock with zero sleeps or wall-clock flakiness.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core {

template <typename C>
concept ProfileClock = requires {
    typename C::duration;
    typename C::time_point;
    { C::now() } -> std::same_as<typename C::time_point>;
};

struct StageStats {
    static constexpr double kNanosecondsPerMillisecond = 1'000'000.0;

    std::string name;
    std::uint64_t call_count = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t min_ns = 0;
    std::uint64_t max_ns = 0;

    [[nodiscard]] constexpr double total_ms() const noexcept {
        return static_cast<double>(total_ns) / kNanosecondsPerMillisecond;
    }

    [[nodiscard]] constexpr double min_ms() const noexcept {
        return static_cast<double>(min_ns) / kNanosecondsPerMillisecond;
    }

    [[nodiscard]] constexpr double max_ms() const noexcept {
        return static_cast<double>(max_ns) / kNanosecondsPerMillisecond;
    }

    [[nodiscard]] constexpr double avg_ns() const noexcept {
        return (call_count > 0)
                   ? static_cast<double>(total_ns) / static_cast<double>(call_count)
                   : 0.0;
    }

    [[nodiscard]] constexpr double avg_ms() const noexcept {
        return avg_ns() / kNanosecondsPerMillisecond;
    }

    [[nodiscard]] bool operator==(const StageStats&) const = default;
};

class Profiler {
public:
    explicit Profiler(bool initially_enabled = false) noexcept
        : enabled_(initially_enabled) {}

    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;
    Profiler(Profiler&&) = delete;
    Profiler& operator=(Profiler&&) = delete;

    [[nodiscard]] static Profiler& global() noexcept {
        static Profiler instance{false};
        return instance;
    }

    void set_enabled(bool enabled) noexcept {
        enabled_.store(enabled, std::memory_order_release);
    }

    [[nodiscard]] bool is_enabled() const noexcept {
        return enabled_.load(std::memory_order_acquire);
    }

    // Records a completed stage sample. No-op when disabled or when stage is empty.
    // Negative durations are clamped to 0 ns.
    void record(std::string_view stage, std::chrono::nanoseconds duration) {
        if (!is_enabled() || stage.empty()) {
            return;
        }

        const std::uint64_t ns =
            (duration.count() > 0) ? static_cast<std::uint64_t>(duration.count()) : 0ULL;

        // Hot path: shared lock + heterogeneous lookup + lock-free atomic updates.
        {
            std::shared_lock read_lock(mutex_);
            if (const auto it = stages_.find(stage); it != stages_.end()) {
                it->second->add_sample(ns);
                return;
            }
        }

        // Cold path: first sample ever recorded for this stage name.
        std::unique_lock write_lock(mutex_);
        auto [it, inserted] = stages_.try_emplace(std::string(stage), nullptr);
        if (inserted || !it->second) {
            it->second = std::make_unique<AtomicStageEntry>();
        }
        it->second->add_sample(ns);
    }

    // Clears all accumulated stage statistics without modifying enabled state.
    void reset() {
        std::unique_lock lock(mutex_);
        stages_.clear();
    }

    // Returns a consistent point-in-time snapshot sorted by total_ns descending,
    // tie-breaking lexicographically by stage name ascending.
    [[nodiscard]] std::vector<StageStats> snapshot() const {
        std::unique_lock lock(mutex_);
        std::vector<StageStats> result;
        result.reserve(stages_.size());

        for (const auto& [name, entry] : stages_) {
            if (!entry) {
                continue;
            }
            const std::uint64_t count = entry->call_count.load(std::memory_order_relaxed);
            if (count == 0) {
                continue;
            }
            result.push_back(StageStats{
                name,
                count,
                entry->total_ns.load(std::memory_order_relaxed),
                entry->min_ns.load(std::memory_order_relaxed),
                entry->max_ns.load(std::memory_order_relaxed),
            });
        }

        std::ranges::sort(result, [](const StageStats& a, const StageStats& b) noexcept {
            if (a.total_ns != b.total_ns) {
                return a.total_ns > b.total_ns;
            }
            return a.name < b.name;
        });
        return result;
    }

    [[nodiscard]] std::optional<StageStats> stage_stats(std::string_view stage) const {
        if (stage.empty()) {
            return std::nullopt;
        }
        std::unique_lock lock(mutex_);
        const auto it = stages_.find(stage);
        if (it == stages_.end() || !it->second) {
            return std::nullopt;
        }
        const std::uint64_t count = it->second->call_count.load(std::memory_order_relaxed);
        if (count == 0) {
            return std::nullopt;
        }
        return StageStats{
            it->first,
            count,
            it->second->total_ns.load(std::memory_order_relaxed),
            it->second->min_ns.load(std::memory_order_relaxed),
            it->second->max_ns.load(std::memory_order_relaxed),
        };
    }

    [[nodiscard]] std::string format_summary() const {
        const auto snap = snapshot();
        if (snap.empty()) {
            return "(no profile data)\n";
        }

        std::string out;
        out.reserve(128 + snap.size() * 96);
        out += std::format("{:<32} {:>8} {:>12} {:>10} {:>10} {:>10}\n",
                           "Stage", "Calls", "Total (ms)", "Avg (ms)", "Min (ms)", "Max (ms)");
        out += std::string(87, '-') + '\n';

        for (const auto& s : snap) {
            out += std::format("{:<32} {:>8} {:>12.3f} {:>10.3f} {:>10.3f} {:>10.3f}\n",
                               s.name, s.call_count, s.total_ms(), s.avg_ms(), s.min_ms(),
                               s.max_ms());
        }
        return out;
    }

    [[nodiscard]] std::string format_json() const {
        const auto snap = snapshot();
        std::string out;
        out.reserve(16 + snap.size() * 140);
        out += "{\"stages\":[";

        for (std::size_t i = 0; i < snap.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            const auto& s = snap[i];
            out += "{\"name\":\"";
            append_json_escaped(out, s.name);
            out += std::format(
                "\",\"calls\":{},\"total_ns\":{},\"min_ns\":{},\"max_ns\":{},"
                "\"avg_ns\":{:.3f},\"total_ms\":{:.6f},\"avg_ms\":{:.6f},"
                "\"min_ms\":{:.6f},\"max_ms\":{:.6f}}}",
                s.call_count, s.total_ns, s.min_ns, s.max_ns,
                s.avg_ns(), s.total_ms(), s.avg_ms(), s.min_ms(), s.max_ms());
        }

        out += "]}";
        return out;
    }

private:
    struct TransparentStringHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }
        [[nodiscard]] std::size_t operator()(const std::string& s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };

    struct AtomicStageEntry {
        std::atomic<std::uint64_t> call_count{0};
        std::atomic<std::uint64_t> total_ns{0};
        std::atomic<std::uint64_t> min_ns{std::numeric_limits<std::uint64_t>::max()};
        std::atomic<std::uint64_t> max_ns{0};

        void add_sample(std::uint64_t ns) noexcept {
            call_count.fetch_add(1, std::memory_order_relaxed);
            total_ns.fetch_add(ns, std::memory_order_relaxed);

            std::uint64_t cur_min = min_ns.load(std::memory_order_relaxed);
            while (ns < cur_min &&
                   !min_ns.compare_exchange_weak(cur_min, ns, std::memory_order_relaxed)) {
            }

            std::uint64_t cur_max = max_ns.load(std::memory_order_relaxed);
            while (ns > cur_max &&
                   !max_ns.compare_exchange_weak(cur_max, ns, std::memory_order_relaxed)) {
            }
        }
    };

    static void append_json_escaped(std::string& out, std::string_view text) {
        constexpr unsigned int kControlLimit = 0x20U;
        for (const char ch : text) {
            const auto uc = static_cast<unsigned char>(ch);
            switch (ch) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (uc < kControlLimit) {
                        out += std::format("\\u{:04x}", static_cast<unsigned int>(uc));
                    } else {
                        out.push_back(ch);
                    }
                    break;
            }
        }
    }

    std::atomic<bool> enabled_{false};
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<AtomicStageEntry>,
                       TransparentStringHash, std::equal_to<>>
        stages_;
};

// RAII guard that sets a Profiler's enabled state for the current scope and
// restores the prior state on destruction.
class ScopedProfilerEnable {
public:
    explicit ScopedProfilerEnable(Profiler& profiler, bool enabled = true) noexcept
        : profiler_(profiler), previous_(profiler.is_enabled()) {
        profiler_.set_enabled(enabled);
    }

    ~ScopedProfilerEnable() noexcept {
        profiler_.set_enabled(previous_);
    }

    ScopedProfilerEnable(const ScopedProfilerEnable&) = delete;
    ScopedProfilerEnable& operator=(const ScopedProfilerEnable&) = delete;
    ScopedProfilerEnable(ScopedProfilerEnable&&) = delete;
    ScopedProfilerEnable& operator=(ScopedProfilerEnable&&) = delete;

private:
    Profiler& profiler_;
    bool previous_;
};

// Zero-overhead-when-disabled RAII timer for a named pipeline stage.
template <ProfileClock Clock = std::chrono::steady_clock>
class BasicScopedProfile {
public:
    explicit BasicScopedProfile(std::string_view stage,
                                Profiler& profiler = Profiler::global()) noexcept
        : profiler_(profiler), stage_(stage) {
        if (!profiler_.is_enabled()) [[likely]] {
            return;
        }
        active_ = true;
        start_ = Clock::now();
    }

    ~BasicScopedProfile() noexcept { stop(); }

    BasicScopedProfile(const BasicScopedProfile&) = delete;
    BasicScopedProfile& operator=(const BasicScopedProfile&) = delete;
    BasicScopedProfile(BasicScopedProfile&&) = delete;
    BasicScopedProfile& operator=(BasicScopedProfile&&) = delete;

    void stop() noexcept {
        if (!active_) [[likely]] {
            return;
        }
        active_ = false;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_);
        try {
            profiler_.record(stage_, elapsed);
        } catch (...) {
        }
    }

    [[nodiscard]] bool is_active() const noexcept { return active_; }

private:
    Profiler& profiler_;
    std::string_view stage_;
    typename Clock::time_point start_{};
    bool active_ = false;
};

using ScopedProfile = BasicScopedProfile<std::chrono::steady_clock>;

}  // namespace core
