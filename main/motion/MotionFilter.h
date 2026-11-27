#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <utility>

// The motion filter (docs/high-level-design.md, section 6): the PIR's edges in, motion out. The
// HC-SR602 holds its output high on motion, for at least about 1.7 s and for longer while the
// motion goes on. Motion is on from the moment the filter accepts a pulse until the PIR falls.
//
// A pulse is accepted once both of these hold, and rejected when one of them cannot:
// - it has been high for the minimum pulse length, 0 meaning from its rising edge;
// - it is at least the pulse count-th pulse of that length to rise within the pulse window.
// Before any of that, a pulse that rises before the hold-off after time zero (boot) has ended is
// ignored: the PIR gives a false pulse in the first seconds after power-up. It is never accepted,
// not even if it is still high once the hold-off has ended, and it counts toward nothing but the
// edges and the ignored counter. Motion comes only from a pulse that rises after the hold-off.
//
// At the defaults, 0 ms and 1 pulse, every pulse is accepted at its rising edge. A pulse rejected
// for having too few others in the window still counts toward the window of the pulses after it.
//
// The listener hears every edge and every decision, in order and with the counters already
// counting it, so the caller logs and publishes from it. Times are milliseconds on one monotonic
// clock, passed in by the caller.
class MotionFilter {
public:
    struct Settings {
        int64_t min_pulse_ms = 0;
        int pulse_count = 1;
        int64_t pulse_window_ms = 10 * 1000;
        int64_t hold_off_ms = 10 * 1000;  // from time zero; a pulse that rises before it ends is ignored
    };

    struct Counters {
        uint32_t edges;     // level changes, rising and falling
        uint32_t glitches;  // edges that found the level unchanged: too short to read, or read twice
        uint32_t accepted;  // pulses that turned motion on
        uint32_t rejected;  // pulses that did not
        uint32_t ignored;   // pulses that rose inside the hold-off after boot
    };

    enum class EventKind {
        PIR_HIGH,         // a rising edge
        PIR_LOW,          // a falling edge
        PIR_GLITCH,       // an edge with the level the filter already had
        PULSE_ACCEPTED,   // motion on
        PULSE_TOO_SHORT,  // rejected: it fell before the minimum pulse length
        PULSE_TOO_FEW,    // rejected: too few pulses within the window
        PULSE_IGNORED,    // not decided: it rose inside the hold-off after boot
        MOTION_OFF,       // the PIR fell while motion was on
    };

    struct Event {
        EventKind kind;
        int64_t at;       // the edge's time, or the time of the decision
        bool level;       // the level the edge read
        int64_t high_ms;  // how long the pulse had been high: every kind but PIR_HIGH and PIR_GLITCH
        int pulses;       // PULSE_ACCEPTED and PULSE_TOO_FEW: the pulses within the window, this one included
    };

    using Listener = std::function<void(const Event& event)>;

private:
    Settings _settings;
    Listener _listener;
    Counters _counters{};
    bool _level{};
    bool _motion{};
    bool _pending{};  // the pulse that is high has not been decided yet
    int64_t _rise_at{};
    std::deque<int64_t> _rises;  // the rising edges of the recent pulses that were long enough

public:
    MotionFilter(Settings settings, Listener listener) : _settings(settings), _listener(std::move(listener)) {}

    // A change applies from the next decision, the pulse that is high included.
    void set_settings(const Settings& settings) { _settings = settings; }
    const Settings& get_settings() const { return _settings; }

    // The PIR's level at a time, from an edge.
    void edge(bool level, int64_t at);

    // Decides the pulse that is high once it has been high for the minimum pulse length.
    void tick(int64_t now);

    bool is_motion() const { return _motion; }
    bool get_level() const { return _level; }
    const Counters& get_counters() const { return _counters; }

private:
    void advance(int64_t now);
    void decide(int64_t now);
    void emit(EventKind kind, int64_t at, int64_t high_ms = 0, int pulses = 0);
};
