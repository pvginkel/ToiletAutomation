// A stand-in until the firmware has pure logic: it proves that the host test build runs, in
// C++20. The MotionFilter tests replace it in step 4 (docs/high-level-design.md, section 11).

#include <gtest/gtest.h>

TEST(SmokeTest, BuildsAsCpp20) { EXPECT_GE(__cplusplus, 202002L); }
