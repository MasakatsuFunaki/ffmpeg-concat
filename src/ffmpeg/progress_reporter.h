#pragma once

// ---------------------------------------------------------------------------
// ProgressReporter – how a run's progress is surfaced (GoF "Strategy" pattern).
//
//   IndicatorsProgressReporter – draws an in-place indicators progress bar.
//   NullProgressReporter        – no-op sink.
//
// update() advances the display, finish() marks a normal completion and restores
// the cursor, abort() (exception path) only restores the cursor.
// ---------------------------------------------------------------------------

#include "ffmpeg/progress_parser.h"

// indicators' bundled termcolor.hpp uses uint8_t without including <cstdint>;
// GCC 13+ no longer provides it transitively, so include it before indicators.
#include <cstdint>

#include <indicators/progress_bar.hpp>

class ProgressReporter {
public:
    virtual ~ProgressReporter() = default;
    virtual void update(const ProgressParser::Snapshot&, double total_sec) = 0;
    virtual void finish() = 0;   // normal completion: mark bar complete + restore cursor
    virtual void abort()  = 0;   // exception path: restore cursor ONLY (no completion draw)
};

class IndicatorsProgressReporter : public ProgressReporter {
public:
    // Hides the cursor and constructs the bar UNCONDITIONALLY (even when the caller
    // never has a positive total to report against).
    IndicatorsProgressReporter();
    void update(const ProgressParser::Snapshot& snap, double total_sec) override;
    void finish() override;
    void abort() override;

private:
    indicators::ProgressBar bar_;
};

class NullProgressReporter : public ProgressReporter {
public:
    void update(const ProgressParser::Snapshot&, double) override {}
    void finish() override {}
    void abort() override {}
};
