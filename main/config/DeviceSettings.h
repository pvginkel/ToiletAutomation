#pragma once

#include <functional>
#include <string>

// The app's fields of the device configuration, which IoT Support serves (docs/high-level-design.md,
// section 8, and docs/firmware-plan.md, "Device configuration"). A field that is missing, of the
// wrong type or out of range takes its default, so the firmware runs before IoT Support's schema
// has the field.
struct DeviceSettings {
    // One field as the configuration's JSON has it: absent, a number, or anything else.
    struct Value {
        enum class Kind { MISSING, NUMBER, OTHER };

        Kind kind = Kind::MISSING;
        double number{};

        static Value missing() { return {}; }
        static Value of(double number) { return {Kind::NUMBER, number}; }
        static Value other() { return {Kind::OTHER, 0}; }
    };

    // Looks a field up by its name in the configuration.
    using Lookup = std::function<Value(const char* name)>;

    // Hears each field that is present but refused, as a sentence that names it.
    using Rejected = std::function<void(const std::string& message)>;

    int fan_max_on_minutes = 60;           // fanMaxOnMinutes, 1 to 240
    int motion_min_pulse_ms = 0;           // motionMinPulseMs, 0 to 1500
    int motion_pulse_count = 1;            // motionPulseCount, 1 to 3
    int motion_pulse_window_seconds = 10;  // motionPulseWindowSeconds, 1 to 60
    double volume_default = 0.5;           // volumeDefault, 0 to 1
    int audio_udp_port = 11106;            // audioUdpPort, 1024 to 65535: Intercom's port

    // The defaults, with every field the configuration has right in its place.
    static DeviceSettings parse(const Lookup& lookup, const Rejected& rejected = {});
};
