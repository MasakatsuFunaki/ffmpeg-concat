#pragma once

#include "common.h"

#include <compare>
#include <span>
#include <string>
#include <vector>

class CutApp final : public ToolApp {
protected:
    [[nodiscard]] std::string name() const override;
    void define_options(boost::program_options::options_description& desc) override;
    [[nodiscard]] std::string extra_help() const override;
    int execute(const boost::program_options::variables_map& vm) override;

private:
    struct Interval {
        double start = 0.0;
        double end = 0.0;

        [[nodiscard]] constexpr double length() const noexcept { return end - start; }
        [[nodiscard]] constexpr auto operator<=>(const Interval&) const noexcept = default;
    };

    [[nodiscard]] static std::vector<Interval> parse_intervals(std::span<const double> starts,
                                                               std::span<const double> ends);

    [[nodiscard]] static bool run_blur(const fs::path& input_file,
                                       const fs::path& output_file,
                                       std::span<const Interval> intervals,
                                       int blur_strength,
                                       bool sound_off,
                                       double duration_sec);
};
