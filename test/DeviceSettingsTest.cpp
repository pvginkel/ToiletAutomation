// DeviceSettings: the app's fields of the device configuration, each with its default and range
// (docs/firmware-plan.md, "Device configuration").

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "config/DeviceSettings.h"

namespace {

using Value = DeviceSettings::Value;

// What parse makes of a device configuration, given as a map from field to value. A field the map
// does not have is missing.
struct Parsed {
    DeviceSettings settings;
    std::vector<std::string> rejected;
};

Parsed parse(const std::map<std::string, Value>& fields) {
    Parsed parsed;

    parsed.settings = DeviceSettings::parse(
        [&fields](const char* name) {
            const auto it = fields.find(name);
            return it == fields.end() ? Value::missing() : it->second;
        },
        [&parsed](const std::string& message) { parsed.rejected.push_back(message); });

    return parsed;
}

void expect_defaults(const DeviceSettings& settings) {
    EXPECT_EQ(settings.fan_max_on_minutes, 60);
    EXPECT_EQ(settings.motion_min_pulse_ms, 0);
    EXPECT_EQ(settings.motion_pulse_count, 1);
    EXPECT_EQ(settings.motion_pulse_window_seconds, 10);
    EXPECT_DOUBLE_EQ(settings.volume_default, 0.5);
    EXPECT_EQ(settings.audio_udp_port, 11106);
}

TEST(DeviceSettingsTest, MissingFieldsTakeTheirDefaults) {
    const auto parsed = parse({});

    expect_defaults(parsed.settings);
    EXPECT_TRUE(parsed.rejected.empty());
}

TEST(DeviceSettingsTest, ValidFieldsAreTaken) {
    const auto parsed = parse({
        {"fanMaxOnMinutes", Value::of(1)},
        {"motionMinPulseMs", Value::of(1500)},
        {"motionPulseCount", Value::of(3)},
        {"motionPulseWindowSeconds", Value::of(60)},
        {"volumeDefault", Value::of(0.25)},
        {"audioUdpPort", Value::of(1024)},
    });

    const auto& settings = parsed.settings;

    EXPECT_EQ(settings.fan_max_on_minutes, 1);
    EXPECT_EQ(settings.motion_min_pulse_ms, 1500);
    EXPECT_EQ(settings.motion_pulse_count, 3);
    EXPECT_EQ(settings.motion_pulse_window_seconds, 60);
    EXPECT_DOUBLE_EQ(settings.volume_default, 0.25);
    EXPECT_EQ(settings.audio_udp_port, 1024);
    EXPECT_TRUE(parsed.rejected.empty());
}

TEST(DeviceSettingsTest, TheOtherEndsOfTheRangesAreTaken) {
    const auto parsed = parse({
        {"fanMaxOnMinutes", Value::of(240)},
        {"motionMinPulseMs", Value::of(0)},
        {"motionPulseCount", Value::of(1)},
        {"motionPulseWindowSeconds", Value::of(1)},
        {"volumeDefault", Value::of(1)},
        {"audioUdpPort", Value::of(65535)},
    });

    const auto& settings = parsed.settings;

    EXPECT_EQ(settings.fan_max_on_minutes, 240);
    EXPECT_EQ(settings.motion_min_pulse_ms, 0);
    EXPECT_EQ(settings.motion_pulse_count, 1);
    EXPECT_EQ(settings.motion_pulse_window_seconds, 1);
    EXPECT_DOUBLE_EQ(settings.volume_default, 1);
    EXPECT_EQ(settings.audio_udp_port, 65535);
    EXPECT_TRUE(parsed.rejected.empty());
}

TEST(DeviceSettingsTest, FieldsOfTheWrongTypeTakeTheirDefaults) {
    const auto parsed = parse({
        {"fanMaxOnMinutes", Value::other()},
        {"motionMinPulseMs", Value::other()},
        {"motionPulseCount", Value::other()},
        {"motionPulseWindowSeconds", Value::other()},
        {"volumeDefault", Value::other()},
        {"audioUdpPort", Value::other()},
    });

    expect_defaults(parsed.settings);
    ASSERT_EQ(parsed.rejected.size(), 6u);
    EXPECT_EQ(parsed.rejected[0], "fanMaxOnMinutes is not a number; taking the default 60");
}

TEST(DeviceSettingsTest, FractionsInIntegerFieldsTakeTheirDefaults) {
    const auto parsed = parse({
        {"fanMaxOnMinutes", Value::of(1.5)},
        {"motionMinPulseMs", Value::of(0.5)},
        {"motionPulseCount", Value::of(2.5)},
        {"motionPulseWindowSeconds", Value::of(9.9)},
        {"audioUdpPort", Value::of(11106.1)},
    });

    expect_defaults(parsed.settings);
    ASSERT_EQ(parsed.rejected.size(), 5u);
    EXPECT_EQ(parsed.rejected[0], "fanMaxOnMinutes is 1.5, not an integer; taking the default 60");
}

TEST(DeviceSettingsTest, FieldsBelowTheirRangeTakeTheirDefaults) {
    const auto parsed = parse({
        {"fanMaxOnMinutes", Value::of(0)},
        {"motionMinPulseMs", Value::of(-1)},
        {"motionPulseCount", Value::of(0)},
        {"motionPulseWindowSeconds", Value::of(0)},
        {"volumeDefault", Value::of(-0.1)},
        {"audioUdpPort", Value::of(1023)},
    });

    expect_defaults(parsed.settings);
    EXPECT_EQ(parsed.rejected.size(), 6u);
}

TEST(DeviceSettingsTest, FieldsAboveTheirRangeTakeTheirDefaults) {
    const auto parsed = parse({
        {"fanMaxOnMinutes", Value::of(241)},
        {"motionMinPulseMs", Value::of(1501)},
        {"motionPulseCount", Value::of(4)},
        {"motionPulseWindowSeconds", Value::of(61)},
        {"volumeDefault", Value::of(1.01)},
        {"audioUdpPort", Value::of(65536)},
    });

    expect_defaults(parsed.settings);
    ASSERT_EQ(parsed.rejected.size(), 6u);
    EXPECT_EQ(parsed.rejected[0], "fanMaxOnMinutes is 241, out of its range 1 to 240; taking the default 60");
}

TEST(DeviceSettingsTest, ARefusedFieldLeavesTheOthersAlone) {
    const auto parsed = parse({
        {"fanMaxOnMinutes", Value::of(1)},
        {"motionPulseCount", Value::other()},
        {"audioUdpPort", Value::of(1e9)},
    });

    const auto& settings = parsed.settings;

    EXPECT_EQ(settings.fan_max_on_minutes, 1);
    EXPECT_EQ(settings.motion_pulse_count, 1);
    EXPECT_EQ(settings.audio_udp_port, 11106);
    EXPECT_EQ(parsed.rejected.size(), 2u);
}

}  // namespace
