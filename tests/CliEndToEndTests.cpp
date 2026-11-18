// End-to-end tests of the real, compiled lob_cli executable: scripted sessions piped
// through its standard input, with stdout, stderr and the exit code captured separately.
// (The Cli class itself is covered in-process by CliTests.cpp; this checks the program
// as a user runs it: argument handling, streams, exit codes, EOF.)
//
// The executable's path is passed by CTest as argv[1] (so paths containing spaces work);
// if absent, the LOB_CLI_PATH environment variable or "../lob_cli" is used.
//
// Limitation: stale-history recovery cannot be produced in the real executable (it
// needs simulated memory exhaustion); that is covered in-process by CliRecoveryTests.
// Here only `recover` on a healthy session is exercised.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "TestFramework.h"

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace {

std::string g_cliPath;

struct Result {
    int exitCode = -1;
    std::string out;
    std::string err;
};

std::string quote(const std::string& s) { return "\"" + s + "\""; }

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Runs lob_cli with `args`, feeding it `input` on stdin.
Result run(const std::string& args, const std::string& input) {
    static int counter = 0;
    const std::string base = "e2e_run_" + std::to_string(++counter);
    const std::string inFile = base + ".in", outFile = base + ".out", errFile = base + ".err";
    {
        std::ofstream f(inFile, std::ios::binary);
        f << input;
    }
    std::string cmd = quote(g_cliPath) + " " + args + " < " + quote(inFile) + " > " + quote(outFile) + " 2> " + quote(errFile);
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";   // cmd.exe /c removes one pair of outer quotes
#endif
    int rc = std::system(cmd.c_str());
#if !defined(_WIN32)
    rc = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
    Result r;
    r.exitCode = rc;
    r.out = slurp(outFile);
    r.err = slurp(errFile);
    std::remove(inFile.c_str());
    std::remove(outFile.c_str());
    std::remove(errFile.c_str());
    return r;
}

Result session(const std::string& script) { return run("--quiet", script); }

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::size_t count(const std::string& text, const std::string& part) {
    std::size_t n = 0;
    for (std::size_t p = text.find(part); p != std::string::npos; p = text.find(part, p + part.size())) ++n;
    return n;
}

std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream is(line);
    for (std::string w; is >> w;) out.push_back(w);
    return out;
}

// The data rows of the first trade table in `text`: each row split into columns
// [trade id, price, qty, buyer, seller, taker, seq].
std::vector<std::vector<std::string>> tradeRows(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    std::istringstream is(text);
    std::string line;
    bool inTable = false;
    while (std::getline(is, line)) {
        if (line.find("TRADE ID") != std::string::npos) { inTable = true; continue; }
        if (!inTable) continue;
        const auto w = words(line);
        if (w.size() == 7 && std::isdigit(static_cast<unsigned char>(w[0][0])) != 0) rows.push_back(w);
        else break;
    }
    return rows;
}

}  // namespace

// ---- The full scripted session -------------------------------------------------------------

TEST(FullSessionFromSubmissionThroughCancellationAndExit) {
    const Result r = session(
        "submit BUY 100 50\n"      // order 1 rests
        "submit SELL 99 30\n"      // order 2 crosses: executes at the RESTING price 100
        "book\n"
        "order 1\n"
        "order 2\n"
        "trades 1\n"
        "trades 2\n"
        "submit BUY 95 10\n"       // order 3 rests
        "modify 3 96 10\n"         // price change: priority reset
        "modify 3 96 4\n"          // quantity decrease: priority kept
        "depth 2\n"
        "best\n"
        "cancel 1\n"               // cancels the remaining 20
        "cancel 3\n"
        "book\n"
        "submit BUY abc 5\n"       // invalid input: error, session continues
        "frobnicate\n"
        "recover\n"                // healthy session: nothing to recover
        "exit\n");
    CHECK_EQ(r.exitCode, 0);

    // 1-2: the resting order was accepted; the crossing order was accepted, filled.
    CHECK(has(r.out, "Order 1: ACCEPTED"));
    CHECK(has(r.out, "Order 2: ACCEPTED"));
    // 3: the trade executed at the resting BUY price (100), not the sell's limit (99).
    const auto rows = tradeRows(r.out);
    CHECK(!rows.empty());
    if (!rows.empty()) {
        CHECK_EQ(rows[0][1], "100");   // price
        CHECK_EQ(rows[0][2], "30");    // quantity
        CHECK_EQ(rows[0][3], "1");     // buyer = the resting order
        CHECK_EQ(rows[0][4], "2");     // seller = the incoming order
        CHECK_EQ(rows[0][5], "SELL");  // taker side
    }
    // 4: book after the trade: 20 units left on the bid at 100.
    CHECK(has(r.out, "Best bid:  100 ticks (20 units in 1 order)"));
    // 5-6: order and trade history.
    CHECK(has(r.out, "PARTIALLY_FILLED"));
    CHECK(has(r.out, "Trades for order 1"));
    CHECK(has(r.out, "Trades for order 2"));
    // 7-8: cancel and modify, with the priority consequence stated.
    CHECK(has(r.out, "Order 1: CANCELLED"));
    CHECK(has(r.out, "executed (kept): 30"));
    CHECK(has(r.out, "cancelled:       20"));
    CHECK(has(r.out, "RESET"));
    CHECK(has(r.out, "PRESERVED"));
    // 9: depth.
    CHECK(has(r.out, "Top 2 level(s) per side"));
    // 10: invalid input goes to stderr, not stdout, and did not stop the session.
    CHECK(has(r.err, "error: PRICE 'abc'"));
    CHECK(has(r.err, "unknown command 'frobnicate'"));
    CHECK(!has(r.out, "error:"));
    // 11: recovery on a healthy session.
    CHECK(has(r.out, "already current"));
    // 12: results are on stdout only (stderr holds nothing but the two errors above).
    CHECK_EQ(count(r.err, "error:"), 2u);
    CHECK(!has(r.err, "ACCEPTED"));
}

TEST(TheCrossingExecutesAtTheRestingBuyPriceNotTheIncomingSellPrice) {
    const Result r = session("submit BUY 100 10\nsubmit SELL 90 10\ntrades 2\nexit\n");
    const auto rows = tradeRows(r.out.substr(r.out.find("Trades for order 2")));
    CHECK(!rows.empty());
    if (!rows.empty()) CHECK_EQ(rows[0][1], "100");
    CHECK_EQ(r.exitCode, 0);
}

// ---- Process-level behaviour -------------------------------------------------------------------

TEST(EmptyStandardInputExitsCleanly) {
    const Result quiet = run("--quiet", "");
    CHECK_EQ(quiet.exitCode, 0);
    CHECK(quiet.out.empty());
    CHECK(quiet.err.empty());
    const Result chatty = run("", "");
    CHECK_EQ(chatty.exitCode, 0);
    CHECK(has(chatty.out, "Type 'help'"));   // banner and prompt when interactive
    CHECK(has(chatty.out, "lob> "));
    CHECK(chatty.err.empty());
}

TEST(EndOfInputWithoutExitIsANormalFinish) {
    const Result r = session("submit BUY 100 5\nbook");   // no trailing newline, no exit
    CHECK_EQ(r.exitCode, 0);
    CHECK(has(r.out, "Order 1: ACCEPTED"));
    CHECK(has(r.out, "Best bid:  100 ticks"));
}

TEST(ExitAndQuitStopProcessingFurtherInput) {
    for (const char* word : {"exit", "quit", "EXIT"}) {
        const Result r = session(std::string("submit BUY 100 5\n") + word + "\nsubmit BUY 101 5\nbook\n");
        CHECK_EQ(r.exitCode, 0);
        CHECK(has(r.out, "Order 1: ACCEPTED"));
        CHECK(!has(r.out, "Order 2"));     // nothing after `exit` ran
        CHECK(!has(r.out, "ASKS"));
    }
}

TEST(HelpFlagPrintsUsageAndExitsZero) {
    for (const char* flag : {"--help", "-h"}) {
        const Result r = run(flag, "submit BUY 100 5\n");
        CHECK_EQ(r.exitCode, 0);
        CHECK(has(r.out, "usage: lob_cli"));
        CHECK(has(r.out, "--quiet"));
        CHECK(r.err.empty());
        CHECK(!has(r.out, "ACCEPTED"));   // stdin was not processed
    }
}

TEST(UnknownFlagsAreRefusedWithExitCodeTwoAndRunNothing) {
    for (const char* flag : {"--bogus", "-x", "--quiet --bogus", "quiet"}) {
        const Result r = run(flag, "submit BUY 100 5\nbook\n");
        CHECK_EQ(r.exitCode, 2);
        CHECK(has(r.err, "unknown option"));
        CHECK(has(r.err, "usage: lob_cli"));
        CHECK(r.out.empty());                // not even the session banner or any result
    }
}

TEST(QuietFlagSuppressesBannerAndPromptOnly) {
    const Result quiet = session("help\n");
    const Result chatty = run("", "help\n");
    CHECK(!has(quiet.out, "lob> "));
    CHECK(has(chatty.out, "lob> "));
    // The command output itself is identical.
    CHECK(has(quiet.out, "submit <BUY|SELL> <PRICE> <QUANTITY> [ID]"));
    CHECK(has(chatty.out, "submit <BUY|SELL> <PRICE> <QUANTITY> [ID]"));
}

TEST(ExitCodeIsZeroEvenAfterManyBadCommands) {
    std::string script;
    for (int i = 0; i < 100; ++i) script += "garbage " + std::to_string(i) + "\nsubmit BUY 0 1\ncancel\n";
    const Result r = session(script + "exit\n");
    CHECK_EQ(r.exitCode, 0);
    CHECK_EQ(count(r.err, "error:"), 300u);
    CHECK(r.out.empty());
}

// ---- Invalid arguments, repeated commands, streams ----------------------------------------------------

TEST(InvalidArgumentsAreReportedOnStderrWithoutChangingTheSession) {
    const Result r = session(
        "submit BUY 100 5\n"
        "submit BUY 10.5 5\nsubmit BUY -3 5\nsubmit BUY 100 0\nsubmit HOLD 1 1\nsubmit BUY 1e3 5\n"
        "submit BUY 9223372036854775808 5\ncancel x\ncancel -1\nmodify 1 0 5\nmodify 1 100 0\ndepth 0\ndepth 1001\n"
        "book extra\n"
        "book\n");
    CHECK_EQ(r.exitCode, 0);
    CHECK_EQ(count(r.err, "error:"), 13u);
    CHECK_EQ(count(r.out, "ACCEPTED"), 1u);           // only the one valid submission
    CHECK(has(r.out, "Best bid:  100 ticks (5 units in 1 order)"));
}

TEST(RepeatedCommandsEachTakeEffectOnce) {
    std::string script;
    for (int i = 0; i < 50; ++i) script += "submit BUY 100 2\n";
    for (int i = 0; i < 5; ++i) script += "cancel 7\n";
    script += "book\n";
    const Result r = session(script);
    CHECK_EQ(r.exitCode, 0);
    CHECK_EQ(count(r.out, "ACCEPTED"), 50u);
    CHECK_EQ(count(r.out, "Order 7: CANCELLED"), 1u);                 // the first cancel
    CHECK_EQ(count(r.out, "cancel REJECTED (order is not active)"), 4u);  // the repeats
    CHECK(has(r.out, "Best bid:  100 ticks (98 units in 49 orders)"));
}

TEST(TheCliNeverReportsSuccessForAnOperationThatDidNotSucceed) {
    const Result r = session(
        "submit BUY 100 5 7\n"       // accepted
        "submit SELL 100 5 7\n"      // duplicate id: must NOT be reported as accepted, and must not trade
        "cancel 999\n"               // unknown: must not say CANCELLED
        "modify 7 100 5\n"           // no change: must not say MODIFIED
        "modify 999 100 5\n"         // unknown
        "cancel 7\n"                 // genuinely cancelled
        "cancel 7\n"                 // already cancelled: must not say CANCELLED again
        "trades 7\n");
    CHECK_EQ(r.exitCode, 0);
    CHECK_EQ(count(r.out, "ACCEPTED"), 1u);
    CHECK_EQ(count(r.out, "Order 7: CANCELLED"), 1u);   // the success line only (a failed cancel prints "current state: CANCELLED")
    CHECK_EQ(count(r.out, "Order 7: MODIFIED"), 0u);
    CHECK_EQ(count(r.out, "REJECTED"), 5u);                // duplicate, 2 unknown/none, no-change, repeat cancel
    CHECK(has(r.out, "(duplicate order id)"));
    CHECK(has(r.out, "(order not found)"));
    CHECK(has(r.out, "(modification changes nothing)"));
    CHECK(has(r.out, "(none)"));                           // order 7 never traded
}

TEST(ResultsGoToStdoutAndErrorsToStderrNeverMixed) {
    const Result r = session("submit BUY 100 5\nbogus\nbook\nsubmit BUY x 5\nbest\n");
    CHECK(!has(r.out, "error:"));
    CHECK(!has(r.err, "Order 1"));
    CHECK(!has(r.err, "Best bid"));
    CHECK_EQ(count(r.err, "error:"), 2u);
}

// The real executable, fed the bytes Windows PowerShell 5.1 produces when it pipes text: a UTF-8 byte order
// mark in front of the first line. (Literals are split so a hex escape cannot swallow the next letter.)
TEST(TheRealCliAcceptsAUtf8ByteOrderMarkOnTheFirstLineOnly) {
    const Result withBom = session("\xEF\xBB\xBF" "submit BUY 100 5\nsubmit SELL 100 5\ntrades 1\nexit\n");
    CHECK_EQ(withBom.exitCode, 0);
    CHECK(withBom.err.empty());
    CHECK_EQ(count(withBom.out, "ACCEPTED"), 2u);
    CHECK(has(withBom.out, "Order 1: ACCEPTED"));

    const Result without = session("submit BUY 100 5\nsubmit SELL 100 5\ntrades 1\nexit\n");
    CHECK(without.err.empty());
    CHECK(withBom.out == without.out);  // identical behaviour with and without the mark

    const Result later = session("submit BUY 100 5\n\xEF\xBB\xBF" "submit SELL 100 5\nexit\n");
    CHECK_EQ(count(later.out, "ACCEPTED"), 1u);   // a mark on a later line is NOT removed
    CHECK(has(later.err, "unknown command"));
}

TEST(WindowsLineEndingsAndStrayWhitespaceAreAccepted) {
    const Result r = session("  SUBMIT\tbuy   100  10\r\n\tSubmit SELL 100 10  \r\n   \r\ntrades 1\r\nQuit\r\n");
    CHECK_EQ(r.exitCode, 0);
    CHECK(r.err.empty());
    CHECK_EQ(tradeRows(r.out.substr(r.out.find("Trades for order 1"))).size(), 1u);
}

TEST(OverlongInputLineIsRejectedAndTheSessionContinues) {
    const Result r = session(std::string(5000, 'x') + "\nsubmit BUY 100 5\n");
    CHECK_EQ(r.exitCode, 0);
    CHECK(has(r.err, "too long"));
    CHECK(has(r.out, "Order 1: ACCEPTED"));
}

TEST(ALargeScriptThroughTheRealBinaryStaysConsistent) {
    // 2000 alternating orders at one price: every pair trades, so the book ends empty.
    std::string script;
    for (int i = 0; i < 2000; ++i) script += i % 2 == 0 ? "submit BUY 100 3\n" : "submit SELL 100 3\n";
    script += "book\norder 1999\norder 2000\nexit\n";
    const Result r = session(script);
    CHECK_EQ(r.exitCode, 0);
    CHECK(r.err.empty());
    CHECK_EQ(count(r.out, "ACCEPTED"), 2000u);
    CHECK_EQ(count(r.out, "(none)"), 2u);       // empty asks and empty bids
    CHECK_EQ(count(r.out, "FILLED"), 1000u + 2u);  // 1000 filled takers shown in submit replies + 2 `order` queries
}

// A custom main: the executable path arrives as argv[1].
int main(int argc, char** argv) {
    if (argc >= 2) {
        g_cliPath = argv[1];
    } else if (const char* env = std::getenv("LOB_CLI_PATH")) {
        g_cliPath = env;
    } else {
        g_cliPath = "../lob_cli";
    }
    return ::testfw::runAll();
}
