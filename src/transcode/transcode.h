#pragma once

#include "common.h"

#include <string>

class TranscodeApp : public ToolApp {
protected:
    std::string name() const override;
    void define_options(boost::program_options::options_description& desc) override;
    int execute(const boost::program_options::variables_map& vm) override;
};
