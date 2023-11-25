#include "motion_sensor.h"
#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <stdlib.h>

/* Constants */
static const char *TAG = "MotionSensor";
static const uint16_t DEBOUNCE_MS = 50;

/* Internal state */
static QueueHandle_t event_queue = NULL;

/* Callback functions*/
motion_sensor_on_trigger_cb_t on_motion_sensor_trigger_cb = NULL;

void motion_sensor_set_on_trigger(motion_sensor_on_trigger_cb_t cb)
{
    on_motion_sensor_trigger_cb = cb;
}

/* Interrupt handler */
static void IRAM_ATTR motion_sensor_isr_handler(void *arg)
{
    int gpio = (int)arg;
    xQueueSendFromISR(event_queue, &gpio, NULL);
}

/* Motion sensor task */
static void motion_sensor_task(void *arg)
{
    int gpio;

    while(1)
    {
        if(!xQueueReceive(event_queue, &gpio, portMAX_DELAY))
            continue;

        gpio_intr_disable(gpio);
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        int level = gpio_get_level(gpio);
        ESP_LOGD(TAG, "GPIO[%d] intr, val: %d", gpio, level);
        if (on_motion_sensor_trigger_cb)
            on_motion_sensor_trigger_cb(gpio, level);
        gpio_intr_enable(gpio);
    }
}

int motion_sensor_initialize(int pin)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    if (pin == -1)
    {
        ESP_LOGI(TAG, "Motion sensor disabled");
        return 0;
    }

    /* Create a queue to handle gpio event from ISR */
    event_queue = xQueueCreate(1, sizeof(uint32_t));

    /* Configure GPIO */
    gpio_config(&io_conf);

    /* Set ISR */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(pin, motion_sensor_isr_handler, (void *)pin);

    xTaskCreate(&motion_sensor_task, "motion_sensor_task", 4096, NULL, 5, NULL);

    return 0;
}
