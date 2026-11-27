#pragma once

#include <optional>

#include "ApplicationBase.h"
#include "config/DeviceSettings.h"
#include "motion/MotionFilter.h"
#include "relay/RelayGuard.h"

// The firmware's app on esp-mdm. One task, the app task, runs esp-mdm's queue: it is the only task
// that touches the app's state, and every other task hands it a closure through get_queue(). The
// air and audio parts join it in later steps (docs/firmware-plan.md).
//
// Home Assistant sees the device through esp-mqtt-support's discovery, with all of the app's state
// in one JSON on the device's state topic.
class Application : public ApplicationBase {
    DeviceSettings _settings;
    RelayGuard _relay_guard;
    MotionFilter _motion_filter;
    // Where the edge the motion filter is hearing comes from, for the log: the PIR or the console.
    const char* _motion_edge_source = "PIR";
    // When the pulse the console injected falls.
    std::optional<int64_t> _injected_fall_at;
    // The PIR's lost interrupts, as last logged.
    uint32_t _logged_lost{};

public:
    struct RelayStatus {
        bool on;
        int pin_level;
        int64_t on_time_left_ms;
    };

    struct MotionStatus {
        bool motion;
        bool filter_level;
        int pin_level;
        MotionFilter::Counters counters;
        uint32_t lost;
        MotionFilter::Settings settings;
    };

    Application();

    // The fan relay, for the console and HA. On the app task only. A command goes through the
    // guard, so the fail-safe covers a relay forced from the console too.
    RelayStatus get_relay_status();
    void set_relay(bool on, const char* source);

    // Motion, for the console. On the app task only. inject_motion feeds the filter a pulse of that
    // length, as if from the PIR, and is false when the filter has a pulse high already.
    MotionStatus get_motion_status();
    bool inject_motion(int64_t high_ms);

protected:
    void do_begin() override;
    void do_configuration_loaded(cJSON* data) override;
    void do_ready() override;
    void do_process() override;

private:
    void run();
    void process_motion();
    void on_motion_event(const MotionFilter::Event& event);
    void publish_discovery();
    void send_state();
};
