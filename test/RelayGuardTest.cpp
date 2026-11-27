// RelayGuard: the fan relay's fail-safe (docs/high-level-design.md, section 7).

#include <gtest/gtest.h>

#include "relay/RelayGuard.h"

namespace {

constexpr int64_t MAX_ON_MS = 60 * 1000;

TEST(RelayGuardTest, IsOffAtStart) {
    RelayGuard guard(MAX_ON_MS);

    EXPECT_FALSE(guard.is_on());
    EXPECT_EQ(guard.get_on_time_left(0), 0);
    EXPECT_FALSE(guard.tick(10 * MAX_ON_MS));
    EXPECT_FALSE(guard.is_on());
}

TEST(RelayGuardTest, TurnsOnThenOff) {
    RelayGuard guard(MAX_ON_MS);

    EXPECT_TRUE(guard.set(true, 1000));
    EXPECT_TRUE(guard.is_on());

    EXPECT_TRUE(guard.set(false, 2000));
    EXPECT_FALSE(guard.is_on());
    EXPECT_EQ(guard.get_on_time_left(2000), 0);

    // Nothing is left for the fail-safe to do.
    EXPECT_FALSE(guard.tick(1000 + MAX_ON_MS));
    EXPECT_FALSE(guard.is_on());
}

TEST(RelayGuardTest, AnOffCommandWhileOffChangesNothing) {
    RelayGuard guard(MAX_ON_MS);

    EXPECT_FALSE(guard.set(false, 1000));
    EXPECT_FALSE(guard.is_on());
}

TEST(RelayGuardTest, TurnsOffAtExactlyTheMaximumAndNotEarlier) {
    RelayGuard guard(MAX_ON_MS);

    guard.set(true, 1000);

    EXPECT_FALSE(guard.tick(1000 + MAX_ON_MS - 1));
    EXPECT_TRUE(guard.is_on());
    EXPECT_EQ(guard.get_on_time_left(1000 + MAX_ON_MS - 1), 1);

    EXPECT_TRUE(guard.tick(1000 + MAX_ON_MS));
    EXPECT_FALSE(guard.is_on());
    EXPECT_EQ(guard.get_on_time_left(1000 + MAX_ON_MS), 0);
}

TEST(RelayGuardTest, AnOnCommandWhileOnKeepsTheDeadline) {
    RelayGuard guard(MAX_ON_MS);

    guard.set(true, 0);

    EXPECT_FALSE(guard.set(true, MAX_ON_MS / 2));
    EXPECT_EQ(guard.get_on_time_left(MAX_ON_MS / 2), MAX_ON_MS / 2);

    EXPECT_FALSE(guard.tick(MAX_ON_MS - 1));
    EXPECT_TRUE(guard.tick(MAX_ON_MS));
    EXPECT_FALSE(guard.is_on());
}

TEST(RelayGuardTest, OnAgainAfterTheFailSafeStartsANewCount) {
    RelayGuard guard(MAX_ON_MS);

    guard.set(true, 0);
    EXPECT_TRUE(guard.tick(MAX_ON_MS));

    const int64_t again = MAX_ON_MS + 5000;

    EXPECT_TRUE(guard.set(true, again));
    EXPECT_TRUE(guard.is_on());
    EXPECT_EQ(guard.get_on_time_left(again), MAX_ON_MS);

    EXPECT_FALSE(guard.tick(again + MAX_ON_MS - 1));
    EXPECT_TRUE(guard.is_on());
    EXPECT_TRUE(guard.tick(again + MAX_ON_MS));
    EXPECT_FALSE(guard.is_on());
}

TEST(RelayGuardTest, ANewMaximumAppliesToTheRunningCount) {
    RelayGuard guard(MAX_ON_MS);

    guard.set(true, 0);
    guard.set_max_on_ms(MAX_ON_MS / 2);

    EXPECT_EQ(guard.get_on_time_left(0), MAX_ON_MS / 2);
    EXPECT_TRUE(guard.tick(MAX_ON_MS / 2));
    EXPECT_FALSE(guard.is_on());
}

}  // namespace
