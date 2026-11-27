#include "Relay.h"

#include "BoardConfig.h"
#include "support.h"

void Relay::begin() {
    // The level first, so the pin is low from the moment it becomes an output.
    ESP_ERROR_CHECK(gpio_set_level(RELAY_GPIO, 0));

    const gpio_config_t config = {
        .pin_bit_mask = 1ull << RELAY_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
}

void Relay::set(bool on) { ESP_ERROR_CHECK(gpio_set_level(RELAY_GPIO, on ? 1 : 0)); }

int Relay::get_level() { return gpio_get_level(RELAY_GPIO); }
