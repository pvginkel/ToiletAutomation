// The volume: the parse of Home Assistant's Volume number command, and the clamp to 0 to 1.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "config/Volume.h"

namespace {

TEST(VolumeTest, ParsesTheNumbersHomeAssistantSends) {
    EXPECT_EQ(parse_volume("0.35"), 0.35);
    EXPECT_EQ(parse_volume("0"), 0.0);
    EXPECT_EQ(parse_volume("1"), 1.0);
    EXPECT_EQ(parse_volume("1.0"), 1.0);
    EXPECT_EQ(parse_volume("0.15000000000000002"), 0.15000000000000002);
}

TEST(VolumeTest, ParsesAnExponent) { EXPECT_EQ(parse_volume("5e-1"), 0.5); }

TEST(VolumeTest, AllowsSpaceAround) {
    EXPECT_EQ(parse_volume(" 0.5"), 0.5);
    EXPECT_EQ(parse_volume("0.5\n"), 0.5);
    EXPECT_EQ(parse_volume("\t0.5 \r\n"), 0.5);
}

TEST(VolumeTest, ParsesANumberOutOfRangeForTheClamp) {
    EXPECT_EQ(parse_volume("1.5"), 1.5);
    EXPECT_EQ(parse_volume("-0.2"), -0.2);
}

TEST(VolumeTest, RefusesWhatIsNotANumber) {
    EXPECT_EQ(parse_volume(""), std::nullopt);
    EXPECT_EQ(parse_volume("   "), std::nullopt);
    EXPECT_EQ(parse_volume("loud"), std::nullopt);
    EXPECT_EQ(parse_volume("true"), std::nullopt);
    EXPECT_EQ(parse_volume("0.5 dB"), std::nullopt);
    EXPECT_EQ(parse_volume("0.5.1"), std::nullopt);
    EXPECT_EQ(parse_volume("0 .5"), std::nullopt);
    EXPECT_EQ(parse_volume("\"0.5\""), std::nullopt);
    EXPECT_EQ(parse_volume("+0.5"), std::nullopt);
    EXPECT_EQ(parse_volume("0x1"), std::nullopt);
}

TEST(VolumeTest, RefusesANumberThatIsNotFinite) {
    EXPECT_EQ(parse_volume("nan"), std::nullopt);
    EXPECT_EQ(parse_volume("inf"), std::nullopt);
    EXPECT_EQ(parse_volume("-inf"), std::nullopt);
    EXPECT_EQ(parse_volume("1e999"), std::nullopt);
}

TEST(VolumeTest, ClampKeepsTheRange) {
    EXPECT_EQ(clamp_volume(0.0), 0.0);
    EXPECT_EQ(clamp_volume(0.35), 0.35);
    EXPECT_EQ(clamp_volume(1.0), 1.0);
}

TEST(VolumeTest, ClampHoldsTheEnds) {
    EXPECT_EQ(clamp_volume(-0.2), 0.0);
    EXPECT_EQ(clamp_volume(1.5), 1.0);
    EXPECT_EQ(clamp_volume(-std::numeric_limits<double>::infinity()), 0.0);
    EXPECT_EQ(clamp_volume(std::numeric_limits<double>::infinity()), 1.0);
}

TEST(VolumeTest, ClampMutesWhatIsNotANumber) { EXPECT_EQ(clamp_volume(std::nan("")), 0.0); }

TEST(VolumeTest, ParseThenClamp) {
    EXPECT_EQ(clamp_volume(*parse_volume("1.5")), 1.0);
    EXPECT_EQ(clamp_volume(*parse_volume("-0.2")), 0.0);
    EXPECT_EQ(clamp_volume(*parse_volume("0.2")), 0.2);
}

}  // namespace
