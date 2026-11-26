#pragma once

#include "common.h"

#include <span>
#include <string>
#include <vector>

class SplitApp final : public ToolApp {
protected:
    [[nodiscard]] std::string name() const override;
    void define_options(boost::program_options::options_description& desc) override;
    [[nodiscard]] std::string extra_help() const override;
    int execute(const boost::program_options::variables_map& vm) override;

private:
    [[nodiscard]] static std::vector<double> compute_absolute_points(
        std::span<const double> points, bool relative);
};
