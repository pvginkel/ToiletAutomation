#include "Console.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>

#include "Signal.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "support.h"

LOG_TAG(Console);

static constexpr char PROMPT[] = "toilet> ";

// How long a command waits for the app task. Its HTTPS calls at boot hold it for seconds.
static constexpr uint32_t APP_TASK_TIMEOUT_MS = 10000;

static esp_err_t add_command(const char* name, const char* help, esp_console_cmd_func_t func) {
    const esp_console_cmd_t command = {.command = name, .help = help, .func = func};

    return esp_console_cmd_register(&command);
}

static esp_err_t add_command(const char* name, const char* hint, const char* help,
                             esp_console_cmd_func_with_context_t func, void* context) {
    const esp_console_cmd_t command = {
        .command = name,
        .help = help,
        .hint = hint,
        .func_w_context = func,
        .context = context,
    };

    return esp_console_cmd_register(&command);
}

// Runs func on the app task and waits for it, so that what the command prints comes before the
// prompt. False when the app task did not get to it in time: func then still runs later, so it
// shares what it writes rather than borrowing the caller's stack.
static bool run_on_app_task(Application* application, const std::function<void()>& func) {
    const auto done = std::make_shared<Signal>();

    application->get_queue().enqueue([func, done]() {
        func();
        done->signal();
    });

    if (!done->wait(pdMS_TO_TICKS(APP_TASK_TIMEOUT_MS))) {
        printf("The app task did not answer within %" PRIu32 " s\n", APP_TASK_TIMEOUT_MS / 1000);
        return false;
    }

    return true;
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

// relay [on|off]: forces the relay through its guard, so the fail-safe covers it, and prints its
// state and the on-time left.
static int relay_command(void* context, int argc, char** argv) {
    const auto application = (Application*)context;

    std::optional<bool> on;
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        on = true;
    } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
        on = false;
    } else if (argc != 1) {
        printf("Usage: relay [on|off]\n");
        return 1;
    }

    const auto status = std::make_shared<Application::RelayStatus>();

    if (!run_on_app_task(application, [application, on, status]() {
            if (on.has_value()) {
                application->set_relay(*on, "the console");
            }
            *status = application->get_relay_status();
        })) {
        return 1;
    }

    if (status->on) {
        printf("Relay on (pin reads %d), %" PRId64 " s of on-time left\n", status->pin_level,
               (status->on_time_left_ms + 999) / 1000);
    } else {
        printf("Relay off (pin reads %d)\n", status->pin_level);
    }

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

    // The test hooks of docs/high-level-design.md, section 9, each registered in the phase that
    // builds its part. A hook reaches its part through _application, on the app task. Still to
    // come: inject motion, inject a VOC index, play a test tone, and print the audio counters.
    ESP_ERROR_RETURN(add_command("relay", "[on|off]",
                                 "Force the fan relay on or off through its fail-safe, and print its state and "
                                 "the on-time left",
                                 relay_command, _application));

    ESP_ERROR_RETURN(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "Console started");

    return ESP_OK;
}
