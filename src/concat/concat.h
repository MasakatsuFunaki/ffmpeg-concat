#pragma once

#include "common.h"

#include <string>

class ConcatApp final : public ToolApp {
protected:
    [[nodiscard]] std::string name() const override;
    void define_options(boost::program_options::options_description& desc) override;
    int execute(const boost::program_options::variables_map& vm) override;
};
