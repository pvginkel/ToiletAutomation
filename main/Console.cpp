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

// tone's defaults and ranges. The level is before the volume, so at the default volume of 0.5 a full
// level is -20 dBFS. The highest frequency stays under half of 48 kHz.
static constexpr long TONE_DEFAULT_HZ = 440;
static constexpr long TONE_DEFAULT_MS = 1000;
static constexpr double TONE_DEFAULT_LEVEL = 1.0;
static constexpr long TONE_MIN_HZ = 20;
static constexpr long TONE_MAX_HZ = 20000;
static constexpr long TONE_MAX_MS = 30000;

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

// A whole number from min to max, or none.
static std::optional<long> parse_long(const char* text, long min, long max) {
    char* end;
    const auto value = strtol(text, &end, 10);
    if (*end || end == text || value < min || value > max) {
        return std::nullopt;
    }
    return value;
}

// A number from 0 to 1, or none.
static std::optional<double> parse_unit(const char* text) {
    char* end;
    const auto value = strtod(text, &end);
    if (*end || end == text || !(value >= 0.0 && value <= 1.0)) {
        return std::nullopt;
    }
    return value;
}

static void print_volume(const Audio::Status& status) {
    printf("Volume %.2f (%.1f dB), %s\n", status.volume, status.volume_db,
           status.volume_saved ? "from NVS" : "volumeDefault: NVS has none");
}

// tone [hz] [ms] [level]: plays a sine through the volume, as a stream would.
static int tone_command(void* context, int argc, char** argv) {
    const auto application = (Application*)context;

    if (argc > 4) {
        printf("Usage: tone [hz] [ms] [level]\n");
        return 1;
    }

    auto hz = TONE_DEFAULT_HZ;
    auto ms = TONE_DEFAULT_MS;
    auto level = TONE_DEFAULT_LEVEL;

    if (argc > 1) {
        const auto parsed = parse_long(argv[1], TONE_MIN_HZ, TONE_MAX_HZ);
        if (!parsed.has_value()) {
            printf("The frequency is %ld to %ld Hz\n", TONE_MIN_HZ, TONE_MAX_HZ);
            return 1;
        }
        hz = *parsed;
    }
    if (argc > 2) {
        const auto parsed = parse_long(argv[2], 1, TONE_MAX_MS);
        if (!parsed.has_value()) {
            printf("The length is 1 to %ld ms\n", TONE_MAX_MS);
            return 1;
        }
        ms = *parsed;
    }
    if (argc > 3) {
        const auto parsed = parse_unit(argv[3]);
        if (!parsed.has_value()) {
            printf("The level is 0 to 1 of full scale, before the volume\n");
            return 1;
        }
        level = *parsed;
    }

    const auto status = std::make_shared<Audio::Status>();

    if (!run_on_app_task(application, [application, hz, ms, level, status]() {
            application->play_tone((float)hz, (uint32_t)ms, (float)level, "the console");
            *status = application->get_audio_status();
        })) {
        return 1;
    }

    printf("Tone: %ld Hz, %ld ms, level %.2f\n", hz, ms, level);
    print_volume(*status);

    return 0;
}

// volume [0-1]: prints the volume, after setting it as Home Assistant's Volume does when asked to.
static int volume_command(void* context, int argc, char** argv) {
    const auto application = (Application*)context;

    std::optional<double> volume;
    if (argc == 2) {
        volume = parse_unit(argv[1]);
        if (!volume.has_value()) {
            printf("The volume is 0 to 1\n");
            return 1;
        }
    } else if (argc != 1) {
        printf("Usage: volume [0-1]\n");
        return 1;
    }

    const auto status = std::make_shared<Audio::Status>();

    if (!run_on_app_task(application, [application, volume, status]() {
            if (volume.has_value()) {
                application->set_volume(*volume, "the console");
            }
            *status = application->get_audio_status();
        })) {
        return 1;
    }

    print_volume(*status);

    return 0;
}

// audio: prints whether the speaker plays, how far the stream reaches ahead, and the stream buffer's,
// the output's and the receiver's counters since boot.
static int audio_command(void* context, int argc, char** argv) {
    const auto application = (Application*)context;

    if (argc != 1) {
        printf("Usage: audio\n");
        return 1;
    }

    const auto status = std::make_shared<Audio::Status>();

    if (!run_on_app_task(application, [application, status]() { *status = application->get_audio_status(); })) {
        return 1;
    }

    const auto& stats = status->stats;
    const auto& stream = stats.stream;

    printf("Speaker %s; the stream buffer holds %" PRIu64 " ms of its %" PRIu32 " ms\n",
           status->playing ? "playing" : "idle", (uint64_t)stats.buffered_samples * 1000 / status->sample_rate_hz,
           status->depth_ms);
    printf("Stream since boot: packets %" PRIu32 ", gaps filled %" PRIu32 ", reordered %" PRIu32
           ", late or duplicate %" PRIu32 ", overflows %" PRIu32 ", invalid %" PRIu32 ", new streams %" PRIu32
           ", underruns %" PRIu32 "\n",
           stream.packets, stream.gaps_filled, stream.reordered, stream.late_or_duplicate, stream.overflows,
           stream.invalid, stream.new_streams, stream.underruns);
    printf("Output since boot: starts %" PRIu32 ", DMA underruns %" PRIu32 ", %" PRIu32 " bytes of stack spare\n",
           stats.output.starts, stats.output.underruns, stats.output.stack_spare_bytes);
    if (status->udp_port) {
        printf("Receiver on UDP port %d since boot: datagrams %" PRIu32 ", errors %" PRIu32 ", %" PRIu32
               " bytes of stack spare\n",
               status->udp_port, stats.receiver.datagrams, stats.receiver.errors, stats.receiver.stack_spare_bytes);
    } else {
        printf("Receiver not started: the network is not up yet\n");
    }
    print_volume(*status);

    return 0;
}

// identify: plays Home Assistant's Identify, two short beeps.
static int identify_command(void* context, int argc, char** argv) {
    const auto application = (Application*)context;

    if (argc != 1) {
        printf("Usage: identify\n");
        return 1;
    }

    const auto status = std::make_shared<Audio::Status>();

    if (!run_on_app_task(application, [application, status]() {
            application->identify("the console");
            *status = application->get_audio_status();
        })) {
        return 1;
    }

    printf("Identify: two beeps\n");
    print_volume(*status);

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

    // The test hooks of docs/high-level-design.md, section 9. A hook reaches its part through
    // _application, on the app task.
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
    ESP_ERROR_RETURN(add_command("tone", "[hz] [ms] [level]",
                                 "Play a sine on the speaker through the volume: 440 Hz for 1000 ms at a level of 1 "
                                 "by default, the level 0 to 1 of full scale before the volume",
                                 tone_command, _application));
    ESP_ERROR_RETURN(add_command("volume", "[0-1]",
                                 "Print the volume and whether it is NVS's. With a value, first set it as Home "
                                 "Assistant's Volume does, kept in NVS",
                                 volume_command, _application));
    ESP_ERROR_RETURN(add_command("identify", nullptr, "Play Home Assistant's Identify: two short beeps",
                                 identify_command, _application));
    ESP_ERROR_RETURN(add_command("audio", nullptr,
                                 "Print whether the speaker plays, how much the stream buffer holds, and the "
                                 "stream's, the output's and the UDP receiver's counters since boot",
                                 audio_command, _application));

    ESP_ERROR_RETURN(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "Console started");

    return ESP_OK;
}
