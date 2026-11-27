#include "VocGate.h"

bool VocGate::sample(int32_t index, bool blackout, int64_t now) {
    if (blackout) {
        _measured.reset();
        return false;
    }

    _measured = index;

    if (_override.has_value() || _published == index) {
        return false;
    }

    // The first index goes out at once; a change after it waits out the interval.
    if (_published.has_value() && now - _published_at < MIN_INTERVAL_MS) {
        return false;
    }

    return publish(index, now);
}

bool VocGate::set_override(int32_t index, int64_t now) {
    _override = index;

    return publish(index, now);
}

bool VocGate::clear_override(int64_t now) {
    if (!_override.has_value()) {
        return false;
    }

    _override.reset();

    return publish(_measured, now);
}

bool VocGate::publish(std::optional<int32_t> index, int64_t now) {
    if (_published == index) {
        return false;
    }

    _published = index;
    _published_at = now;

    return true;
}
