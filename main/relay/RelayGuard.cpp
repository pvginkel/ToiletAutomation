#include "RelayGuard.h"

#include <algorithm>

bool RelayGuard::set(bool on, int64_t now) {
    if (on == _on) {
        return false;
    }

    _on = on;
    if (on) {
        _on_since = now;
    }

    return true;
}

bool RelayGuard::tick(int64_t now) {
    if (!_on || now - _on_since < _max_on_ms) {
        return false;
    }

    _on = false;

    return true;
}

int64_t RelayGuard::get_on_time_left(int64_t now) const {
    if (!_on) {
        return 0;
    }

    return std::max<int64_t>(0, _on_since + _max_on_ms - now);
}
