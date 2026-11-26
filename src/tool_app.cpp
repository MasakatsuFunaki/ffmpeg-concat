#include "common.h"

#include "platform/cli_args.h"

#include <string>
#include <vector>

namespace po = boost::program_options;

ToolApp::~ToolApp() = default;

std::string ToolApp::extra_help() const {
    return {};
}

int ToolApp::run(int argc, char* argv[]) {
    if (argc <= 0 || argv == nullptr) {
        return 1;
    }

    po::options_description desc(name() + " - Options");
    desc.add_options()
        ("help,h", "Show help message")
    ;
    define_options(desc);

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (argc == 1 || vm.count("help") > 0) {
            BOOST_LOG_TRIVIAL(info) << desc;
            if (const std::string help = extra_help(); !help.empty()) {
                BOOST_LOG_TRIVIAL(info) << help;
            }
            return 0;
        }
        po::notify(vm);
    } catch (const po::error& e) {
        BOOST_LOG_TRIVIAL(error) << "Error: " << e.what() << "\n" << desc;
        return 1;
    }

    return execute(vm);
}

int ToolApp::run_cli(int argc, char* argv[]) {
    std::vector<std::string> utf8_storage = platform::utf8_command_line(argc, argv);
    if (utf8_storage.empty()) {
        return run(argc, argv);
    }

    std::vector<char*> utf8_argv;
    utf8_argv.reserve(utf8_storage.size() + 1);
    for (std::string& arg : utf8_storage) {
        utf8_argv.push_back(arg.data());
    }
    utf8_argv.push_back(nullptr);

    return run(static_cast<int>(utf8_storage.size()), utf8_argv.data());
}
