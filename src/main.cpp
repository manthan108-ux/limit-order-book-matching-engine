// Entry point of the interactive command line application. All behaviour lives in
// Cli / CommandParser / OrderManager; this file only wires standard streams to them.
#include <exception>
#include <iostream>
#include <string_view>

#include "Cli.h"
#include "OrderManager.h"

namespace {

void printUsage(std::ostream& os) {
    os << "usage: lob_cli [--quiet]\n"
          "  Interactive limit order book. Reads commands from standard input; type 'help'.\n"
          "  --quiet, -q   no banner and no prompt (for piped or scripted input)\n"
          "  --help, -h    show this text\n";
}

}  // namespace

int main(int argc, char** argv) {
    lob::Cli::Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--quiet" || arg == "-q") {
            options.showPrompt = false;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(std::cout);
            return 0;
        } else {
            std::cerr << "error: unknown option '" << arg << "'\n";
            printUsage(std::cerr);
            return 2;
        }
    }

    try {
        lob::OrderManager manager;
        lob::Cli cli(manager, std::cout, std::cerr, options);
        return cli.run(std::cin);
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "fatal: unknown error\n";
    }
    return 1;
}
