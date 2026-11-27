#include "AirSensor.h"

#include <cinttypes>

#include "BoardConfig.h"
#include "support.h"

LOG_TAG(AirSensor);

// The algorithm's sampling interval.
static constexpr uint32_t SAMPLE_INTERVAL_MS = 1000;

static constexpr uint32_t TASK_STACK_SIZE = 4096;
static constexpr UBaseType_t TASK_PRIORITY = 2;

void AirSensor::begin() {
    const auto created =
        xTaskCreate([](void* arg) { ((AirSensor*)arg)->run(); }, "air", TASK_STACK_SIZE, this, TASK_PRIORITY, nullptr);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void AirSensor::run() {
    if (!start_sensor()) {
        ESP_LOGE(TAG, "VOC: no samples this boot, so Home Assistant gets no index");

        vTaskDelete(nullptr);
        return;
    }

    // A failed measurement is skipped, and logged when the run of them starts and when it ends.
    uint32_t failed = 0;

    auto last_wake = xTaskGetTickCount();

    while (true) {
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));

        uint16_t raw;
        const auto err = _sgp40.measure_raw(raw);
        if (err != ESP_OK) {
            if (failed++ == 0) {
                ESP_LOGW(TAG, "VOC: measurement failed, skipping samples until it works: %s", esp_err_to_name(err));
            }
            continue;
        }

        if (failed) {
            ESP_LOGI(TAG, "VOC: measuring again, after %" PRIu32 " failed samples", failed);
            failed = 0;
        }

        const auto index = _voc_index.process(raw);
        const Sample sample = {
            .raw = raw,
            .index = index,
            .blackout = _voc_index.is_blackout(),
            .lost = _lost,
        };

        if (!_queue->try_enqueue([this, sample]() { _listener(sample); })) {
            _lost++;
        }
    }
}

bool AirSensor::start_sensor() {
    auto err = _sgp40.begin(AIRQ_GPIO_SDA, AIRQ_GPIO_SCL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "VOC: the SGP40's I2C bus did not start: %s", esp_err_to_name(err));
        return false;
    }

    uint64_t serial_number;
    err = _sgp40.read_serial_number(serial_number);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "VOC: the SGP40 did not answer for its serial number: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "VOC: SGP40 serial 0x%012" PRIx64, serial_number);

    uint16_t result;
    err = _sgp40.self_test(result);
    if (err == ESP_FAIL) {
        ESP_LOGE(TAG, "VOC: SGP40 self-test failed, 0x%04x of the 0x%04x that passes", result, SGP40::SELF_TEST_PASSED);
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "VOC: the SGP40's self-test did not run: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "VOC: SGP40 self-test passed (0x%04x), sampling once a second", result);

    return true;
}
