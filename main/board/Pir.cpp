#include "Pir.h"

#include <atomic>

#include "BoardConfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "support.h"

// The app task drains the queue every few milliseconds, but its HTTPS calls at boot hold it for
// seconds. The HC-SR602's pulses are 1.7 s at the shortest, so 32 edges hold over 27 s of them.
static constexpr UBaseType_t QUEUE_LENGTH = 32;

namespace {

struct QueuedEdge {
    int level;
    int64_t time_us;
};

}  // namespace

static QueueHandle_t queue;
static std::atomic<uint32_t> lost;

static void isr(void* arg) {
    const QueuedEdge edge = {gpio_get_level(PIR_GPIO), esp_timer_get_time()};

    if (xQueueSendFromISR(queue, &edge, nullptr) != pdTRUE) {
        lost.fetch_add(1, std::memory_order_relaxed);
    }
}

void Pir::begin() {
    queue = xQueueCreate(QUEUE_LENGTH, sizeof(QueuedEdge));
    ESP_ASSERT_CHECK(queue);

    const gpio_config_t config = {
        .pin_bit_mask = 1ull << PIR_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));

    // The level at start goes in first, so it comes before any edge the interrupt queues.
    const QueuedEdge start = {gpio_get_level(PIR_GPIO), esp_timer_get_time()};
    if (start.level) {
        ESP_ASSERT_CHECK(xQueueSend(queue, &start, 0) == pdTRUE);
    }

    const auto err = gpio_install_isr_service(0);
    if (err != ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE: something else installed it already.
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(gpio_isr_handler_add(PIR_GPIO, isr, nullptr));
}

bool Pir::receive(Edge& edge) {
    QueuedEdge queued;
    if (xQueueReceive(queue, &queued, 0) != pdTRUE) {
        return false;
    }

    edge = {
        .level = queued.level != 0,
        .at_ms = queued.time_us / 1000,
    };

    return true;
}

int Pir::get_level() { return gpio_get_level(PIR_GPIO); }

uint32_t Pir::get_lost() { return lost.load(std::memory_order_relaxed); }
