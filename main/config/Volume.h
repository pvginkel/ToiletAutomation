#pragma once

#include <optional>
#include <string_view>

// The speaker's volume, 0 to 1, as Home Assistant's Volume number sets it and NVS keeps it. 0 mutes.
// esp-audio-support maps it onto a range in dB.

// The number a Home Assistant number command's payload holds, such as "0.35": a decimal, with spaces
// around it allowed. None when it is anything else, a number that is not finite included.
std::optional<double> parse_volume(std::string_view payload);

// The volume held to 0 to 1. A value that is not a number mutes.
double clamp_volume(double volume);
