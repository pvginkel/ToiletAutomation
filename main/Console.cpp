#include "Console.h"

#include <cinttypes>
#include <cstdio>

#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "support.h"

LOG_TAG(Console);

static constexpr char PROMPT[] = "toilet> ";

static esp_err_t add_command(const char* name, const char* help, esp_console_cmd_func_t func) {
    const esp_console_cmd_t command = {.command = name, .help = help, .func = func};

    return esp_console_cmd_register(&command);
}

static int version_command(int argc, char** argv) {
    const auto description = esp_app_get_description();

    printf("%s %s\n", description->project_name, description->version);
    printf("ESP-IDF %s, built %s %s\n", description->idf_ver, description->date, description->time);
    printf("Up %" PRId64 " s\n", esp_get_millis() / 1000);

    return 0;
}

static int heap_command(int argc, char** argv) {
    printf("Internal: %zu free, %zu at least, %zu largest block\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("PSRAM: %zu free, %zu at least, %zu largest block\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    return 0;
}

esp_err_t Console::begin() {
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = PROMPT;

    const esp_console_dev_usb_serial_jtag_config_t device_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_console_repl_t* repl;
    ESP_ERROR_RETURN(esp_console_new_repl_usb_serial_jtag(&device_config, &repl_config, &repl));

    ESP_ERROR_RETURN(esp_console_register_help_command());
    ESP_ERROR_RETURN(
        add_command("version", "Print the firmware version, the ESP-IDF version and the uptime", version_command));
    ESP_ERROR_RETURN(add_command("heap", "Print the free internal RAM and PSRAM", heap_command));

    // The test hooks of docs/high-level-design.md, section 9, register here, each in the step that
    // builds its part: inject motion, inject a VOC index, force the relay, play a test tone, and
    // print the audio counters. A hook reaches its part through _application, on the app task.

    ESP_ERROR_RETURN(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "Console started");

    return ESP_OK;
}
