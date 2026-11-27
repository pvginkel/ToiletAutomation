#pragma once

// Every pin of the ToiletAutomation board: a Waveshare ESP32-S3-Tiny, whose pin names are GPIO
// numbers, wired to the schematic (docs/hardware-bring-up.md, "Pins").

#include "driver/gpio.h"

// HC-SR602 PIR: OUT is high on motion.

constexpr gpio_num_t PIR_GPIO = GPIO_NUM_1;

// SEN0394 (SGP40) on I2C. The SEN0394 board carries the bus's pull-ups.

constexpr gpio_num_t AIRQ_GPIO_SCL = GPIO_NUM_2;
constexpr gpio_num_t AIRQ_GPIO_SDA = GPIO_NUM_3;

// The fan relay: 1 kOhm to an S8050's base, which switches the G5Q-1A's coil. High is on. The relay
// is off at boot (design decision 5), and nothing holds the base low while the chip is in reset.

constexpr gpio_num_t RELAY_GPIO = GPIO_NUM_4;

// MAX98357 on I2S. Its SD is tied to 3V3, so it plays the left slot only.

constexpr gpio_num_t SPEAKER_GPIO_DATA = GPIO_NUM_16;  // DIN
constexpr gpio_num_t SPEAKER_GPIO_SCK = GPIO_NUM_17;   // BCLK
constexpr gpio_num_t SPEAKER_GPIO_WS = GPIO_NUM_18;    // LRC
