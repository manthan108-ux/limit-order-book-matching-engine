#include "CommandParser.h"

#include <charconv>
#include <system_error>

namespace lob {

namespace {

bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

std::string lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// A token as shown back to the user: bounded, so an absurdly long token cannot
// flood the terminal.
std::string shown(std::string_view token) {
    constexpr std::size_t kMaxShown = 32;
    std::string s = "'";
    s.append(token.substr(0, kMaxShown));
    if (token.size() > kMaxShown) s += "...";
    s += "'";
    return s;
}

enum class NumberStatus { Ok, Invalid, OutOfRange, Decimal };

// Strict whole-token integer parse via std::from_chars (no locale, no exceptions,
// no partial parses, no wrap-around).
template <typename T>
NumberStatus parseWhole(std::string_view token, T& out) {
    if (token.empty()) return NumberStatus::Invalid;
    const char* const first = token.data();
    const char* const last = token.data() + token.size();
    T value{};
    const std::from_chars_result r = std::from_chars(first, last, value);
    if (r.ec == std::errc{}) {
        if (r.ptr == last) {
            out = value;
            return NumberStatus::Ok;
        }
        // "12.5": digits, a point, digits. Say so, rather than "invalid".
        if (*r.ptr == '.' && r.ptr + 1 != last) {
            bool allDigits = true;
            for (const char* p = r.ptr + 1; p != last; ++p) allDigits = allDigits && isDigit(*p);
            if (allDigits) return NumberStatus::Decimal;
        }
        return NumberStatus::Invalid;
    }
    if (r.ec == std::errc::result_out_of_range) {
        // Only a genuinely numeric token is "out of range"; "99999999999999999999x" is invalid.
        const char* p = first;
        if (*p == '-') ++p;
        if (p == last) return NumberStatus::Invalid;
        for (; p != last; ++p) {
            if (!isDigit(*p)) return NumberStatus::Invalid;
        }
        return NumberStatus::OutOfRange;
    }
    return NumberStatus::Invalid;
}

// Each helper returns an empty string on success, or the error message.

std::string parsePrice(std::string_view token, Price& out) {
    switch (parseWhole(token, out)) {
        case NumberStatus::Ok:
            if (!isValidPrice(out)) {
                return "PRICE must be a positive whole number of ticks (got " + std::to_string(out) + ")";
            }
            return {};
        case NumberStatus::Decimal:
            return "PRICE " + shown(token) +
                   " is not a whole number: prices are integer ticks and are never rounded";
        case NumberStatus::OutOfRange:
            return "PRICE " + shown(token) + " is out of range";
        case NumberStatus::Invalid:
            break;
    }
    return "PRICE " + shown(token) + " is not a valid whole number of ticks";
}

std::string parseQuantity(std::string_view token, const char* what, bool zeroMeansCancel, Quantity& out) {
    switch (parseWhole(token, out)) {
        case NumberStatus::Ok:
            if (!isValidQuantity(out)) {
                if (out == 0 && zeroMeansCancel) {
                    return std::string(what) + " must be positive; to remove an order use 'cancel <ORDER_ID>'";
                }
                return std::string(what) + " must be a positive whole number of units (got " +
                       std::to_string(out) + ")";
            }
            return {};
        case NumberStatus::Decimal:
            return std::string(what) + " " + shown(token) + " is not a whole number of units";
        case NumberStatus::OutOfRange:
            return std::string(what) + " " + shown(token) + " is out of range";
        case NumberStatus::Invalid:
            break;
    }
    return std::string(what) + " " + shown(token) + " is not a valid whole number of units";
}

std::string parseOrderId(std::string_view token, OrderId& out) {
    switch (parseWhole(token, out)) {
        case NumberStatus::Ok:
            if (out == 0) return "ORDER_ID must be a positive whole number (0 is not a valid id)";
            return {};
        case NumberStatus::OutOfRange:
            return "ORDER_ID " + shown(token) + " is out of range";
        case NumberStatus::Decimal:
        case NumberStatus::Invalid:
            break;
    }
    return "ORDER_ID " + shown(token) + " is not a valid positive whole number";
}

std::string parseSide(std::string_view token, Side& out) {
    const std::string s = lowered(token);
    if (s == "buy") {
        out = Side::Buy;
        return {};
    }
    if (s == "sell") {
        out = Side::Sell;
        return {};
    }
    return "SIDE " + shown(token) + " is not valid: use BUY or SELL";
}

ParseResult fail(std::string message) {
    ParseResult r;
    r.error = std::move(message);
    return r;
}

// Checks the number of arguments (tokens after the command word).
std::string checkArity(std::size_t have, std::size_t min, std::size_t max, const char* usage) {
    if (have < min) return std::string("missing argument(s). usage: ") + usage;
    if (have > max) return std::string("too many arguments. usage: ") + usage;
    return {};
}

}  // namespace

std::vector<std::string_view> tokenize(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && isSpace(line[i])) ++i;
        const std::size_t start = i;
        while (i < line.size() && !isSpace(line[i])) ++i;
        if (i > start) tokens.push_back(line.substr(start, i - start));
    }
    return tokens;
}

ParseResult parseCommand(std::string_view line) {
    if (line.size() > kMaxLineLength) {
        return fail("input line too long (" + std::to_string(line.size()) + " characters; the maximum is " +
                    std::to_string(kMaxLineLength) + ")");
    }
    const std::vector<std::string_view> tokens = tokenize(line);
    ParseResult result;
    if (tokens.empty()) return result;  // CommandType::Empty

    const std::string word = lowered(tokens[0]);
    const std::size_t args = tokens.size() - 1;
    ParsedCommand& cmd = result.command;

    if (word == "submit") {
        constexpr const char* kUsage = "submit <BUY|SELL> <PRICE> <QUANTITY> [ID]";
        if (std::string e = checkArity(args, 3, 4, kUsage); !e.empty()) return fail(std::move(e));
        cmd.type = CommandType::Submit;
        if (std::string e = parseSide(tokens[1], cmd.side); !e.empty()) return fail(std::move(e));
        if (std::string e = parsePrice(tokens[2], cmd.price); !e.empty()) return fail(std::move(e));
        if (std::string e = parseQuantity(tokens[3], "QUANTITY", false, cmd.quantity); !e.empty()) {
            return fail(std::move(e));
        }
        if (args == 4) {
            OrderId id = 0;
            if (std::string e = parseOrderId(tokens[4], id); !e.empty()) return fail(std::move(e));
            cmd.orderId = id;
        }
        return result;
    }
    if (word == "cancel" || word == "order" || word == "trades") {
        const std::string usage = word + " <ORDER_ID>";
        if (std::string e = checkArity(args, 1, 1, usage.c_str()); !e.empty()) return fail(std::move(e));
        cmd.type = word == "cancel" ? CommandType::Cancel
                   : word == "order" ? CommandType::Order
                                     : CommandType::Trades;
        OrderId id = 0;
        if (std::string e = parseOrderId(tokens[1], id); !e.empty()) return fail(std::move(e));
        cmd.orderId = id;
        return result;
    }
    if (word == "modify") {
        constexpr const char* kUsage = "modify <ORDER_ID> <NEW_PRICE> <NEW_REMAINING_QUANTITY>";
        if (std::string e = checkArity(args, 3, 3, kUsage); !e.empty()) return fail(std::move(e));
        cmd.type = CommandType::Modify;
        OrderId id = 0;
        if (std::string e = parseOrderId(tokens[1], id); !e.empty()) return fail(std::move(e));
        cmd.orderId = id;
        if (std::string e = parsePrice(tokens[2], cmd.price); !e.empty()) return fail(std::move(e));
        if (std::string e = parseQuantity(tokens[3], "NEW_REMAINING_QUANTITY", true, cmd.quantity);
            !e.empty()) {
            return fail(std::move(e));
        }
        return result;
    }
    if (word == "depth") {
        constexpr const char* kUsage = "depth <LEVELS>";
        if (std::string e = checkArity(args, 1, 1, kUsage); !e.empty()) return fail(std::move(e));
        cmd.type = CommandType::Depth;
        unsigned long long levels = 0;
        switch (parseWhole(tokens[1], levels)) {
            case NumberStatus::Ok:
                break;
            case NumberStatus::OutOfRange:
                return fail("LEVELS " + shown(tokens[1]) + " is out of range (maximum " +
                            std::to_string(kMaxDepthLevels) + ")");
            default:
                return fail("LEVELS " + shown(tokens[1]) + " is not a valid positive whole number");
        }
        if (levels < 1 || levels > kMaxDepthLevels) {
            return fail("LEVELS must be between 1 and " + std::to_string(kMaxDepthLevels) + " (got " +
                        std::to_string(levels) + ")");
        }
        cmd.levels = static_cast<std::size_t>(levels);
        return result;
    }

    struct NoArg {
        const char* name;
        CommandType type;
    };
    static constexpr NoArg kNoArg[] = {{"book", CommandType::Book},       {"best", CommandType::Best},
                                       {"recover", CommandType::Recover}, {"help", CommandType::Help},
                                       {"exit", CommandType::Exit},       {"quit", CommandType::Exit}};
    for (const NoArg& c : kNoArg) {
        if (word == c.name) {
            if (args != 0) return fail("too many arguments. usage: " + word);
            cmd.type = c.type;
            return result;
        }
    }
    return fail("unknown command " + shown(tokens[0]) + ". Type 'help' for the list of commands.");
}

}  // namespace lob
