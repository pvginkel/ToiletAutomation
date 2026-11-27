#pragma once

// Every pin of the ToiletAutomation board: a Waveshare ESP32-S3-Tiny, whose pin names are GPIO
// numbers, wired to the schematic (docs/hardware-bring-up.md, "Pins").

#include "driver/gpio.h"
#include "driver/i2s_types.h"

// HC-SR602 PIR: OUT is high on motion.

constexpr gpio_num_t PIR_GPIO = GPIO_NUM_1;

// SEN0394 (SGP40) on I2C. The SEN0394 board carries the bus's pull-ups.

constexpr gpio_num_t AIRQ_GPIO_SCL = GPIO_NUM_2;
constexpr gpio_num_t AIRQ_GPIO_SDA = GPIO_NUM_3;

// The fan relay: 1 kOhm to an S8050's base, which switches the G5Q-1A's coil. High is on. The relay
// is off at boot (design decision 5), and nothing holds the base low while the chip is in reset.

constexpr gpio_num_t RELAY_GPIO = GPIO_NUM_4;

// MAX98357 on I2S, in Philips format at 48 kHz as the bring-up tested it. Its SD is tied to 3V3, so it
// plays the left slot only.

constexpr i2s_port_t SPEAKER_I2S_PORT = I2S_NUM_0;

constexpr gpio_num_t SPEAKER_GPIO_DATA = GPIO_NUM_16;  // DIN
constexpr gpio_num_t SPEAKER_GPIO_SCK = GPIO_NUM_17;   // BCLK
constexpr gpio_num_t SPEAKER_GPIO_WS = GPIO_NUM_18;    // LRC

// The loudest the speaker takes: above -12 dB, a full-scale tone crackles. So a volume of 1 is -12 dB.
constexpr float SPEAKER_MAX_DB = -12.0f;
