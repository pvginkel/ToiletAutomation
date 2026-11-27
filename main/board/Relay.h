#pragma once

// The fan relay's output, RELAY_GPIO (board/BoardConfig.h). High is on. The pin is an input as well
// as an output, so its level reads back, as hwtest's relay_init has it.
//
// This is the pin only. The fail-safe is RelayGuard's (relay/RelayGuard.h), and Application drives
// the pin from it on the app task.
class Relay {
public:
    // Drives the relay off. app_main calls it before anything else runs, esp-mdm included.
    static void begin();

    static void set(bool on);

    // The pin's level as it reads back: 1 is on.
    static int get_level();
};
