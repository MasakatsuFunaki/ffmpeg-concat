#pragma once

// ProgressReporter – Strategy interface for surfacing a run's progress.
//   IndicatorsProgressReporter: in-place console bar (PIMPL-insulated from
//                               indicators and platform console headers).
//   NullProgressReporter:       no-op sink for headless runs and tests.

#include "ffmpeg/progress_parser.h"

#include <memory>

class ProgressReporter {
public:
    virtual ~ProgressReporter();

    virtual void update(const ProgressParser::Snapshot& snap, double total_sec) = 0;
    virtual void finish() = 0;  // normal completion: mark complete and restore cursor
    virtual void abort() = 0;   // failure/exception path: restore cursor only

protected:
    ProgressReporter() = default;
    ProgressReporter(const ProgressReporter&) = default;
    ProgressReporter& operator=(const ProgressReporter&) = default;
    ProgressReporter(ProgressReporter&&) noexcept = default;
    ProgressReporter& operator=(ProgressReporter&&) noexcept = default;
};

class IndicatorsProgressReporter final : public ProgressReporter {
public:
    IndicatorsProgressReporter();
    ~IndicatorsProgressReporter() override;

    IndicatorsProgressReporter(const IndicatorsProgressReporter&) = delete;
    IndicatorsProgressReporter& operator=(const IndicatorsProgressReporter&) = delete;
    IndicatorsProgressReporter(IndicatorsProgressReporter&&) noexcept;
    IndicatorsProgressReporter& operator=(IndicatorsProgressReporter&&) noexcept;

    void update(const ProgressParser::Snapshot& snap, double total_sec) override;
    void finish() override;
    void abort() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class NullProgressReporter final : public ProgressReporter {
public:
    void update(const ProgressParser::Snapshot&, double) override {}
    void finish() override {}
    void abort() override {}
};
