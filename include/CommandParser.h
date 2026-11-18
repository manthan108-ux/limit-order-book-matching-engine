#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "OrderTypes.h"

namespace lob {

// Terminal-independent command parsing. This layer knows the command syntax and
// nothing else: no I/O, no OrderManager. It turns one line of text into a
// ParsedCommand or an error message, so it can be tested without a terminal.
//
// Conventions
//  * Prices are integer ticks and quantities are integer units, exactly as the
//    engine uses them. Decimal prices are rejected, never rounded.
//  * Command words and sides are case-insensitive (SUBMIT, Submit, buy, BUY).
//  * Any run of spaces/tabs separates tokens; leading and trailing whitespace is
//    ignored.
//  * Numbers must be whole tokens in plain decimal (optional leading '-'): no
//    '+', no exponent, no trailing characters, and they must fit the target type.
//    Nothing is truncated or wrapped.
//  * Values that can never be valid (non-positive price or quantity, id 0) are
//    rejected here with a clear message, so they never reach the OrderManager and
//    never consume an order id. Everything else (duplicate ids, unknown orders,
//    crossing rules, ...) is the OrderManager's decision, not the parser's.

inline constexpr std::size_t kMaxLineLength = 1024;
inline constexpr std::size_t kMaxDepthLevels = 1000;

enum class CommandType {
    Empty,    // a blank line: not an error, nothing to do
    Submit,   // submit <BUY|SELL> <PRICE> <QUANTITY> [ID]
    Cancel,   // cancel <ORDER_ID>
    Modify,   // modify <ORDER_ID> <NEW_PRICE> <NEW_REMAINING_QUANTITY>
    Book,     // book
    Depth,    // depth <LEVELS>
    Order,    // order <ORDER_ID>
    Trades,   // trades <ORDER_ID>
    Best,     // best
    Recover,  // recover
    Help,     // help
    Exit      // exit | quit
};

struct ParsedCommand {
    CommandType type = CommandType::Empty;
    Side side = Side::Buy;            // Submit
    Price price = 0;                  // Submit, Modify (ticks)
    Quantity quantity = 0;            // Submit (quantity), Modify (new REMAINING quantity)
    // Submit: the optional client-supplied id (absent = let the OrderManager generate
    // one). Cancel / Modify / Order / Trades: the target order id (always present).
    std::optional<OrderId> orderId;
    std::size_t levels = 0;           // Depth
};

struct ParseResult {
    ParsedCommand command;
    std::string error;  // empty on success
    bool ok() const noexcept { return error.empty(); }
};

// Splits on runs of whitespace. The views point into `line`, which must outlive them.
std::vector<std::string_view> tokenize(std::string_view line);

// Parses one input line. Never throws for malformed input; reports it in `error`.
ParseResult parseCommand(std::string_view line);

}  // namespace lob
