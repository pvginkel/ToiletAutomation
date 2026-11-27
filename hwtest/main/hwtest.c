// Hardware sanity test for the ToiletAutomation board.
//
// Checks that every part on the schematic answers on the pins the schematic gives it. At boot it
// reports the module (flash, PSRAM), the I2C bus and the SGP40, the PIR, and plays two short
// beeps; then it runs a console on USB-Serial-JTAG so the bench can drive each part.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// The pins, from schematic/ToiletAutomation (the Waveshare ESP32-S3-Tiny's pin names are GPIO
// numbers).
#define PIN_PIR_OUT GPIO_NUM_1       // HC-SR602 OUT
#define PIN_AIRQ_SCL GPIO_NUM_2      // SEN0394 (SGP40) SCL
#define PIN_AIRQ_SDA GPIO_NUM_3      // SEN0394 (SGP40) SDA
#define PIN_RELAY GPIO_NUM_4         // 1K -> S8050 base -> G5Q-1A coil
#define PIN_SPEAKER_DATA GPIO_NUM_16 // MAX98357 DIN
#define PIN_SPEAKER_SCK GPIO_NUM_17  // MAX98357 BCLK
#define PIN_SPEAKER_WS GPIO_NUM_18   // MAX98357 LRC

#define SGP40_ADDR 0x59
#define SAMPLE_RATE 48000
#define I2C_TIMEOUT_MS 100

static const char *TAG = "hwtest";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_sgp40;
static i2s_chan_handle_t s_tx;
static QueueHandle_t s_pir_queue;
static volatile uint32_t s_pir_rising;
static int s_relay;

static void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) + 1); }

// ---- Module -----------------------------------------------------------------------------------

static uint32_t efuse_field(const esp_efuse_desc_t *field[]) {
    uint32_t value = 0;
    esp_efuse_read_field_blob(field, &value, esp_efuse_get_field_size(field));
    return value;
}

static void report_module(void) {
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t flash_size = 0;
    esp_flash_get_physical_size(NULL, &flash_size);

    // esptool's decoding of the ESP32-S3 package eFuses.
    static const char *const flash_caps[] = {"none", "8MB", "4MB"};
    static const char *const psram_caps[] = {"none", "8MB", "2MB"};
    uint32_t flash_cap = efuse_field(ESP_EFUSE_FLASH_CAP);
    uint32_t psram_cap = efuse_field(ESP_EFUSE_PSRAM_CAP);

    ESP_LOGI(TAG, "chip: ESP32-S3 rev v%d.%d, %d cores, MAC %02x:%02x:%02x:%02x:%02x:%02x",
             info.revision / 100, info.revision % 100, info.cores, mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    ESP_LOGI(TAG, "flash: %lu KB physical, embedded=%s, eFuse flash cap %lu (%s), vendor %lu",
             (unsigned long)(flash_size / 1024), (info.features & CHIP_FEATURE_EMB_FLASH) ? "yes" : "no",
             (unsigned long)flash_cap, flash_cap < 3 ? flash_caps[flash_cap] : "?",
             (unsigned long)efuse_field(ESP_EFUSE_FLASH_VENDOR));
    ESP_LOGI(TAG, "psram: %s, %u KB, embedded=%s, eFuse PSRAM cap %lu (%s), vendor %lu",
             esp_psram_is_initialized() ? "initialised" : "not found (quad mode tried)",
             esp_psram_is_initialized() ? (unsigned)(esp_psram_get_size() / 1024) : 0u,
             (info.features & CHIP_FEATURE_EMB_PSRAM) ? "yes" : "no", (unsigned long)psram_cap,
             psram_cap < 3 ? psram_caps[psram_cap] : "?",
             (unsigned long)efuse_field(ESP_EFUSE_PSRAM_VENDOR));
    ESP_LOGI(TAG, "pins: PIR_OUT=%d AIRQ_SCL=%d AIRQ_SDA=%d RELAY=%d SPEAKER_DATA=%d SPEAKER_SCK=%d "
                  "SPEAKER_WS=%d",
             PIN_PIR_OUT, PIN_AIRQ_SCL, PIN_AIRQ_SDA, PIN_RELAY, PIN_SPEAKER_DATA, PIN_SPEAKER_SCK,
             PIN_SPEAKER_WS);
}

// ---- I2C and the SGP40 ------------------------------------------------------------------------

// With the weak internal pull-downs on, a line with an external pull-up still reads high, and a
// floating or shorted line reads low.
static void report_i2c_idle_levels(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_AIRQ_SCL) | BIT64(PIN_AIRQ_SDA),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&cfg);
    delay_ms(5);
    int scl = gpio_get_level(PIN_AIRQ_SCL);
    int sda = gpio_get_level(PIN_AIRQ_SDA);
    ESP_LOGI(TAG, "i2c idle against internal pull-downs: SCL=%d SDA=%d (%s)", scl, sda,
             scl && sda ? "external pull-ups present" : "MISSING pull-up, or a line is floating");
    gpio_reset_pin(PIN_AIRQ_SCL);
    gpio_reset_pin(PIN_AIRQ_SDA);
}

static esp_err_t i2c_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_AIRQ_SDA,
        .scl_io_num = PIN_AIRQ_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        return err;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SGP40_ADDR,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_sgp40);
}

static int i2c_scan(void) {
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, I2C_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "i2c: device at 0x%02x%s", addr, addr == SGP40_ADDR ? " (SGP40)" : "");
            found++;
        }
    }
    ESP_LOGI(TAG, "i2c scan: %d device(s)", found);
    return found;
}

// Sensirion's CRC-8: polynomial 0x31, init 0xFF.
static uint8_t sensirion_crc(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t sgp40_command(const uint8_t *tx, size_t tx_len, uint32_t wait_ms, uint16_t *words,
                               size_t n_words) {
    esp_err_t err = i2c_master_transmit(s_sgp40, tx, tx_len, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    delay_ms(wait_ms);
    uint8_t rx[9];
    err = i2c_master_receive(s_sgp40, rx, n_words * 3, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < n_words; i++) {
        if (sensirion_crc(&rx[i * 3], 2) != rx[i * 3 + 2]) {
            return ESP_ERR_INVALID_CRC;
        }
        words[i] = (uint16_t)(rx[i * 3] << 8 | rx[i * 3 + 1]);
    }
    return ESP_OK;
}

static esp_err_t sgp40_serial(uint64_t *serial) {
    static const uint8_t cmd[] = {0x36, 0x82};
    uint16_t w[3];
    esp_err_t err = sgp40_command(cmd, sizeof cmd, 1, w, 3);
    if (err == ESP_OK) {
        *serial = (uint64_t)w[0] << 32 | (uint64_t)w[1] << 16 | w[2];
    }
    return err;
}

static esp_err_t sgp40_self_test(uint16_t *result) {
    static const uint8_t cmd[] = {0x28, 0x0E};
    return sgp40_command(cmd, sizeof cmd, 320, result, 1);
}

// The raw signal with the default compensation: 50 %RH, 25 °C.
static esp_err_t sgp40_raw(uint16_t *raw) {
    uint8_t cmd[8] = {0x26, 0x0F, 0x80, 0x00, 0, 0x66, 0x66, 0};
    cmd[4] = sensirion_crc(&cmd[2], 2);
    cmd[7] = sensirion_crc(&cmd[5], 2);
    return sgp40_command(cmd, sizeof cmd, 30, raw, 1);
}

static bool sgp40_check(void) {
    uint64_t serial = 0;
    esp_err_t err = sgp40_serial(&serial);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sgp40: serial number read failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "sgp40: serial 0x%012llx", (unsigned long long)serial);
    uint16_t result = 0;
    err = sgp40_self_test(&result);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sgp40: self-test failed to run: %s", esp_err_to_name(err));
        return false;
    }
    bool pass = result == 0xD400;
    ESP_LOGI(TAG, "sgp40: self-test 0x%04x (%s)", result, pass ? "pass" : "FAIL");
    return pass;
}

// ---- PIR --------------------------------------------------------------------------------------

typedef struct {
    int level;
    int64_t time_us;
} pir_event_t;

static void pir_isr(void *arg) {
    pir_event_t event = {gpio_get_level(PIN_PIR_OUT), esp_timer_get_time()};
    xQueueSendFromISR(s_pir_queue, &event, NULL);
}

static void pir_task(void *arg) {
    int last = gpio_get_level(PIN_PIR_OUT);
    int64_t rise_us = -1;
    pir_event_t event;
    for (;;) {
        xQueueReceive(s_pir_queue, &event, portMAX_DELAY);
        if (event.level == last) {
            continue;
        }
        last = event.level;
        if (event.level) {
            int64_t since_ms = rise_us >= 0 ? (event.time_us - rise_us) / 1000 : -1;
            rise_us = event.time_us;
            s_pir_rising++;
            ESP_LOGI(TAG, "pir: HIGH (rising edge #%lu, %lld ms after the previous one)",
                     (unsigned long)s_pir_rising, since_ms);
        } else {
            ESP_LOGI(TAG, "pir: LOW (was high for %lld ms)",
                     rise_us >= 0 ? (event.time_us - rise_us) / 1000 : -1);
        }
    }
}

static void pir_init(void) {
    // The pull-down makes a disconnected sensor read low instead of floating.
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_PIR_OUT),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    s_pir_queue = xQueueCreate(32, sizeof(pir_event_t));
    xTaskCreate(pir_task, "pir", 3072, NULL, 5, NULL);
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_PIR_OUT, pir_isr, NULL));
    ESP_LOGI(TAG, "pir: level %d at start (the HC-SR602 may trigger while it warms up)",
             gpio_get_level(PIN_PIR_OUT));
}

// ---- Relay ------------------------------------------------------------------------------------

static void relay_set(int on) {
    gpio_set_level(PIN_RELAY, on);
    s_relay = on;
    ESP_LOGI(TAG, "relay: %s (pin reads %d)", on ? "ON" : "OFF", gpio_get_level(PIN_RELAY));
}

static void relay_init(void) {
    gpio_set_level(PIN_RELAY, 0);
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_RELAY),
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    relay_set(0);
}

// ---- Speaker ----------------------------------------------------------------------------------

static void speaker_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = PIN_SPEAKER_SCK,
                .ws = PIN_SPEAKER_WS,
                .dout = PIN_SPEAKER_DATA,
                .din = I2S_GPIO_UNUSED,
            },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_LOGI(TAG, "speaker: I2S Philips, 16-bit stereo, %d Hz", SAMPLE_RATE);
}

typedef enum { CHANNEL_BOTH, CHANNEL_LEFT, CHANNEL_RIGHT } channel_t;

// Plays a sine with 5 ms fades, then flushes silence and stops the clocks, which mutes the amp.
static esp_err_t play_tone(int hz, int ms, int volume_pct, channel_t channel) {
    static int16_t buf[2 * 256];
    const int total = (int)((int64_t)SAMPLE_RATE * ms / 1000);
    const int fade = SAMPLE_RATE / 200;
    const float amplitude = 32767.0f * (float)volume_pct / 100.0f;
    const float step = 2.0f * (float)M_PI * (float)hz / SAMPLE_RATE;
    float phase = 0;
    size_t written;
    esp_err_t err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        return err;
    }
    int64_t start = esp_timer_get_time();
    for (int done = 0; done < total && err == ESP_OK;) {
        int n = total - done < 256 ? total - done : 256;
        for (int i = 0; i < n; i++) {
            int k = done + i;
            float envelope = k < fade ? (float)k / fade : total - k < fade ? (float)(total - k) / fade : 1.0f;
            int16_t sample = (int16_t)(amplitude * envelope * sinf(phase));
            phase += step;
            if (phase > 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
            buf[2 * i] = channel == CHANNEL_RIGHT ? 0 : sample;
            buf[2 * i + 1] = channel == CHANNEL_LEFT ? 0 : sample;
        }
        err = i2s_channel_write(s_tx, buf, (size_t)n * 4, &written, 1000);
        done += n;
    }
    int64_t elapsed_ms = (esp_timer_get_time() - start) / 1000;
    memset(buf, 0, sizeof buf);
    for (int i = 0; i < 8 && err == ESP_OK; i++) {
        err = i2s_channel_write(s_tx, buf, sizeof buf, &written, 1000);
    }
    i2s_channel_disable(s_tx);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "tone: %d Hz, %d%%, %s, %d frames written in %lld ms", hz, volume_pct,
                 channel == CHANNEL_LEFT ? "left" : channel == CHANNEL_RIGHT ? "right" : "both", total,
                 elapsed_ms);
    }
    return err;
}

// ---- Console ----------------------------------------------------------------------------------

static int arg_int(int argc, char **argv, int index, int fallback) {
    return argc > index ? atoi(argv[index]) : fallback;
}

static int cmd_info(int argc, char **argv) {
    report_module();
    return 0;
}

static int cmd_i2cscan(int argc, char **argv) { return i2c_scan() > 0 ? 0 : 1; }

static int cmd_sgp40(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "selftest") == 0) {
        return sgp40_check() ? 0 : 1;
    }
    int count = arg_int(argc, argv, 1, 1);
    for (int i = 0; i < count; i++) {
        int64_t start = esp_timer_get_time();
        uint16_t raw = 0;
        esp_err_t err = sgp40_raw(&raw);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "sgp40: raw read failed: %s", esp_err_to_name(err));
            return 1;
        }
        ESP_LOGI(TAG, "sgp40: raw %u", raw);
        if (i + 1 < count) {
            int64_t left_ms = 1000 - (esp_timer_get_time() - start) / 1000;
            delay_ms(left_ms > 0 ? (uint32_t)left_ms : 0);
        }
    }
    return 0;
}

static int cmd_pir(int argc, char **argv) {
    ESP_LOGI(TAG, "pir: level %d, %lu rising edge(s) since boot", gpio_get_level(PIN_PIR_OUT),
             (unsigned long)s_pir_rising);
    return 0;
}

static int cmd_relay(int argc, char **argv) {
    if (argc < 2) {
        ESP_LOGI(TAG, "relay: %s", s_relay ? "ON" : "OFF");
    } else if (strcmp(argv[1], "on") == 0) {
        relay_set(1);
    } else if (strcmp(argv[1], "off") == 0) {
        relay_set(0);
    } else if (strcmp(argv[1], "pulse") == 0) {
        relay_set(1);
        delay_ms((uint32_t)arg_int(argc, argv, 2, 500));
        relay_set(0);
    } else {
        printf("usage: relay [on|off|pulse [ms]]\n");
        return 1;
    }
    return 0;
}

static int cmd_tone(int argc, char **argv) {
    int hz = arg_int(argc, argv, 1, 440);
    int ms = arg_int(argc, argv, 2, 1000);
    int volume = arg_int(argc, argv, 3, 20);
    channel_t channel = CHANNEL_BOTH;
    if (argc > 4) {
        channel = strcmp(argv[4], "left") == 0 ? CHANNEL_LEFT : strcmp(argv[4], "right") == 0 ? CHANNEL_RIGHT : CHANNEL_BOTH;
    }
    if (hz < 20 || hz > 20000 || ms < 1 || ms > 30000 || volume < 0 || volume > 100) {
        printf("usage: tone [hz 20-20000] [ms 1-30000] [volume 0-100] [both|left|right]\n");
        return 1;
    }
    esp_err_t err = play_tone(hz, ms, volume, channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tone: %s", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static void register_command(const char *name, const char *help, const char *hint,
                             esp_console_cmd_func_t func) {
    const esp_console_cmd_t cmd = {.command = name, .help = help, .hint = hint, .func = func};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static void console_start(void) {
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "hwtest> ";
    repl_cfg.task_stack_size = 6144;
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl));
    esp_console_register_help_command();
    register_command("info", "Report the chip, flash, PSRAM and pin map", NULL, cmd_info);
    register_command("i2cscan", "Scan the AIRQ I2C bus", NULL, cmd_i2cscan);
    register_command("sgp40", "Read the SGP40's raw signal at 1 Hz, or run its self-test",
                     "[count|selftest]", cmd_sgp40);
    register_command("pir", "Report the PIR level and edge count", NULL, cmd_pir);
    register_command("relay", "Switch the relay", "[on|off|pulse [ms]]", cmd_relay);
    register_command("tone", "Play a sine on the speaker", "[hz] [ms] [volume%] [both|left|right]",
                     cmd_tone);
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

// ---- Boot -------------------------------------------------------------------------------------

void app_main(void) {
    relay_init(); // first, so the relay is driven off as early as possible
    report_module();

    report_i2c_idle_levels();
    bool i2c_ok = i2c_init() == ESP_OK;
    int devices = i2c_ok ? i2c_scan() : 0;
    bool sgp40_ok = i2c_ok && sgp40_check();
    uint16_t raw = 0;
    if (sgp40_ok && sgp40_raw(&raw) == ESP_OK) {
        ESP_LOGI(TAG, "sgp40: raw %u (first reading after power-up)", raw);
    }

    pir_init();
    speaker_init();
    bool tone_ok = play_tone(880, 120, 10, CHANNEL_BOTH) == ESP_OK;
    delay_ms(100);
    tone_ok = tone_ok && play_tone(1320, 120, 10, CHANNEL_BOTH) == ESP_OK;

    ESP_LOGI(TAG, "HWTEST SUMMARY: i2c_devices=%d sgp40=%s pir_level=%d relay=off speaker=%s psram=%s",
             devices, sgp40_ok ? "ok" : "FAIL", gpio_get_level(PIN_PIR_OUT), tone_ok ? "i2s_ok" : "FAIL",
             esp_psram_is_initialized() ? "ok" : "none");
    console_start();
}
