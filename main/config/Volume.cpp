#include "Volume.h"

#include <charconv>
#include <cmath>

std::optional<double> parse_volume(std::string_view payload) {
    constexpr std::string_view SPACE = " \t\r\n";

    const auto first = payload.find_first_not_of(SPACE);
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    payload = payload.substr(first, payload.find_last_not_of(SPACE) - first + 1);

    double value;
    const auto end = payload.data() + payload.size();
    const auto [ptr, error] = std::from_chars(payload.data(), end, value, std::chars_format::general);

    if (error != std::errc() || ptr != end || !std::isfinite(value)) {
        return std::nullopt;
    }

    return value;
}

double clamp_volume(double volume) {
    if (!(volume > 0.0)) {
        return 0.0;
    }

    return volume < 1.0 ? volume : 1.0;
}
