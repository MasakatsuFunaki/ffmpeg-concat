#include "ffmpeg/progress_reporter.h"

#include <algorithm>

#include <indicators/cursor_control.hpp>

IndicatorsProgressReporter::IndicatorsProgressReporter()
    : bar_{
          indicators::option::BarWidth{40},
          indicators::option::Start{"["},
          indicators::option::Fill{"#"},
          indicators::option::Lead{">"},
          indicators::option::Remainder{"."},
          indicators::option::End{"]"},
          indicators::option::ShowPercentage{true},
          indicators::option::ShowElapsedTime{true},
          indicators::option::ShowRemainingTime{true},
      } {
    indicators::show_console_cursor(false);
}

void IndicatorsProgressReporter::update(const ProgressParser::Snapshot& snap, double total_sec) {
    int pct = static_cast<int>(std::min(100.0, snap.current_sec / total_sec * 100.0));
    bar_.set_option(indicators::option::PostfixText{
        "time=" + snap.out_time + " fps=" + snap.fps + " speed=" + snap.speed});
    bar_.set_progress(pct);
}

void IndicatorsProgressReporter::finish() {
    if (!bar_.is_completed()) bar_.mark_as_completed();
    indicators::show_console_cursor(true);
}

void IndicatorsProgressReporter::abort() {
    indicators::show_console_cursor(true);
}
