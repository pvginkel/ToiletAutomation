// VocGate: the VOC index algorithm's samples in, the index Home Assistant sees out
// (docs/high-level-design.md, section 7).

#include <gtest/gtest.h>

#include "air/VocGate.h"

namespace {

// The algorithm's blackout: 45 s at 1 Hz, in which its index is 0.
constexpr int BLACKOUT_SAMPLES = 46;
constexpr int64_t BLACKOUT_END_MS = BLACKOUT_SAMPLES * 1000;

// A gate past its blackout, with the first index published at BLACKOUT_END_MS.
VocGate past_blackout(int32_t first_index) {
    VocGate gate;
    for (int i = 0; i < BLACKOUT_SAMPLES; i++) {
        gate.sample(0, true, i * 1000);
    }
    gate.sample(first_index, false, BLACKOUT_END_MS);
    return gate;
}

}  // namespace

TEST(VocGate, PublishesNothingDuringTheBlackout) {
    VocGate gate;

    for (int i = 0; i < BLACKOUT_SAMPLES; i++) {
        EXPECT_FALSE(gate.sample(i % 2 == 0 ? 0 : 100, true, i * 1000)) << "sample " << i;
    }

    EXPECT_FALSE(gate.get_published().has_value());
    EXPECT_FALSE(gate.get_measured().has_value());
}

TEST(VocGate, PublishesTheFirstIndexAfterTheBlackoutAtOnce) {
    VocGate gate;
    for (int i = 0; i < BLACKOUT_SAMPLES; i++) {
        gate.sample(0, true, i * 1000);
    }

    EXPECT_TRUE(gate.sample(97, false, BLACKOUT_END_MS));
    EXPECT_EQ(gate.get_published(), 97);
    EXPECT_EQ(gate.get_measured(), 97);
}

TEST(VocGate, PublishesNothingWhileTheIndexStands) {
    auto gate = past_blackout(100);

    for (int64_t at = BLACKOUT_END_MS + 1000; at <= BLACKOUT_END_MS + 60 * 1000; at += 1000) {
        EXPECT_FALSE(gate.sample(100, false, at)) << "at " << at;
    }
    EXPECT_EQ(gate.get_published(), 100);
}

TEST(VocGate, PublishesAChangeAtMostOnceEvery10Seconds) {
    auto gate = past_blackout(100);

    // A change within 10 s of the last publish waits.
    EXPECT_FALSE(gate.sample(101, false, BLACKOUT_END_MS + 1000));
    EXPECT_FALSE(gate.sample(104, false, BLACKOUT_END_MS + VocGate::MIN_INTERVAL_MS - 1));
    EXPECT_EQ(gate.get_published(), 100);
    EXPECT_EQ(gate.get_measured(), 104);

    // At 10 s it goes out, as the index then stands.
    EXPECT_TRUE(gate.sample(103, false, BLACKOUT_END_MS + VocGate::MIN_INTERVAL_MS));
    EXPECT_EQ(gate.get_published(), 103);

    // The next 10 s count from that publish.
    EXPECT_FALSE(gate.sample(120, false, BLACKOUT_END_MS + VocGate::MIN_INTERVAL_MS + 5000));
    EXPECT_TRUE(gate.sample(121, false, BLACKOUT_END_MS + 2 * VocGate::MIN_INTERVAL_MS));
    EXPECT_EQ(gate.get_published(), 121);
}

TEST(VocGate, DropsAChangeThatReturnsBeforeThe10SecondsAreOver) {
    auto gate = past_blackout(100);

    EXPECT_FALSE(gate.sample(101, false, BLACKOUT_END_MS + 1000));
    EXPECT_FALSE(gate.sample(100, false, BLACKOUT_END_MS + 2000));
    EXPECT_FALSE(gate.sample(100, false, BLACKOUT_END_MS + VocGate::MIN_INTERVAL_MS));
    EXPECT_EQ(gate.get_published(), 100);
}

TEST(VocGate, TheOverridePublishesAtOnceAndHoldsAgainstTheSamples) {
    auto gate = past_blackout(100);

    // At once, inside the 10 s of the first publish.
    EXPECT_TRUE(gate.set_override(250, BLACKOUT_END_MS + 1000));
    EXPECT_EQ(gate.get_published(), 250);
    EXPECT_EQ(gate.get_override(), 250);

    for (int64_t at = BLACKOUT_END_MS + 2000; at <= BLACKOUT_END_MS + 60 * 1000; at += 1000) {
        EXPECT_FALSE(gate.sample(110, false, at)) << "at " << at;
    }
    EXPECT_EQ(gate.get_published(), 250);
    EXPECT_EQ(gate.get_measured(), 110);

    // A new override replaces it, at once too.
    EXPECT_TRUE(gate.set_override(30, BLACKOUT_END_MS + 61 * 1000));
    EXPECT_EQ(gate.get_published(), 30);
}

TEST(VocGate, TheReleasePublishesTheMeasuredIndexAtOnce) {
    auto gate = past_blackout(100);
    gate.set_override(250, BLACKOUT_END_MS + 1000);
    gate.sample(112, false, BLACKOUT_END_MS + 2000);

    EXPECT_TRUE(gate.clear_override(BLACKOUT_END_MS + 3000));
    EXPECT_EQ(gate.get_published(), 112);
    EXPECT_FALSE(gate.get_override().has_value());

    // The samples are back, and the 10 s count from the release.
    EXPECT_FALSE(gate.sample(115, false, BLACKOUT_END_MS + 4000));
    EXPECT_TRUE(gate.sample(115, false, BLACKOUT_END_MS + 3000 + VocGate::MIN_INTERVAL_MS));
    EXPECT_EQ(gate.get_published(), 115);

    // A release without an override changes nothing.
    EXPECT_FALSE(gate.clear_override(BLACKOUT_END_MS + 30 * 1000));
}

TEST(VocGate, TheReleaseDuringTheBlackoutPublishesNothing) {
    VocGate gate;
    gate.sample(0, true, 0);

    EXPECT_TRUE(gate.set_override(250, 1000));
    gate.sample(0, true, 2000);
    EXPECT_EQ(gate.get_published(), 250);

    EXPECT_TRUE(gate.clear_override(3000));
    EXPECT_FALSE(gate.get_published().has_value());

    // The first index after the blackout still goes out at once.
    EXPECT_TRUE(gate.sample(100, false, 4000));
    EXPECT_EQ(gate.get_published(), 100);
}
