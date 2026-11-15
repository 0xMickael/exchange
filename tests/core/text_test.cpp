#include "exchange/core/text.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace exchange {
namespace {

TEST(Text, CommandsSurviveARoundTrip) {
    const std::vector<Command> commands = {
        NewOrder{
            .session_id = 3, .client_order_id = 1, .side = Side::Buy, .price = 102, .quantity = 12},
        NewOrder{.session_id = 18446744073709551615ULL,
                 .client_order_id = 7,
                 .side = Side::Sell,
                 .price = -5,
                 .quantity = 0},
        CancelOrder{.session_id = 3, .order_id = 2},
        SessionClosed{.session_id = 3},
        SnapshotRequest{.session_id = 3},
    };

    for (const Command& command : commands) {
        const std::string line = to_string(command);
        EXPECT_EQ(parse_command(line), std::optional<Command>(command)) << line;
    }
}

TEST(Text, CommandTextForms) {
    EXPECT_EQ(to_string(Command{NewOrder{.session_id = 3,
                                         .client_order_id = 1,
                                         .side = Side::Buy,
                                         .price = 102,
                                         .quantity = 12}}),
              "new 3 1 buy 102 12");
    EXPECT_EQ(to_string(Command{CancelOrder{.session_id = 3, .order_id = 2}}), "cancel 3 2");
    EXPECT_EQ(to_string(Command{SessionClosed{.session_id = 3}}), "close 3");
}

TEST(Text, ParsingAcceptsExtraWhitespace) {
    EXPECT_EQ(parse_command("  cancel\t 3   2  "),
              std::optional<Command>(CancelOrder{.session_id = 3, .order_id = 2}));
}

TEST(Text, ParsingRejectsMalformedLines) {
    const std::vector<std::string> lines = {
        "",
        "   ",
        "bogus 1",
        "new",
        "new 1 1 buy 100",
        "new 1 1 buy 100 5 extra",
        "new 1 1 hold 100 5",
        "new 1 1 BUY 100 5",
        "new -1 1 buy 100 5",
        "new 1 1 buy 100 -5",
        "new 1 1 buy 1.5 5",
        "new 1 1 buy 100 5x",
        "new 1 1 buy 100 99999999999999999999999",
        "cancel 1",
        "cancel 1 x",
        "cancel 1 2 3",
        "close",
        "close 1 2",
        "close one",
        "snapshot",
        "snapshot 1 2",
    };

    for (const std::string& line : lines) {
        EXPECT_FALSE(parse_command(line).has_value()) << "'" << line << "'";
    }
}

} // namespace
} // namespace exchange
