#pragma once

#include "ApplicationBase.h"
#include "config/DeviceSettings.h"
#include "relay/RelayGuard.h"

// The firmware's app on esp-mdm. One task, the app task, runs esp-mdm's queue: it is the only task
// that touches the app's state, and every other task hands it a closure through get_queue(). The
// motion, air and audio parts join it in later steps (docs/firmware-plan.md).
//
// Home Assistant sees the device through esp-mqtt-support's discovery, with all of the app's state
// in one JSON on the device's state topic.
class Application : public ApplicationBase {
    DeviceSettings _settings;
    RelayGuard _relay_guard;

public:
    struct RelayStatus {
        bool on;
        int pin_level;
        int64_t on_time_left_ms;
    };

    Application();

    // The fan relay, for the console and HA. On the app task only. A command goes through the
    // guard, so the fail-safe covers a relay forced from the console too.
    RelayStatus get_relay_status();
    void set_relay(bool on, const char* source);

protected:
    void do_begin() override;
    void do_configuration_loaded(cJSON* data) override;
    void do_ready() override;
    void do_process() override;

private:
    void run();
    void publish_discovery();
    void send_state();
};
