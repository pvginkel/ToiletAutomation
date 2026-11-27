#pragma once

#include "ApplicationBase.h"

// The firmware's app on esp-mdm. One task, the app task, runs esp-mdm's queue: it is the only task
// that touches the app's state, and every other task hands it a closure through get_queue(). The
// motion, air, relay and audio parts join it in later steps (docs/high-level-design.md, section
// 11).
class Application : public ApplicationBase {
protected:
    void do_begin() override;
    void do_configuration_loaded(cJSON* data) override;
    void do_ready() override;

private:
    void run();
};
