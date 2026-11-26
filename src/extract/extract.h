#pragma once

// ExtractApp -- thin CLI shell (Template Method override) over extractcore.

#include "common.h"

#include <string>

class ExtractApp final : public ToolApp {
protected:
    [[nodiscard]] std::string name() const override;
    void define_options(boost::program_options::options_description& desc) override;
    [[nodiscard]] std::string extra_help() const override;
    int execute(const boost::program_options::variables_map& vm) override;
};
