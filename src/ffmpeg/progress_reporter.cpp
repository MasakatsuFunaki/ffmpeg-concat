#include "ffmpeg/progress_reporter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>

// indicators' bundled termcolor.hpp references uint8_t without including
// <cstdint>; keep <cstdint> above the indicators headers for GCC 13+.
#include <indicators/cursor_control.hpp>
#include <indicators/progress_bar.hpp>

namespace {

constexpr std::size_t kBarWidth = 40;
constexpr double kMinPercent = 0.0;
constexpr double kMaxPercent = 100.0;

}  // namespace

ProgressReporter::~ProgressReporter() = default;

struct IndicatorsProgressReporter::Impl {
    indicators::ProgressBar bar{
        indicators::option::BarWidth{kBarWidth},
        indicators::option::Start{"["},
        indicators::option::Fill{"#"},
        indicators::option::Lead{">"},
        indicators::option::Remainder{"."},
        indicators::option::End{"]"},
        indicators::option::ShowPercentage{true},
        indicators::option::ShowElapsedTime{true},
        indicators::option::ShowRemainingTime{true},
    };
};

IndicatorsProgressReporter::IndicatorsProgressReporter()
    : impl_(std::make_unique<Impl>()) {
    indicators::show_console_cursor(false);
}

IndicatorsProgressReporter::~IndicatorsProgressReporter() = default;

IndicatorsProgressReporter::IndicatorsProgressReporter(IndicatorsProgressReporter&&) noexcept = default;

IndicatorsProgressReporter& IndicatorsProgressReporter::operator=(
    IndicatorsProgressReporter&&) noexcept = default;

void IndicatorsProgressReporter::update(const ProgressParser::Snapshot& snap, double total_sec) {
    if (!impl_ || !(total_sec > 0.0)) {
        return;
    }
    const double raw_pct = (snap.current_sec / total_sec) * kMaxPercent;
    if (!std::isfinite(raw_pct)) {
        return;
    }
    const auto pct = static_cast<std::size_t>(std::clamp(raw_pct, kMinPercent, kMaxPercent));
    impl_->bar.set_option(indicators::option::PostfixText{
        std::format("time={} fps={} speed={}", snap.out_time, snap.fps, snap.speed)});
    impl_->bar.set_progress(pct);
}

void IndicatorsProgressReporter::finish() {
    if (!impl_) {
        return;
    }
    if (!impl_->bar.is_completed()) {
        impl_->bar.mark_as_completed();
    }
    indicators::show_console_cursor(true);
}

void IndicatorsProgressReporter::abort() {
    if (!impl_) {
        return;
    }
    indicators::show_console_cursor(true);
}
