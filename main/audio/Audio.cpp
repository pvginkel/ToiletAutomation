#include "Audio.h"

#include <cinttypes>
#include <cmath>

#include "NVSProperty.h"
#include "board/BoardConfig.h"
#include "config/Volume.h"
#include "support.h"

LOG_TAG(Audio);

static constexpr char NVS_NAMESPACE[] = "audio";
static constexpr char NVS_VOLUME_KEY[] = "volume";

// Identify: two short beeps, at full scale before the volume, so at the default volume they are as loud
// as hwtest's beeps at boot.
static constexpr float IDENTIFY_HZ = 880.0f;
static constexpr uint32_t IDENTIFY_MS = 120;
static constexpr uint32_t IDENTIFY_PAUSE_MS = 120;
static constexpr uint32_t IDENTIFY_BEEPS = 2;
static constexpr float IDENTIFY_LEVEL = 1.0f;

esp_err_t Audio::begin(const DeviceSettings& settings) {
    ESP_ERROR_RETURN(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &_nvs));

    float saved;
    const auto err = nvs_get_f32(_nvs, NVS_VOLUME_KEY, &saved);
    if (err == ESP_OK && std::isfinite(saved)) {
        // NVS keeps a float: 0.2 comes back as 0.2000000029, which Home Assistant would show.
        _volume = clamp_volume(std::round(saved * 10000.0) / 10000.0);
        _volume_saved = true;
    } else {
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Reading the volume from NVS failed: %s", esp_err_to_name(err));
        }
        _volume = settings.volume_default;
    }

    // The ring in PSRAM: the board has 2 MB of it, and internal RAM goes to Wi-Fi and TLS.
    const AudioPlayerConfig config = {
        .i2s_port = SPEAKER_I2S_PORT,
        .bclk_gpio = SPEAKER_GPIO_SCK,
        .ws_gpio = SPEAKER_GPIO_WS,
        .dout_gpio = SPEAKER_GPIO_DATA,
        .format = I2SFormat::PHILIPS,
        .buffer_in_psram = true,
        .udp_port = (uint16_t)settings.audio_udp_port,
        .volume = (float)_volume,
    };

    ESP_ERROR_RETURN(_player.begin(config));

    ESP_LOGI(TAG, "Volume %.2f (%.1f dB), %s", _volume, _player.get_volume_db(),
             _volume_saved ? "from NVS" : "the default until the device configuration loads");

    return ESP_OK;
}

bool Audio::apply_settings(const DeviceSettings& settings) {
    if (_volume_saved || settings.volume_default == _volume) {
        return false;
    }

    apply_volume(settings.volume_default);

    ESP_LOGI(TAG, "Volume %.2f (%.1f dB), the device configuration's volumeDefault: NVS has none", _volume,
             _player.get_volume_db());

    return true;
}

bool Audio::set_volume(double volume, const char* source) {
    volume = clamp_volume(volume);

    const auto changed = volume != _volume;
    if (changed) {
        apply_volume(volume);
    }

    // NVS keeps the volume once it is set, even to volumeDefault's, so a later volumeDefault leaves it.
    if (!_volume_saved || changed) {
        auto err = nvs_set_f32(_nvs, NVS_VOLUME_KEY, (float)volume);
        if (err == ESP_OK) {
            err = nvs_commit(_nvs);
        }

        if (err == ESP_OK) {
            _volume_saved = true;

            ESP_LOGI(TAG, "Volume %.2f (%.1f dB), from %s; saved in NVS", _volume, _player.get_volume_db(), source);
        } else {
            ESP_LOGE(TAG, "Volume %.2f (%.1f dB), from %s; saving it in NVS failed: %s", _volume,
                     _player.get_volume_db(), source, esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "Volume already %.2f, from %s", _volume, source);
    }

    return changed;
}

void Audio::identify(const char* source) {
    ESP_LOGI(TAG, "Identify, from %s: %" PRIu32 " beeps of %.0f Hz, %" PRIu32 " ms, at volume %.2f", source,
             IDENTIFY_BEEPS, IDENTIFY_HZ, IDENTIFY_MS, _volume);

    _player.play_tone(IDENTIFY_HZ, IDENTIFY_MS, IDENTIFY_LEVEL, IDENTIFY_BEEPS, IDENTIFY_PAUSE_MS);
}

void Audio::play_tone(float frequency_hz, uint32_t duration_ms, float level, const char* source) {
    ESP_LOGI(TAG, "Tone, from %s: %.0f Hz, %" PRIu32 " ms, level %.2f, at volume %.2f (%.1f dB)", source, frequency_hz,
             duration_ms, level, _volume, _player.get_volume_db());

    _player.play_tone(frequency_hz, duration_ms, level);
}

Audio::Status Audio::get_status() {
    return {
        .volume = _volume,
        .volume_db = _player.get_volume_db(),
        .volume_saved = _volume_saved,
        .playing = _player.is_playing(),
        .stats = _player.get_stats(),
    };
}

void Audio::apply_volume(double volume) {
    _volume = volume;

    _player.set_volume((float)volume);
}
