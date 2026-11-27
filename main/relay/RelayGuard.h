#pragma once

#include <cstdint>

// The fan relay's state and its fail-safe (docs/high-level-design.md, section 7). The relay turns
// itself off once it has been on for the maximum on-time, counted from its change from off to on.
// An on command while it is on does not restart the count: HA sees the relay report off, and turns
// it on again if it still wants the fan.
//
// It starts off, and its state is not persisted. Times are milliseconds on one monotonic clock,
// passed in by the caller.
class RelayGuard {
    int64_t _max_on_ms;
    bool _on{};
    int64_t _on_since{};

public:
    explicit RelayGuard(int64_t max_on_ms) : _max_on_ms(max_on_ms) {}

    // The maximum on-time. A change applies to the count that is running.
    void set_max_on_ms(int64_t max_on_ms) { _max_on_ms = max_on_ms; }
    int64_t get_max_on_ms() const { return _max_on_ms; }

    bool is_on() const { return _on; }

    // Turns the relay on or off. True when that changed its state.
    bool set(bool on, int64_t now);

    // Turns the relay off once it has been on for the maximum on-time. True when it did.
    bool tick(int64_t now);

    // What is left of the on-time before the fail-safe turns the relay off, or 0 while it is off.
    int64_t get_on_time_left(int64_t now) const;
};
