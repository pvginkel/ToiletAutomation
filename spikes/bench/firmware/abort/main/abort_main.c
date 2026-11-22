#include <stdio.h>
#include <stdlib.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Survives the panic's software reset, so a gap in the printed counts is a boot whose
// output the host never saw.
RTC_NOINIT_ATTR static uint32_t boot_count;
RTC_NOINIT_ATTR static uint32_t boot_magic;

void app_main(void)
{
    if (boot_magic != 0xB3AC4B1E) {
        boot_magic = 0xB3AC4B1E;
        boot_count = 0;
    }
    boot_count++;
    printf("bench-spike boot %lu (reset reason %d)\n", (unsigned long)boot_count, (int)esp_reset_reason());
    for (int i = 3; i > 0; i--) {
        printf("bench-spike abort in %d s, uptime %lld ms\n", i, esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("bench-spike calling abort()\n");
    fflush(stdout);
    abort();
}
