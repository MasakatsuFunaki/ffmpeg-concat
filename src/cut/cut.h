#pragma once

#include "common.h"

#include <string>
#include <vector>
#include <utility>

class CutApp : public ToolApp {
protected:
    std::string name() const override;
    void define_options(boost::program_options::options_description& desc) override;
    std::string extra_help() const override;
    int execute(const boost::program_options::variables_map& vm) override;

private:
    using Interval = std::pair<double, double>;

    std::vector<Interval> parse_intervals(const std::vector<double>& starts,
                                          const std::vector<double>& ends);

    bool run_blur(const Ffmpeg& runner,
                  const fs::path& input_file,
                  const fs::path& output_file,
                  const std::vector<Interval>& intervals,
                  int blur_strength,
                  bool sound_off,
                  double duration_sec);
};
