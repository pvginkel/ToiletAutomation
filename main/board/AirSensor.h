#pragma once

#include <cstdint>
#include <functional>

#include "Queue.h"
#include "SGP40.h"
#include "VOCIndex.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The SEN0394's SGP40 on AIRQ_GPIO_SCL and AIRQ_GPIO_SDA (board/BoardConfig.h), and the VOC index
// algorithm over it (docs/high-level-design.md, section 7).
//
// Its own task brings the sensor up, logging its serial number and its self-test, then samples it
// once a second. Each sample goes to the app task through the queue, to the listener, which feeds the
// VOC gate (air/VocGate.h). A sample the full queue cannot take is dropped and counted.
class AirSensor {
public:
    struct Sample {
        uint16_t raw;
        int32_t index;  // 0 in the blackout
        bool blackout;
        uint32_t lost;  // the samples dropped since boot
    };

    using Listener = std::function<void(const Sample& sample)>;

private:
    Queue* _queue;
    Listener _listener;
    SGP40 _sgp40;
    VOCIndex _voc_index;
    uint32_t _lost{};

public:
    AirSensor(Queue* queue, Listener listener) : _queue(queue), _listener(std::move(listener)) {}

    // Starts the sampling task, which owns the sensor and the algorithm from then on.
    void begin();

private:
    void run();
    bool start_sensor();
};
