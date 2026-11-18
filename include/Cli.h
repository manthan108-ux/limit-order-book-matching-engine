#pragma once

#include <iosfwd>
#include <string_view>

#include "CommandParser.h"
#include "OrderManager.h"

namespace lob {

// The interactive terminal front end: executes parsed commands against an
// OrderManager and prints the results. It holds NO order state of its own: every
// fact it prints comes from the OrderManager (reports, lifecycle records, trade
// history) or from the book through the manager's const view, and every change is
// made by calling OrderManager. It never touches containers, never matches, and
// never re-validates lifecycle rules.
//
// Streams: normal results, including business rejections ("REJECTED: duplicate
// order id"), go to `out`; syntax errors and unexpected failures go to `err`.
// Output is plain text with fixed-width columns; it does not depend on colours,
// terminal size or terminal capabilities.
//
// Recovery (see OrderManager): after an out-of-memory failure the manager may have
// bookkeeping pending. The CLI never hides that:
//  * `order` and `trades` print a warning, and label what follows as possibly out
//    of date, whenever isHistoryCurrent() is false;
//  * mutating commands that finish while bookkeeping is still pending say so;
//  * the `recover` command calls OrderManager::recover() once and reports whether
//    the history is current afterwards (it can be repeated);
//  * the CLI never resubmits, retries or re-executes a trading command, and a
//    trading command that failed with an exception is reported as "may already
//    have executed: do not resubmit".
//
// None of the member functions throw.
class Cli {
public:
    struct Options {
        bool showPrompt = true;  // banner and "lob> " prompt (off for scripted input)
    };

    enum class Status { Continue, Exit };

    Cli(OrderManager& manager, std::ostream& out, std::ostream& err, Options options);
    Cli(OrderManager& manager, std::ostream& out, std::ostream& err)
        : Cli(manager, out, err, Options{}) {}

    // Parses and executes one line. A malformed line prints an error and returns
    // Continue; only `exit` / `quit` return Exit.
    Status handleLine(std::string_view line);

    // Executes an already parsed command.
    Status execute(const ParsedCommand& command);

    // Reads lines until `exit`/`quit` or end of input (which exits cleanly).
    // Returns the process exit code (always 0: bad input is not a failure of the program).
    int run(std::istream& in);

private:
    Status dispatch(const ParsedCommand& command);

    void doSubmit(const ParsedCommand& command);
    void doCancel(OrderId id);
    void doModify(const ParsedCommand& command);
    void doBook();
    void doDepth(std::size_t levels);
    void doOrder(OrderId id);
    void doTrades(OrderId id);
    void doBest();
    void doRecover();
    void doHelp();

    void printStaleWarning();
    void printPendingNote();
    void reportFailure(CommandType type, const char* what) noexcept;

    OrderManager& manager_;
    std::ostream& out_;
    std::ostream& err_;
    Options options_;
};

}  // namespace lob
