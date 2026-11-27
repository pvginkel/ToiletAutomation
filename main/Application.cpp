#include "Application.h"

#include "esp_app_desc.h"
#include "support.h"

LOG_TAG(Application);

// esp-mdm's HTTPS calls run on the app task, from its queue: the access token, the device
// configuration, the coredump upload and the OTA check. esp-mdm's defaults give the main task
// 6 KB for the same work in a firmware that runs its queue there.
static constexpr uint32_t APP_TASK_STACK_SIZE = 8192;
static constexpr UBaseType_t APP_TASK_PRIORITY = 2;

static constexpr uint32_t LOOP_DELAY_MS = 10;

void Application::do_begin() {
    ApplicationBase::do_begin();

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

    ESP_LOGI(TAG, "Device configuration loaded");
}

void Application::do_ready() {
    ApplicationBase::do_ready();

    ESP_LOGI(TAG, "Ready, firmware %s", esp_app_get_description()->version);
}
