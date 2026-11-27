// MotionFilter: the PIR's edges in, motion out (docs/high-level-design.md, section 6).

#include <gtest/gtest.h>

#include <vector>

#include "motion/MotionFilter.h"

namespace {

using Kind = MotionFilter::EventKind;

// The shortest pulse the HC-SR602 gave on the bench (docs/hardware-bring-up.md, "The PIR").
constexpr int64_t SHORTEST_REAL_PULSE_MS = 1700;

// A filter that records what its listener hears.
struct Recorded {
    std::vector<MotionFilter::Event> events;
    MotionFilter filter;

    explicit Recorded(MotionFilter::Settings settings)
        : filter(settings, [this](const MotionFilter::Event& event) { events.push_back(event); }) {}

    // The kinds heard since the last call.
    std::vector<Kind> take() {
        std::vector<Kind> kinds;
        for (const auto& event : events) {
            kinds.push_back(event.kind);
        }
        events.clear();
        return kinds;
    }

    void pulse(int64_t rise_at, int64_t high_ms) {
        filter.edge(true, rise_at);
        filter.tick(rise_at + high_ms / 2);
        filter.edge(false, rise_at + high_ms);
    }
};

void expect_counters(const MotionFilter& filter, uint32_t edges, uint32_t accepted, uint32_t rejected,
                     uint32_t ignored = 0) {
    EXPECT_EQ(filter.get_counters().ignored, ignored);
    EXPECT_EQ(filter.get_counters().edges, edges);
    EXPECT_EQ(filter.get_counters().accepted, accepted);
    EXPECT_EQ(filter.get_counters().rejected, rejected);
}

TEST(MotionFilterTest, IsOffAtStart) {
    Recorded recorded({.hold_off_ms = 0});

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_FALSE(recorded.filter.get_level());

    recorded.filter.tick(60 * 1000);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_TRUE(recorded.take().empty());
    expect_counters(recorded.filter, 0, 0, 0);
}

TEST(MotionFilterTest, TheShortestRealPulsePassesAtTheDefaults) {
    Recorded recorded({.hold_off_ms = 0});

    recorded.filter.edge(true, 1000);

    EXPECT_TRUE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_HIGH, Kind::PULSE_ACCEPTED}));

    recorded.filter.tick(1000 + SHORTEST_REAL_PULSE_MS - 1);
    EXPECT_TRUE(recorded.filter.is_motion());

    recorded.filter.edge(false, 1000 + SHORTEST_REAL_PULSE_MS);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.events.back().high_ms, SHORTEST_REAL_PULSE_MS);
    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_LOW, Kind::MOTION_OFF}));
    expect_counters(recorded.filter, 2, 1, 0);
}

TEST(MotionFilterTest, TheShortestRealPulsePassesAtTheLargestMinimum) {
    Recorded recorded({.min_pulse_ms = 1500, .hold_off_ms = 0});

    recorded.filter.edge(true, 0);
    recorded.filter.tick(1499);

    EXPECT_FALSE(recorded.filter.is_motion());

    recorded.filter.tick(1500);

    EXPECT_TRUE(recorded.filter.is_motion());

    recorded.filter.edge(false, SHORTEST_REAL_PULSE_MS);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.take(),
              (std::vector<Kind>{Kind::PIR_HIGH, Kind::PULSE_ACCEPTED, Kind::PIR_LOW, Kind::MOTION_OFF}));
    expect_counters(recorded.filter, 2, 1, 0);
}

TEST(MotionFilterTest, APulseShorterThanTheMinimumIsRejected) {
    Recorded recorded({.min_pulse_ms = 1000, .hold_off_ms = 0});

    recorded.filter.edge(true, 0);
    recorded.filter.tick(999);
    recorded.filter.edge(false, 999);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.events.back().high_ms, 999);
    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_HIGH, Kind::PIR_LOW, Kind::PULSE_TOO_SHORT}));
    expect_counters(recorded.filter, 2, 0, 1);
}

TEST(MotionFilterTest, APulseThatFallsAtTheMinimumBeforeATickIsAccepted) {
    Recorded recorded({.min_pulse_ms = 1000, .hold_off_ms = 0});

    recorded.filter.edge(true, 0);
    recorded.filter.edge(false, 1000);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.take(),
              (std::vector<Kind>{Kind::PIR_HIGH, Kind::PULSE_ACCEPTED, Kind::PIR_LOW, Kind::MOTION_OFF}));
    expect_counters(recorded.filter, 2, 1, 0);
}

TEST(MotionFilterTest, TwoPulsesWithTheSecondInsideTheWindow) {
    Recorded recorded({.pulse_count = 2, .pulse_window_ms = 10 * 1000, .hold_off_ms = 0});

    recorded.pulse(0, SHORTEST_REAL_PULSE_MS);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.events[1].pulses, 1);
    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_HIGH, Kind::PULSE_TOO_FEW, Kind::PIR_LOW}));

    // The window counts from rising edge to rising edge, and includes its end.
    recorded.filter.edge(true, 10 * 1000);

    EXPECT_TRUE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.events.back().pulses, 2);
    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_HIGH, Kind::PULSE_ACCEPTED}));
    expect_counters(recorded.filter, 3, 1, 1);

    // Motion that goes on keeps the pulses coming, and each is accepted at its rising edge.
    recorded.filter.edge(false, 10 * 1000 + SHORTEST_REAL_PULSE_MS);
    recorded.filter.edge(true, 15 * 1000);

    EXPECT_TRUE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.take(),
              (std::vector<Kind>{Kind::PIR_LOW, Kind::MOTION_OFF, Kind::PIR_HIGH, Kind::PULSE_ACCEPTED}));
    expect_counters(recorded.filter, 5, 2, 1);
}

TEST(MotionFilterTest, TwoPulsesWithTheSecondOutsideTheWindow) {
    Recorded recorded({.pulse_count = 2, .pulse_window_ms = 10 * 1000, .hold_off_ms = 0});

    recorded.pulse(0, SHORTEST_REAL_PULSE_MS);
    recorded.pulse(10 * 1000 + 1, SHORTEST_REAL_PULSE_MS);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_HIGH, Kind::PULSE_TOO_FEW, Kind::PIR_LOW, Kind::PIR_HIGH,
                                                  Kind::PULSE_TOO_FEW, Kind::PIR_LOW}));
    expect_counters(recorded.filter, 4, 0, 2);

    // The window slides: a third pulse inside the second's window is accepted.
    recorded.filter.edge(true, 15 * 1000);

    EXPECT_TRUE(recorded.filter.is_motion());
    expect_counters(recorded.filter, 5, 1, 2);
}

TEST(MotionFilterTest, APulseTooShortDoesNotCountTowardTheWindow) {
    Recorded recorded({.min_pulse_ms = 1000, .pulse_count = 2, .pulse_window_ms = 10 * 1000, .hold_off_ms = 0});

    recorded.pulse(0, 500);
    recorded.pulse(5 * 1000, SHORTEST_REAL_PULSE_MS);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_HIGH, Kind::PIR_LOW, Kind::PULSE_TOO_SHORT, Kind::PIR_HIGH,
                                                  Kind::PULSE_TOO_FEW, Kind::PIR_LOW}));
    expect_counters(recorded.filter, 4, 0, 2);
}

TEST(MotionFilterTest, ARetriggerKeepsMotionOn) {
    Recorded recorded({.min_pulse_ms = 1000, .hold_off_ms = 0});

    recorded.filter.edge(true, 0);
    recorded.filter.tick(1000);

    EXPECT_TRUE(recorded.filter.is_motion());

    // The HC-SR602 holds its output high while the motion goes on: 4.8 s on the bench.
    for (int64_t now = 1000; now < 4800; now += 10) {
        recorded.filter.tick(now);
        ASSERT_TRUE(recorded.filter.is_motion());
    }

    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_HIGH, Kind::PULSE_ACCEPTED}));
    expect_counters(recorded.filter, 1, 1, 0);
}

TEST(MotionFilterTest, MotionGoesOffWhenThePirFalls) {
    Recorded recorded({.hold_off_ms = 0});

    recorded.filter.edge(true, 0);
    recorded.filter.tick(60 * 60 * 1000);

    EXPECT_TRUE(recorded.filter.is_motion());

    recorded.filter.edge(false, 60 * 60 * 1000 + 1);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.events.back().at, 60 * 60 * 1000 + 1);
    EXPECT_EQ(recorded.take(),
              (std::vector<Kind>{Kind::PIR_HIGH, Kind::PULSE_ACCEPTED, Kind::PIR_LOW, Kind::MOTION_OFF}));

    recorded.filter.tick(2 * 60 * 60 * 1000);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_TRUE(recorded.take().empty());
}

TEST(MotionFilterTest, AnEdgeWithTheLevelUnchangedIsAGlitch) {
    Recorded recorded({.hold_off_ms = 0});

    recorded.filter.edge(false, 1000);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_GLITCH}));

    recorded.filter.edge(true, 2000);
    recorded.filter.edge(true, 2001);

    EXPECT_TRUE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_HIGH, Kind::PULSE_ACCEPTED, Kind::PIR_GLITCH}));
    EXPECT_EQ(recorded.filter.get_counters().glitches, 2u);
    expect_counters(recorded.filter, 1, 1, 0);
}

TEST(MotionFilterTest, APulseInsideTheHoldOffIsIgnored) {
    Recorded recorded({});

    recorded.pulse(1228, 1753);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.events[1].at, 1228);
    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_HIGH, Kind::PULSE_IGNORED, Kind::PIR_LOW}));
    expect_counters(recorded.filter, 2, 0, 0, 1);
}

TEST(MotionFilterTest, APulseStraddlingTheEndOfTheHoldOffIsIgnored) {
    Recorded recorded({});

    recorded.filter.edge(true, 9000);
    recorded.filter.tick(10 * 1000);
    recorded.filter.tick(12 * 1000);

    EXPECT_FALSE(recorded.filter.is_motion());

    recorded.filter.edge(false, 13 * 1000);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_HIGH, Kind::PULSE_IGNORED, Kind::PIR_LOW}));
    expect_counters(recorded.filter, 2, 0, 0, 1);
}

TEST(MotionFilterTest, TheFirstPulseAfterTheHoldOffIsAccepted) {
    Recorded recorded({});

    recorded.pulse(1228, 1753);
    recorded.take();
    recorded.filter.edge(true, 10 * 1000);

    EXPECT_TRUE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_HIGH, Kind::PULSE_ACCEPTED}));

    recorded.filter.edge(false, 12 * 1000);

    EXPECT_FALSE(recorded.filter.is_motion());
    expect_counters(recorded.filter, 4, 1, 0, 1);
}

TEST(MotionFilterTest, AHeldOffPulseDoesNotCountTowardThePulseCount) {
    Recorded recorded({.pulse_count = 2});

    recorded.pulse(1228, 1753);
    recorded.pulse(11 * 1000, SHORTEST_REAL_PULSE_MS);

    EXPECT_FALSE(recorded.filter.is_motion());
    EXPECT_EQ(recorded.take(), (std::vector<Kind>{Kind::PIR_HIGH, Kind::PULSE_IGNORED, Kind::PIR_LOW, Kind::PIR_HIGH,
                                                  Kind::PULSE_TOO_FEW, Kind::PIR_LOW}));
    expect_counters(recorded.filter, 4, 0, 1, 1);

    recorded.filter.edge(true, 15 * 1000);

    EXPECT_TRUE(recorded.filter.is_motion());
    expect_counters(recorded.filter, 5, 1, 1, 1);
}

}  // namespace
