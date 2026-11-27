#pragma once

#include "AudioPlayer.h"
#include "config/DeviceSettings.h"
#include "nvs.h"

// The speaker: esp-audio-support's AudioPlayer on the board's MAX98357, with the volume Home Assistant
// sets. On the app task only, apart from the player, which is the component's and safe on any task.
//
// The volume is kept in NVS. Until Home Assistant or the console has set one, the device
// configuration's volumeDefault applies.
class Audio {
    AudioPlayer _player;
    nvs_handle_t _nvs{};
    double _volume{};
    // Whether the volume is NVS's, rather than volumeDefault.
    bool _volume_saved{};

public:
    struct Status {
        double volume;
        float volume_db;
        bool volume_saved;
        bool playing;
        AudioPlayerStats stats;
    };

    // Reads the volume from NVS and starts the player, idle.
    esp_err_t begin(const DeviceSettings& settings);

    // The device configuration has loaded: its volumeDefault applies while NVS has no volume. True when
    // that changed the volume.
    bool apply_settings(const DeviceSettings& settings);

    // Sets the volume, clamped, and keeps it in NVS. False when it already was that volume.
    bool set_volume(double volume, const char* source);
    double get_volume() const { return _volume; }

    // Two short beeps.
    void identify(const char* source);
    void play_tone(float frequency_hz, uint32_t duration_ms, float level, const char* source);

    Status get_status();

private:
    void apply_volume(double volume);
};
