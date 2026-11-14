#include <gtest/gtest.h>

// Proves that the compiler, GoogleTest and CTest are wired together.
// To be replaced by the matching engine tests.
TEST(Toolchain, CompilesAsCpp20) {
    EXPECT_GE(__cplusplus, 202002L);
}
