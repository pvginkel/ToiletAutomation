#include "Application.h"

#include <cinttypes>

#include "board/Relay.h"
#include "esp_app_desc.h"
#include "support.h"

LOG_TAG(Application);

// esp-mdm's HTTPS calls run on the app task, from its queue: the access token, the device
// configuration, the coredump upload and the OTA check. esp-mdm's defaults give the main task
// 6 KB for the same work in a firmware that runs its queue there.
static constexpr uint32_t APP_TASK_STACK_SIZE = 8192;
static constexpr UBaseType_t APP_TASK_PRIORITY = 2;

static constexpr uint32_t LOOP_DELAY_MS = 10;

static constexpr int64_t MS_PER_MINUTE = 60 * 1000;

Application::Application() : _relay_guard(_settings.fan_max_on_minutes * MS_PER_MINUTE) {}

void Application::do_begin() {
    ApplicationBase::do_begin();

    get_mqtt_connection().on_publish_discovery([this]() { publish_discovery(); });

    // HA's entities are unavailable until the state topic says the device is online.
    get_mqtt_connection().on_connected_changed([this](auto state) {
        if (state.connected) {
            send_state();
        }
    });

    ESP_LOGI(TAG, "Starting the app task");

    const auto created = xTaskCreate([](void* arg) { ((Application*)arg)->run(); }, "app", APP_TASK_STACK_SIZE, this,
                                     APP_TASK_PRIORITY, nullptr);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void Application::run() {
    while (true) {
        process();

        vTaskDelay(pdMS_TO_TICKS(LOOP_DELAY_MS));
    }
}

void Application::do_configuration_loaded(cJSON* data) {
    ApplicationBase::do_configuration_loaded(data);

    _settings = DeviceSettings::parse(
        [data](const char* name) {
            const auto item = cJSON_GetObjectItemCaseSensitive(data, name);
            if (!item || cJSON_IsNull(item)) {
                return DeviceSettings::Value::missing();
            }
            if (cJSON_IsNumber(item)) {
                return DeviceSettings::Value::of(item->valuedouble);
            }
            return DeviceSettings::Value::other();
        },
        [](const std::string& message) { ESP_LOGW(TAG, "Device configuration: %s", message.c_str()); });

    _relay_guard.set_max_on_ms(_settings.fan_max_on_minutes * MS_PER_MINUTE);

    ESP_LOGI(TAG,
             "Device configuration loaded: fanMaxOnMinutes %d, motionMinPulseMs %d, motionPulseCount %d, "
             "motionPulseWindowSeconds %d, volumeDefault %.2f, audioUdpPort %d",
             _settings.fan_max_on_minutes, _settings.motion_min_pulse_ms, _settings.motion_pulse_count,
             _settings.motion_pulse_window_seconds, _settings.volume_default, _settings.audio_udp_port);
}

void Application::do_ready() {
    ApplicationBase::do_ready();

    ESP_LOGI(TAG, "Ready, firmware %s", esp_app_get_description()->version);
}

void Application::do_process() {
    ApplicationBase::do_process();

    if (_relay_guard.tick(esp_get_millis())) {
        Relay::set(false);

        ESP_LOGI(TAG, "Relay off: on for its maximum on-time of %" PRId64 " min (pin reads %d)",
                 _relay_guard.get_max_on_ms() / MS_PER_MINUTE, Relay::get_level());

        send_state();
    }
}

Application::RelayStatus Application::get_relay_status() {
    return {
        .on = _relay_guard.is_on(),
        .pin_level = Relay::get_level(),
        .on_time_left_ms = _relay_guard.get_on_time_left(esp_get_millis()),
    };
}

void Application::set_relay(bool on, const char* source) {
    const auto now = esp_get_millis();
    const auto changed = _relay_guard.set(on, now);

    // The pin follows the guard, whether or not the command changed it.
    Relay::set(_relay_guard.is_on());

    if (changed) {
        ESP_LOGI(TAG, "Relay %s, from %s (pin reads %d)", on ? "on" : "off", source, Relay::get_level());
    } else if (on) {
        ESP_LOGI(TAG, "Relay already on, from %s: the count runs on, %" PRId64 " s of on-time left", source,
                 (_relay_guard.get_on_time_left(now) + 999) / 1000);
    } else {
        ESP_LOGI(TAG, "Relay already off, from %s", source);
    }

    // HA hears the state after every command, a command that changed nothing included.
    send_state();
}

void Application::publish_discovery() {
    get_mqtt_connection().publish_switch_discovery(
        MQTTDiscovery{
            .name = "Fan",
            .object_id = "fan",
            .icon = "mdi:fan",
        },
        MQTTSwitchDiscovery{
            // The switch's states are its payloads, "on" and "off", and the state JSON has a bool.
            .value_template = "{{ 'on' if value_json.fan else 'off' }}",
        },
        [this](bool on) { set_relay(on, "Home Assistant"); });

    get_mqtt_connection().publish_button_discovery(
        MQTTDiscovery{
            .name = "Restart",
            .object_id = "restart",
            .entity_category = "config",
            .device_class = "restart",
        },
        []() {
            ESP_LOGI(TAG, "Restart, from Home Assistant");

            esp_restart();
        });
}

void Application::send_state() {
    auto& mqtt_connection = get_mqtt_connection();
    if (!mqtt_connection.is_connected()) {
        return;
    }

    const auto state = cJSON_CreateObject();
    ESP_ASSERT_CHECK(state);
    DEFER(cJSON_Delete(state));

    cJSON_AddBoolToObject(state, "fan", _relay_guard.is_on());

    mqtt_connection.send_state(state);
}
