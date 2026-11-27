#pragma once

#include <cstdint>
#include <optional>

// Decides the VOC index Home Assistant sees (docs/high-level-design.md, section 7): the algorithm's
// samples in, the published index out.
//
// - While the algorithm is in its blackout, nothing is published, so an HA automation never acts on
//   an index the algorithm has not learned.
// - After it, a change of the index is published, at most once every 10 s: the first index at once,
//   and a change within 10 s of the last publish once the 10 s are over, as the index then stands.
// - The override, from the console, publishes its index at once and holds it against the samples.
//   Its release publishes the measured index at once, or nothing while the algorithm is still in
//   its blackout.
//
// Every call that can change the published index says whether it did, so the caller sends the state
// then. Times are milliseconds on one monotonic clock, passed in by the caller.
class VocGate {
public:
    static constexpr int64_t MIN_INTERVAL_MS = 10 * 1000;

private:
    std::optional<int32_t> _measured;
    std::optional<int32_t> _override;
    std::optional<int32_t> _published;
    int64_t _published_at{};

public:
    // A sample of the algorithm: its index, and whether it is in its blackout, which makes the index
    // meaningless.
    bool sample(int32_t index, bool blackout, int64_t now);

    bool set_override(int32_t index, int64_t now);
    bool clear_override(int64_t now);

    // The index the device publishes, none while the algorithm is in its blackout.
    std::optional<int32_t> get_published() const { return _published; }
    // The algorithm's last index after its blackout.
    std::optional<int32_t> get_measured() const { return _measured; }
    std::optional<int32_t> get_override() const { return _override; }

private:
    bool publish(std::optional<int32_t> index, int64_t now);
};
