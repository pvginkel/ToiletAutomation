#include "Console.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
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

// motion inject's pulse: by default longer than the HC-SR602's shortest, 1.7 s.
static constexpr long INJECT_DEFAULT_MS = 2000;
static constexpr long INJECT_MAX_MS = 10 * 60 * 1000;

// voc inject's index: the VOC index's own range.
static constexpr long VOC_INDEX_MIN = 1;
static constexpr long VOC_INDEX_MAX = 500;

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

// motion [inject [ms]]: prints motion, the PIR's level and the filter's counters since boot, after
// feeding the filter a pulse of that length when asked to.
static int motion_command(void* context, int argc, char** argv) {
    const auto application = (Application*)context;

    std::optional<long> inject_ms;
    if (argc >= 2 && argc <= 3 && strcmp(argv[1], "inject") == 0) {
        inject_ms = INJECT_DEFAULT_MS;
        if (argc == 3) {
            char* end;
            inject_ms = strtol(argv[2], &end, 10);
            if (*end || end == argv[2] || *inject_ms < 1 || *inject_ms > INJECT_MAX_MS) {
                printf("The pulse is 1 to %ld ms\n", INJECT_MAX_MS);
                return 1;
            }
        }
    } else if (argc != 1) {
        printf("Usage: motion [inject [ms]]\n");
        return 1;
    }

    const auto status = std::make_shared<Application::MotionStatus>();
    const auto injected = std::make_shared<bool>(true);

    if (!run_on_app_task(application, [application, inject_ms, status, injected]() {
            if (inject_ms.has_value()) {
                *injected = application->inject_motion(*inject_ms);
            }
            *status = application->get_motion_status();
        })) {
        return 1;
    }

    if (!*injected) {
        printf("Not injected: the filter has a pulse high already\n");
    }

    const auto& counters = status->counters;
    const auto& settings = status->settings;

    printf("Motion %s; the filter's level %d, the PIR reads %d\n", status->motion ? "on" : "off",
           status->filter_level ? 1 : 0, status->pin_level);
    printf("Since boot: edges %" PRIu32 ", glitches %" PRIu32 ", accepted %" PRIu32 ", rejected %" PRIu32
           ", ignored %" PRIu32 ", lost %" PRIu32 "\n",
           counters.edges, counters.glitches, counters.accepted, counters.rejected, counters.ignored, status->lost);
    printf("Filter: min pulse %" PRId64 " ms, pulse count %d, pulse window %" PRId64 " s, hold-off %" PRId64 " s\n",
           settings.min_pulse_ms, settings.pulse_count, settings.pulse_window_ms / 1000, settings.hold_off_ms / 1000);

    return *injected ? 0 : 1;
}

// voc [inject <index>|off]: prints the air sensor's last sample and the index HA gets, after
// overriding that index, or releasing the override, when asked to.
static int voc_command(void* context, int argc, char** argv) {
    const auto application = (Application*)context;

    // Outer: whether to inject. Inner: the index, or none to release the override.
    std::optional<std::optional<int32_t>> inject;
    if (argc == 3 && strcmp(argv[1], "inject") == 0) {
        if (strcmp(argv[2], "off") == 0) {
            inject.emplace();
        } else {
            char* end;
            const auto index = strtol(argv[2], &end, 10);
            if (*end || end == argv[2] || index < VOC_INDEX_MIN || index > VOC_INDEX_MAX) {
                printf("The index is %ld to %ld, or off\n", VOC_INDEX_MIN, VOC_INDEX_MAX);
                return 1;
            }
            inject.emplace((int32_t)index);
        }
    } else if (argc != 1) {
        printf("Usage: voc [inject <index>|off]\n");
        return 1;
    }

    const auto status = std::make_shared<Application::VocStatus>();

    if (!run_on_app_task(application, [application, inject, status]() {
            if (inject.has_value()) {
                application->inject_voc(*inject);
            }
            *status = application->get_voc_status();
        })) {
        return 1;
    }

    if (!status->sample.has_value()) {
        printf("No sample from the SGP40 yet: the boot log says why\n");
    } else if (status->sample->blackout) {
        printf("Raw %u, in the blackout: no index yet, after %" PRIu32 " samples\n", status->sample->raw,
               status->samples);
    } else {
        printf("Raw %u, index %" PRId32 ", past the blackout, after %" PRIu32 " samples\n", status->sample->raw,
               status->sample->index, status->samples);
    }

    if (status->sample.has_value() && status->sample->lost) {
        printf("Samples lost to a full queue since boot: %" PRIu32 "\n", status->sample->lost);
    }

    if (status->override_index.has_value()) {
        printf("Published: %" PRId32 ", injected; voc inject off brings back the measured index\n",
               *status->override_index);
    } else if (status->published.has_value()) {
        printf("Published: %" PRId32 "\n", *status->published);
    } else {
        printf("Published: no index\n");
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
    // come: play a test tone, and print the audio counters.
    ESP_ERROR_RETURN(add_command("relay", "[on|off]",
                                 "Force the fan relay on or off through its fail-safe, and print its state and "
                                 "the on-time left",
                                 relay_command, _application));
    ESP_ERROR_RETURN(add_command("motion", "[inject [ms]]",
                                 "Print motion, the PIR's level and the motion filter's counters since boot. "
                                 "inject first feeds the filter a pulse of that length, 2000 ms by default",
                                 motion_command, _application));
    ESP_ERROR_RETURN(add_command("voc", "[inject <index>|off]",
                                 "Print the SGP40's raw value, the VOC index and the blackout, and the index "
                                 "Home Assistant gets. inject first overrides that index, and inject off releases it",
                                 voc_command, _application));

    ESP_ERROR_RETURN(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "Console started");

    return ESP_OK;
}
