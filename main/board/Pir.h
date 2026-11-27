#pragma once

#include <cstdint>

// The PIR's output, PIR_GPIO (board/BoardConfig.h): high on motion. The pin is an input with the
// pull-down, so a disconnected sensor reads low, and an interrupt on either edge, as hwtest's
// pir_init has it.
//
// The interrupt queues the level it reads and the time. Application drains the queue on the app
// task and feeds the motion filter (motion/MotionFilter.h).
class Pir {
public:
    struct Edge {
        bool level;
        int64_t at_ms;  // esp_timer's clock, in milliseconds
    };

    // Sets the pin up and starts the interrupt. When the PIR reads high already, the queue starts
    // with that level, so the filter hears the pulse that runs at boot.
    static void begin();

    // The next edge the interrupt queued, without waiting. False when there is none.
    static bool receive(Edge& edge);

    // The pin's level now.
    static int get_level();

    // The interrupts dropped since boot, because the queue was full.
    static uint32_t get_lost();
};
