#pragma once

#include "common.h"

#include <string>

// ExtractApp – thin CLI shell (Template Method override) over the extractcore
// pattern components. It parses/validates flags, builds a vector of
// FrameExtractor strategies and runs them per video; all OpenCV/OpenSSL heavy
// lifting lives in extractcore.
class ExtractApp : public ToolApp {
protected:
    std::string name() const override;
    void define_options(boost::program_options::options_description& desc) override;
    std::string extra_help() const override;
    int execute(const boost::program_options::variables_map& vm) override;
};
