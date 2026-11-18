#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "CommandParser.h"
#include "TestFramework.h"

using namespace lob;

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

ParseResult parse(const std::string& line) { return parseCommand(line); }

// A line that must be rejected, and whose message mentions `part`.
bool rejects(const std::string& line, const std::string& part) {
    const ParseResult r = parse(line);
    if (r.ok()) {
        std::cerr << "    expected an error for: " << line << "\n";
        return false;
    }
    if (!contains(r.error, part)) {
        std::cerr << "    error for [" << line << "] was [" << r.error << "], expected it to mention [" << part << "]\n";
        return false;
    }
    return true;
}

const std::string kInt64Max = "9223372036854775807";
const std::string kInt64MaxPlusOne = "9223372036854775808";
const std::string kUint64Max = "18446744073709551615";
const std::string kUint64MaxPlusOne = "18446744073709551616";

}  // namespace

// ---- Tokenizing ----------------------------------------------------------------------------

TEST(TokenizeHandlesRepeatedAndMixedWhitespace) {
    const std::string line = "  \t submit   BUY\t100  \r 50  ";
    const auto t = tokenize(line);
    CHECK_EQ(t.size(), 4u);
    CHECK_EQ(std::string(t[0]), "submit");
    CHECK_EQ(std::string(t[3]), "50");
    CHECK(tokenize("").empty());
    CHECK(tokenize("   \t  ").empty());
}

// ---- Command recognition ------------------------------------------------------------------------

TEST(EveryCommandIsRecognised) {
    CHECK(parse("submit BUY 100 50").command.type == CommandType::Submit);
    CHECK(parse("cancel 7").command.type == CommandType::Cancel);
    CHECK(parse("modify 7 102 40").command.type == CommandType::Modify);
    CHECK(parse("book").command.type == CommandType::Book);
    CHECK(parse("depth 5").command.type == CommandType::Depth);
    CHECK(parse("order 7").command.type == CommandType::Order);
    CHECK(parse("trades 7").command.type == CommandType::Trades);
    CHECK(parse("best").command.type == CommandType::Best);
    CHECK(parse("recover").command.type == CommandType::Recover);
    CHECK(parse("help").command.type == CommandType::Help);
    CHECK(parse("exit").command.type == CommandType::Exit);
    CHECK(parse("quit").command.type == CommandType::Exit);
    for (const char* ok : {"book", "best", "recover", "help", "exit", "quit"}) CHECK(parse(ok).ok());
}

TEST(CommandsAndSidesAreCaseInsensitive) {
    CHECK(parse("SUBMIT buy 100 50").ok());
    CHECK(parse("SUBMIT buy 100 50").command.side == Side::Buy);
    CHECK(parse("Submit SeLl 100 50").command.side == Side::Sell);
    CHECK(parse("BOOK").command.type == CommandType::Book);
    CHECK(parse("Exit").command.type == CommandType::Exit);
    CHECK(parse("QUIT").command.type == CommandType::Exit);
    CHECK(parse("Cancel 3").command.type == CommandType::Cancel);
}

TEST(EmptyAndWhitespaceOnlyLinesAreNotErrors) {
    for (const char* line : {"", " ", "   \t  ", "\r"}) {
        const ParseResult r = parse(line);
        CHECK(r.ok());
        CHECK(r.command.type == CommandType::Empty);
    }
}

TEST(UnknownCommandsAreRejected) {
    CHECK(rejects("frobnicate", "unknown command"));
    CHECK(rejects("sell 100 5", "unknown command"));  // a side is not a command
    CHECK(rejects("submit1 BUY 1 1", "unknown command"));
    CHECK(rejects("?", "unknown command"));
    CHECK(rejects("book now", "too many arguments"));  // a known word with arguments it does not take
}

// ---- submit --------------------------------------------------------------------------------------------

TEST(SubmitParsesAllArguments) {
    const ParseResult r = parse("submit BUY 100 50");
    CHECK(r.ok());
    CHECK(r.command.side == Side::Buy);
    CHECK_EQ(r.command.price, 100);
    CHECK_EQ(r.command.quantity, 50);
    CHECK(!r.command.orderId.has_value());  // the manager will generate the id

    const ParseResult withId = parse("submit SELL 105 30 1001");
    CHECK(withId.ok());
    CHECK(withId.command.side == Side::Sell);
    CHECK_EQ(withId.command.price, 105);
    CHECK_EQ(withId.command.quantity, 30);
    CHECK(withId.command.orderId.has_value());
    CHECK_EQ(*withId.command.orderId, 1001u);
}

TEST(SubmitToleratesWhitespace) {
    const ParseResult r = parse("   submit \t BUY   100    50   42  \r");
    CHECK(r.ok());
    CHECK_EQ(r.command.price, 100);
    CHECK_EQ(*r.command.orderId, 42u);
}

TEST(SubmitArity) {
    CHECK(rejects("submit", "missing argument"));
    CHECK(rejects("submit BUY", "missing argument"));
    CHECK(rejects("submit BUY 100", "missing argument"));
    CHECK(rejects("submit BUY 100 50 7 extra", "too many arguments"));
    CHECK(rejects("submit BUY 100 50 7 8", "too many arguments"));
    CHECK(rejects("submit", "submit <BUY|SELL>"));  // the usage is shown
}

TEST(SubmitRejectsInvalidSide) {
    CHECK(rejects("submit HOLD 100 50", "SIDE"));
    CHECK(rejects("submit B 100 50", "SIDE"));
    CHECK(rejects("submit 100 BUY 50", "SIDE"));
    CHECK(rejects("submit buyy 100 50", "SIDE"));
}

TEST(SubmitRejectsInvalidPrice) {
    CHECK(rejects("submit BUY abc 50", "PRICE"));
    CHECK(rejects("submit BUY 0 50", "PRICE"));
    CHECK(rejects("submit BUY -5 50", "PRICE"));
    CHECK(rejects("submit BUY 10.5 50", "never rounded"));  // decimals are refused, not rounded
    CHECK(rejects("submit BUY 10.0 50", "never rounded"));
    CHECK(rejects("submit BUY 1e3 50", "PRICE"));
    CHECK(rejects("submit BUY +5 50", "PRICE"));
    CHECK(rejects("submit BUY 0x10 50", "PRICE"));
    CHECK(rejects("submit BUY 100abc 50", "PRICE"));
    CHECK(rejects("submit BUY 1,000 50", "PRICE"));
    CHECK(rejects("submit BUY - 50", "PRICE"));
}

TEST(SubmitRejectsInvalidQuantity) {
    CHECK(rejects("submit BUY 100 abc", "QUANTITY"));
    CHECK(rejects("submit BUY 100 0", "QUANTITY"));
    CHECK(rejects("submit BUY 100 -3", "QUANTITY"));
    CHECK(rejects("submit BUY 100 2.5", "QUANTITY"));
    CHECK(rejects("submit BUY 100 5x", "QUANTITY"));
}

TEST(SubmitRejectsInvalidClientIds) {
    CHECK(rejects("submit BUY 100 50 abc", "ORDER_ID"));
    CHECK(rejects("submit BUY 100 50 0", "ORDER_ID"));
    CHECK(rejects("submit BUY 100 50 -1", "ORDER_ID"));
    CHECK(rejects("submit BUY 100 50 1.5", "ORDER_ID"));
}

TEST(NumericOverflowIsDetectedNotWrapped) {
    CHECK(rejects("submit BUY " + kInt64MaxPlusOne + " 5", "out of range"));
    CHECK(rejects("submit BUY 5 " + kInt64MaxPlusOne, "out of range"));
    CHECK(rejects("submit BUY 5 5 " + kUint64MaxPlusOne, "out of range"));
    CHECK(rejects("submit BUY 99999999999999999999999999 5", "out of range"));
    CHECK(rejects("submit BUY -99999999999999999999999999 5", "out of range"));
    CHECK(rejects("cancel " + kUint64MaxPlusOne, "out of range"));
    CHECK(rejects("depth 99999999999999999999999999", "out of range"));
    // A numeric prefix followed by junk is "invalid", not "out of range".
    CHECK(rejects("submit BUY 99999999999999999999999999x 5", "not a valid"));
}

TEST(LargestRepresentableValuesAreAccepted) {
    const ParseResult r = parse("submit BUY " + kInt64Max + " " + kInt64Max + " " + kUint64Max);
    CHECK(r.ok());
    CHECK_EQ(r.command.price, std::numeric_limits<Price>::max());
    CHECK_EQ(r.command.quantity, std::numeric_limits<Quantity>::max());
    CHECK_EQ(*r.command.orderId, std::numeric_limits<OrderId>::max());
}

TEST(LeadingZerosAreAcceptedAsPlainDecimal) {
    const ParseResult r = parse("submit BUY 007 0050");
    CHECK(r.ok());
    CHECK_EQ(r.command.price, 7);
    CHECK_EQ(r.command.quantity, 50);
}

// ---- cancel / order / trades -------------------------------------------------------------------------

TEST(OrderIdCommandsParseTheirArgument) {
    for (const char* word : {"cancel", "order", "trades"}) {
        const ParseResult r = parse(std::string(word) + " 1001");
        CHECK(r.ok());
        CHECK(r.command.orderId.has_value());
        CHECK_EQ(*r.command.orderId, 1001u);
    }
}

TEST(OrderIdCommandsValidateTheirArgument) {
    for (const char* word : {"cancel", "order", "trades"}) {
        const std::string w = word;
        CHECK(rejects(w, "missing argument"));
        CHECK(rejects(w + " 1 2", "too many arguments"));
        CHECK(rejects(w + " abc", "ORDER_ID"));
        CHECK(rejects(w + " 0", "ORDER_ID"));
        CHECK(rejects(w + " -4", "ORDER_ID"));
        CHECK(rejects(w + " 3.5", "ORDER_ID"));
        CHECK(rejects(w + " " + kUint64MaxPlusOne, "out of range"));
    }
}

// ---- modify -------------------------------------------------------------------------------------------------

TEST(ModifyParsesAllThreeArguments) {
    const ParseResult r = parse("modify 1001 102 40");
    CHECK(r.ok());
    CHECK_EQ(*r.command.orderId, 1001u);
    CHECK_EQ(r.command.price, 102);
    CHECK_EQ(r.command.quantity, 40);  // the new REMAINING quantity
}

TEST(ModifyValidatesArguments) {
    CHECK(rejects("modify", "missing argument"));
    CHECK(rejects("modify 1", "missing argument"));
    CHECK(rejects("modify 1 100", "missing argument"));
    CHECK(rejects("modify 1 100 5 6", "too many arguments"));
    CHECK(rejects("modify x 100 5", "ORDER_ID"));
    CHECK(rejects("modify 1 0 5", "PRICE"));
    CHECK(rejects("modify 1 -2 5", "PRICE"));
    CHECK(rejects("modify 1 10.5 5", "never rounded"));
    CHECK(rejects("modify 1 100 abc", "NEW_REMAINING_QUANTITY"));
    CHECK(rejects("modify 1 100 -5", "NEW_REMAINING_QUANTITY"));
    CHECK(rejects("modify 1 " + kInt64MaxPlusOne + " 5", "out of range"));
}

TEST(ModifyToZeroQuantityPointsToCancel) {
    CHECK(rejects("modify 1 100 0", "cancel"));
}

// ---- depth --------------------------------------------------------------------------------------------------------

TEST(DepthAcceptsTheValidRange) {
    CHECK_EQ(parse("depth 1").command.levels, 1u);
    CHECK_EQ(parse("depth 5").command.levels, 5u);
    CHECK_EQ(parse("depth 1000").command.levels, kMaxDepthLevels);
    CHECK(parse("DEPTH 10").ok());
}

TEST(DepthRejectsZeroNegativeHugeAndGarbage) {
    CHECK(rejects("depth", "missing argument"));
    CHECK(rejects("depth 1 2", "too many arguments"));
    CHECK(rejects("depth 0", "between 1 and"));
    CHECK(rejects("depth 1001", "between 1 and"));
    CHECK(rejects("depth 4000000000", "between 1 and"));
    CHECK(rejects("depth -3", "LEVELS"));
    CHECK(rejects("depth abc", "LEVELS"));
    CHECK(rejects("depth 2.5", "LEVELS"));
}

// ---- Argument-less commands -------------------------------------------------------------------------------------------

TEST(ArgumentlessCommandsRejectExtraArguments) {
    for (const char* word : {"book", "best", "recover", "help", "exit", "quit"}) {
        CHECK(rejects(std::string(word) + " x", "too many arguments"));
    }
}

// ---- Very long and hostile input --------------------------------------------------------------------------------------------

TEST(OverlongLinesAreRejectedNotProcessed) {
    const std::string longLine(kMaxLineLength + 1, 'a');
    CHECK(rejects(longLine, "too long"));
    CHECK(rejects("submit BUY 1 1 " + std::string(5000, '7'), "too long"));
    // Exactly at the limit is still fine.
    std::string atLimit = "book";
    atLimit.append(kMaxLineLength - atLimit.size(), ' ');
    CHECK_EQ(atLimit.size(), kMaxLineLength);
    CHECK(parse(atLimit).ok());
}

TEST(LongTokensAreEchoedBounded) {
    const ParseResult r = parse("submit BUY " + std::string(900, '9') + "x 5");
    CHECK(!r.ok());
    CHECK(r.error.size() < 200);  // the offending token is not dumped in full
}

TEST(EmbeddedControlAndNonAsciiCharactersDoNotCrash) {
    const std::string weird = std::string("submit BUY 1\x01 5");
    CHECK(!parse(weird).ok());
    CHECK(!parse("submit BUY \xC3\xA9 5").ok());
    CHECK(!parse(std::string("sub\0mit", 7)).ok());
    CHECK(parse("\t\vbook\f").ok());
}

TEST(ParseNeverThrowsOnAssortedGarbage) {
    const std::vector<std::string> garbage = {
        "-", "--", "---", "...", ".", "0.", ".5", "1..2", "9999999999999999999999", "-0", "+",
        "submit - - -", "submit BUY - -", "modify - - -", "depth -", "cancel -", "\"quoted\"",
        "submit BUY 1 1 1 1 1 1 1 1", ";;;", "book;book", "book && quit", "\\", "%s%s%s", "{}"};
    for (const std::string& g : garbage) {
        const ParseResult r = parse(g);
        CHECK(r.ok() || !r.error.empty());
    }
}

TEST_MAIN()
