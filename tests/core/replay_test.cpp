#include "exchange/core/replay.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace exchange {
namespace {

const std::filesystem::path scenario_directory =
    std::filesystem::path(EXCHANGE_TEST_DATA_DIR) / "replay";

std::vector<std::string> lines_of(std::istream& stream) {
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

// Each scenario is a pair of files: NAME.commands is the input and NAME.events
// the expected output. The expected outputs were written by hand from the
// specification, not captured from the implementation.
TEST(Replay, ScenariosProduceTheExpectedEvents) {
    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenario_directory)) {
        if (entry.path().extension() == ".commands") {
            scenarios.push_back(entry.path());
        }
    }
    std::ranges::sort(scenarios);
    ASSERT_GE(scenarios.size(), 3U) << "no scenarios found in " << scenario_directory;

    for (const std::filesystem::path& commands_path : scenarios) {
        SCOPED_TRACE(commands_path.filename().string());
        std::filesystem::path events_path = commands_path;
        events_path.replace_extension(".events");

        std::ifstream commands(commands_path);
        std::ifstream expected(events_path);
        ASSERT_TRUE(commands.is_open());
        ASSERT_TRUE(expected.is_open()) << "missing " << events_path;

        std::stringstream actual;
        const ReplayResult result = replay(commands, actual);

        EXPECT_EQ(result.error_line, 0U);
        EXPECT_EQ(lines_of(actual), lines_of(expected));
    }
}

TEST(Replay, IgnoresCommentsAndEmptyLines) {
    std::istringstream input("# a comment\n"
                             "\n"
                             "   \n"
                             "  # an indented comment\n"
                             "new 1 1 buy 100 5\r\n");
    std::ostringstream output;

    const ReplayResult result = replay(input, output);

    EXPECT_EQ(result.error_line, 0U);
    EXPECT_EQ(result.commands, 1U);
    EXPECT_EQ(result.events, 2U);
    EXPECT_EQ(output.str(), "> new 1 1 buy 100 5\n"
                            "accepted session=1 client_order_id=1 order_id=1\n"
                            "book side=buy price=100 total=5\n");
}

TEST(Replay, StopsAtTheFirstInvalidLine) {
    std::istringstream input("new 1 1 buy 100 5\n"
                             "# comment\n"
                             "new 1 2 buy\n"
                             "new 1 3 buy 100 5\n");
    std::ostringstream output;

    const ReplayResult result = replay(input, output);

    EXPECT_EQ(result.error_line, 3U);
    EXPECT_EQ(result.commands, 1U);
}

TEST(Replay, SameInputProducesTheSameOutput) {
    const std::string script = "new 1 1 sell 101 5\n"
                               "new 2 1 buy 101 3\n"
                               "cancel 1 1\n"
                               "close 2\n";
    std::istringstream first_input(script);
    std::istringstream second_input(script);
    std::ostringstream first_output;
    std::ostringstream second_output;

    replay(first_input, first_output);
    replay(second_input, second_output);

    EXPECT_FALSE(first_output.str().empty());
    EXPECT_EQ(first_output.str(), second_output.str());
}

} // namespace
} // namespace exchange
