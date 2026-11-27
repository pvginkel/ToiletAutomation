#include "DeviceSettings.h"

#include <cmath>
#include <sstream>

namespace {

struct IntegerField {
    const char* name;
    int DeviceSettings::* member;
    int min;
    int max;
};

struct NumberField {
    const char* name;
    double DeviceSettings::* member;
    double min;
    double max;
};

constexpr IntegerField INTEGER_FIELDS[] = {
    {"fanMaxOnMinutes", &DeviceSettings::fan_max_on_minutes, 1, 240},
    {"motionMinPulseMs", &DeviceSettings::motion_min_pulse_ms, 0, 1500},
    {"motionPulseCount", &DeviceSettings::motion_pulse_count, 1, 3},
    {"motionPulseWindowSeconds", &DeviceSettings::motion_pulse_window_seconds, 1, 60},
    {"audioUdpPort", &DeviceSettings::audio_udp_port, 1024, 65535},
};

constexpr NumberField NUMBER_FIELDS[] = {
    {"volumeDefault", &DeviceSettings::volume_default, 0, 1},
};

// The field's number when the configuration has a valid one, after reporting why it is refused
// when it has one that is not.
template <typename T>
bool read_number(const DeviceSettings::Lookup& lookup, const DeviceSettings::Rejected& rejected, const char* name,
                 T min, T max, T default_value, bool integer, double& number) {
    const auto value = lookup(name);

    const auto reject = [&](const char* why) {
        if (rejected) {
            std::ostringstream message;
            message << name << " " << why << "; taking the default " << default_value;
            rejected(message.str());
        }

        return false;
    };

    switch (value.kind) {
        case DeviceSettings::Value::Kind::MISSING:
            return false;
        case DeviceSettings::Value::Kind::OTHER:
            return reject("is not a number");
        case DeviceSettings::Value::Kind::NUMBER:
            break;
    }

    number = value.number;

    if (integer && std::trunc(number) != number) {
        std::ostringstream why;
        why << "is " << number << ", not an integer";
        return reject(why.str().c_str());
    }

    if (!(number >= min && number <= max)) {
        std::ostringstream why;
        why << "is " << number << ", out of its range " << min << " to " << max;
        return reject(why.str().c_str());
    }

    return true;
}

}  // namespace

DeviceSettings DeviceSettings::parse(const Lookup& lookup, const Rejected& rejected) {
    DeviceSettings settings;

    for (const auto& field : INTEGER_FIELDS) {
        double number;
        if (read_number(lookup, rejected, field.name, field.min, field.max, settings.*field.member, true, number)) {
            settings.*field.member = (int)number;
        }
    }

    for (const auto& field : NUMBER_FIELDS) {
        double number;
        if (read_number(lookup, rejected, field.name, field.min, field.max, settings.*field.member, false, number)) {
            settings.*field.member = number;
        }
    }

    return settings;
}
