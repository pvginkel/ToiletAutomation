#pragma once

#include "Application.h"
#include "esp_err.h"

// The console on USB-Serial-JTAG, the primary console, which the bench drives
// (docs/high-level-design.md, section 9). Its commands run on the console's own task, so a command
// that touches the app's state hands it to the app task through the app's queue.
//
// esp-mdm's LogManager takes over esp_log's output to ship it to IoT Support, and passes every line
// on to the console as well. What a command prints goes to the console only.
class Console {
    Application* _application;

public:
    explicit Console(Application* application) : _application(application) {}

    esp_err_t begin();
};
