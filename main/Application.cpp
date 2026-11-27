#include "Application.h"

#include <cinttypes>

#include "board/Pir.h"
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

static MotionFilter::Settings get_motion_settings(const DeviceSettings& settings) {
    return {
        .min_pulse_ms = settings.motion_min_pulse_ms,
        .pulse_count = settings.motion_pulse_count,
        .pulse_window_ms = settings.motion_pulse_window_seconds * 1000ll,
    };
}

Application::Application()
    : _relay_guard(_settings.fan_max_on_minutes * MS_PER_MINUTE),
      _motion_filter(get_motion_settings(_settings),
                     [this](const MotionFilter::Event& event) { on_motion_event(event); }),
      _air_sensor(&get_queue(), [this](const AirSensor::Sample& sample) { on_air_sample(sample); }) {}

void Application::do_begin() {
    ApplicationBase::do_begin();

    get_mqtt_connection().on_publish_discovery([this]() { publish_discovery(); });

    // Before the network, so the filter hears a pulse that runs at boot. Its edges wait in the
    // PIR's queue until the app task drains it.
    Pir::begin();

    ESP_LOGI(TAG, "Motion: PIR started, reads %d", Pir::get_level());

    // Also before the network, so the algorithm's blackout runs while the device connects. Its
    // samples wait in the queue until the app task runs it.
    _air_sensor.begin();

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
    _motion_filter.set_settings(get_motion_settings(_settings));

    ESP_LOGI(TAG,
             "Device configuration loaded: fanMaxOnMinutes %d, motionMinPulseMs %d, motionPulseCount %d, "
             "motionPulseWindowSeconds %d, volumeDefault %.2f, audioUdpPort %d",
             _settings.fan_max_on_minutes, _settings.motion_min_pulse_ms, _settings.motion_pulse_count,
             _settings.motion_pulse_window_seconds, _settings.volume_default, _settings.audio_udp_port);
}

void Application::do_ready() {
    ApplicationBase::do_ready();

    // HA's entities are unavailable until the state topic says the device is online. This is the
    // first point on the app task with MQTT connected: on_connected_changed runs on the MQTT event
    // task, which must not publish (esp-mqtt-support's back-pressure). A lost connection restarts
    // the device, so it comes once a boot.
    send_state();

    ESP_LOGI(TAG, "Ready, firmware %s", esp_app_get_description()->version);
}

void Application::do_process() {
    ApplicationBase::do_process();

    process_motion();

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

void Application::process_motion() {
    Pir::Edge edge;
    while (Pir::receive(edge)) {
        _motion_edge_source = "PIR";
        _motion_filter.edge(edge.level, edge.at_ms);
    }

    const auto now = esp_get_millis();

    if (_injected_fall_at.has_value() && now >= *_injected_fall_at) {
        const auto at = *_injected_fall_at;
        _injected_fall_at.reset();

        _motion_edge_source = "injected pulse";
        _motion_filter.edge(false, at);
    }

    _motion_filter.tick(now);

    const auto lost = Pir::get_lost();
    if (lost != _logged_lost) {
        _logged_lost = lost;

        ESP_LOGW(TAG, "Motion: %" PRIu32 " PIR interrupts lost since boot, to a full queue", lost);
    }
}

void Application::on_motion_event(const MotionFilter::Event& event) {
    const auto& counters = _motion_filter.get_counters();
    const auto& settings = _motion_filter.get_settings();

    // An edge's time on the log's clock, which counts from the reset where esp_timer counts from the
    // app's start. The app task logs an edge late while it is busy, 10 s at boot, so the line says
    // when the edge came.
    const auto at = event.at + ((int64_t)esp_log_timestamp() - esp_get_millis());

    // Every line ends with the counters as they stand after it, so a count reads off the last one.
    char counted[112];
    snprintf(counted, sizeof(counted),
             "edges %" PRIu32 ", glitches %" PRIu32 ", accepted %" PRIu32 ", rejected %" PRIu32 ", ignored %" PRIu32,
             counters.edges, counters.glitches, counters.accepted, counters.rejected, counters.ignored);

    switch (event.kind) {
        case MotionFilter::EventKind::PIR_HIGH:
            ESP_LOGI(TAG, "Motion: %s high at %" PRId64 " ms (%s)", _motion_edge_source, at, counted);
            break;

        case MotionFilter::EventKind::PIR_LOW:
            ESP_LOGI(TAG, "Motion: %s low at %" PRId64 " ms, after %" PRId64 " ms high (%s)", _motion_edge_source, at,
                     event.high_ms, counted);
            break;

        case MotionFilter::EventKind::PIR_GLITCH:
            ESP_LOGI(TAG, "Motion: %s glitch at %" PRId64 " ms, the level still %d (%s)", _motion_edge_source, at,
                     event.level ? 1 : 0, counted);
            break;

        case MotionFilter::EventKind::PULSE_ACCEPTED:
            ESP_LOGI(TAG, "Motion on: pulse accepted after %" PRId64 " ms high, %d of %d within %" PRId64 " s (%s)",
                     event.high_ms, event.pulses, settings.pulse_count, settings.pulse_window_ms / 1000, counted);
            send_state();
            break;

        case MotionFilter::EventKind::PULSE_TOO_SHORT:
            ESP_LOGI(TAG, "Motion: pulse rejected, high for %" PRId64 " ms of the %" PRId64 " ms minimum (%s)",
                     event.high_ms, settings.min_pulse_ms, counted);
            break;

        case MotionFilter::EventKind::PULSE_TOO_FEW:
            ESP_LOGI(TAG, "Motion: pulse rejected, %d of the %d pulses it needs within %" PRId64 " s (%s)",
                     event.pulses, settings.pulse_count, settings.pulse_window_ms / 1000, counted);
            break;

        case MotionFilter::EventKind::PULSE_IGNORED:
            ESP_LOGI(TAG,
                     "Motion: pulse ignored, it rose at %" PRId64 " ms, inside the %" PRId64
                     " s hold-off after boot (%s)",
                     at, settings.hold_off_ms / 1000, counted);
            break;

        case MotionFilter::EventKind::MOTION_OFF:
            ESP_LOGI(TAG, "Motion off: the pulse fell after %" PRId64 " ms high (%s)", event.high_ms, counted);
            send_state();
            break;
    }
}

void Application::on_air_sample(const AirSensor::Sample& sample) {
    const auto was_blackout = !_air_sample.has_value() || _air_sample->blackout;

    _air_sample = sample;
    _air_samples++;

    if (sample.lost != _logged_air_lost) {
        _logged_air_lost = sample.lost;

        ESP_LOGW(TAG, "VOC: %" PRIu32 " samples lost since boot, to a full queue", sample.lost);
    }

    if (was_blackout && !sample.blackout) {
        ESP_LOGI(TAG, "VOC: first index %" PRId32 " after the blackout (raw %u)", sample.index, sample.raw);
    }

    if (_voc_gate.sample(sample.index, sample.blackout, esp_get_millis())) {
        send_state();
    }
}

Application::VocStatus Application::get_voc_status() {
    return {
        .sample = _air_sample,
        .samples = _air_samples,
        .published = _voc_gate.get_published(),
        .override_index = _voc_gate.get_override(),
    };
}

void Application::inject_voc(std::optional<int32_t> index) {
    const auto now = esp_get_millis();

    if (index.has_value()) {
        _voc_gate.set_override(*index, now);

        ESP_LOGI(TAG, "VOC: index %" PRId32 " injected, from the console: Home Assistant gets it until inject off",
                 *index);
    } else if (_voc_gate.clear_override(now)) {
        const auto published = _voc_gate.get_published();
        if (published.has_value()) {
            ESP_LOGI(TAG, "VOC: injection off, from the console: back to the measured index %" PRId32, *published);
        } else {
            ESP_LOGI(TAG, "VOC: injection off, from the console: no index until the blackout is over");
        }
    } else {
        ESP_LOGI(TAG, "VOC: injection already off, from the console");
    }

    // HA hears the state after every command, a command that changed nothing included.
    send_state();
}

Application::MotionStatus Application::get_motion_status() {
    return {
        .motion = _motion_filter.is_motion(),
        .filter_level = _motion_filter.get_level(),
        .pin_level = Pir::get_level(),
        .counters = _motion_filter.get_counters(),
        .lost = Pir::get_lost(),
        .settings = _motion_filter.get_settings(),
    };
}

bool Application::inject_motion(int64_t high_ms) {
    if (_motion_filter.get_level()) {
        ESP_LOGI(TAG, "Motion: no %" PRId64 " ms pulse injected, from the console: a pulse is high already", high_ms);
        return false;
    }

    const auto now = esp_get_millis();

    ESP_LOGI(TAG, "Motion: injecting a %" PRId64 " ms pulse, from the console", high_ms);

    _injected_fall_at = now + high_ms;

    _motion_edge_source = "injected pulse";
    _motion_filter.edge(true, now);

    return true;
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

    get_mqtt_connection().publish_binary_sensor_discovery(
        MQTTDiscovery{
            .name = "Motion",
            .object_id = "motion",
            .device_class = "motion",
        },
        MQTTBinarySensorDiscovery{
            .value_template = "{{ value_json.motion }}",
        });

    // No device class: HA's VOC classes take a concentration, and the index is a number from 1 to 500.
    // The state has null in the blackout, which HA's MQTT sensor shows as unknown.
    get_mqtt_connection().publish_sensor_discovery(
        MQTTDiscovery{
            .name = "VOC index",
            .object_id = "voc_index",
            .icon = "mdi:air-filter",
        },
        MQTTSensorDiscovery{
            .state_class = "measurement",
            .value_template = "{{ value_json.voc_index }}",
            .suggested_display_precision = 0,
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
    cJSON_AddBoolToObject(state, "motion", _motion_filter.is_motion());

    const auto voc_index = _voc_gate.get_published();
    if (voc_index.has_value()) {
        cJSON_AddNumberToObject(state, "voc_index", *voc_index);
    } else {
        cJSON_AddNullToObject(state, "voc_index");
    }

    mqtt_connection.send_state(state);
}
