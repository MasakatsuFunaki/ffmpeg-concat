#pragma once

#include "common.h"

#include <vector>

class SplitApp : public ToolApp {
protected:
    std::string name() const override;
    void define_options(boost::program_options::options_description& desc) override;
    std::string extra_help() const override;
    int execute(const boost::program_options::variables_map& vm) override;

private:
    std::vector<double> compute_absolute_points(const std::vector<double>& points, bool relative);
};
