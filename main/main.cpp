#include "Application.h"
#include "Console.h"
#include "board/BoardConfig.h"
#include "driver/gpio.h"
#include "support.h"

LOG_TAG(main);

// The relay is off at boot (docs/high-level-design.md, decision 5). Nothing drives GPIO4 while
// the chip is in reset (docs/hardware-bring-up.md), so the pin is driven low before anything else
// runs, esp-mdm included.
static void drive_relay_off() {
    ESP_ERROR_CHECK(gpio_set_level(RELAY_GPIO, 0));

    const gpio_config_t config = {
        .pin_bit_mask = 1ull << RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&config));

    ESP_LOGI(TAG, "Relay driven off");
}

extern "C" void app_main(void) {
    drive_relay_off();

    static Application application;
    application.begin();

    static Console console(&application);
    ESP_ERROR_CHECK(console.begin());

    // app_main returns: the app task and the console's task run the firmware from here.
}
