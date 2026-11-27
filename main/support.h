#pragma once

// Helpers for the firmware's ESP-IDF code. The pure logic the host tests build (test/) includes
// the C++ standard library only, so it never includes this.

#include "defer.h"
#include "error.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#ifndef LOG_TAG
#define LOG_TAG(v) [[maybe_unused]] static const char* TAG = #v
#endif

#define esp_get_millis() (esp_timer_get_time() / 1000ll)
