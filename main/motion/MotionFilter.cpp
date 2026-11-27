#include "MotionFilter.h"

void MotionFilter::edge(bool level, int64_t at) {
    // A pulse that reached the minimum before this edge is decided at the edge, the pulse that
    // falls here included.
    advance(at);

    if (level == _level) {
        _counters.glitches++;
        emit(EventKind::PIR_GLITCH, at);
        return;
    }

    _counters.edges++;
    _level = level;

    if (level) {
        _rise_at = at;
        const auto ignoring = at < _settings.hold_off_ms;
        _pending = !ignoring;

        emit(EventKind::PIR_HIGH, at);

        if (ignoring) {
            _counters.ignored++;
            emit(EventKind::PULSE_IGNORED, at);
            return;
        }

        // At a minimum of 0 the pulse is decided at its rising edge.
        advance(at);
        return;
    }

    const auto high_ms = at - _rise_at;

    emit(EventKind::PIR_LOW, at, high_ms);

    if (_pending) {
        _pending = false;
        _counters.rejected++;
        emit(EventKind::PULSE_TOO_SHORT, at, high_ms);
    } else if (_motion) {
        _motion = false;
        emit(EventKind::MOTION_OFF, at, high_ms);
    }
}

void MotionFilter::tick(int64_t now) { advance(now); }

void MotionFilter::advance(int64_t now) {
    if (_pending && now - _rise_at >= _settings.min_pulse_ms) {
        decide(now);
    }
}

void MotionFilter::decide(int64_t now) {
    _pending = false;

    // The pulses that rose within the window up to this one's rising edge, this one included.
    _rises.push_back(_rise_at);
    while (!_rises.empty() && _rise_at - _rises.front() > _settings.pulse_window_ms) {
        _rises.pop_front();
    }
    while ((int)_rises.size() > _settings.pulse_count) {
        _rises.pop_front();
    }

    const auto pulses = (int)_rises.size();
    const auto high_ms = now - _rise_at;

    if (pulses >= _settings.pulse_count) {
        _motion = true;
        _counters.accepted++;
        emit(EventKind::PULSE_ACCEPTED, now, high_ms, pulses);
    } else {
        _counters.rejected++;
        emit(EventKind::PULSE_TOO_FEW, now, high_ms, pulses);
    }
}

void MotionFilter::emit(EventKind kind, int64_t at, int64_t high_ms, int pulses) {
    if (_listener) {
        _listener(Event{
            .kind = kind,
            .at = at,
            .level = _level,
            .high_ms = high_ms,
            .pulses = pulses,
        });
    }
}
