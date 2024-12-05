#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h" // Include NVS flash
#include <stdlib.h>
#include <stdio.h>
#include <string.h> // Include string.h for memcpy
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// Global LVGL objects
static lv_obj_t *label_radiation;
static lv_obj_t *label_dosage;
static lv_obj_t *chart;
static lv_chart_series_t *series;
static lv_obj_t *scale; // Add scale object

// Variables to track min and max values
static int min_value = INT_MAX;
static int max_value = INT_MIN;

// Conversion factor for CPM to µSv/h
static const float conversion_factor = 0.0057;

// Array to store CPM values for a moving average
#define AVERAGE_PERIOD 60 // 60 seconds rolling average
static int cpm_values[AVERAGE_PERIOD];
static int cpm_index = 0;
static int cpm_count = 0;

// Queue to handle received data
static QueueHandle_t data_queue;

// Data structure to pass to the queue
typedef struct {
    int radiation_level;
} data_t;

// Data processing task
static void data_processing_task(void *arg) {
    data_t received_data;
    while (1) {
        if (xQueueReceive(data_queue, &received_data, portMAX_DELAY)) {
            int radiation_level = received_data.radiation_level;

            // Update min and max values
            if (radiation_level < min_value) min_value = radiation_level;
            if (radiation_level > max_value) max_value = radiation_level;

            // Store the value in the rolling buffer
            cpm_values[cpm_index] = radiation_level;
            cpm_index = (cpm_index + 1) % AVERAGE_PERIOD;
            if (cpm_count < AVERAGE_PERIOD) cpm_count++;

            // Calculate the average CPM
            int sum_cpm = 0;
            for (int i = 0; i < cpm_count; i++) {
                sum_cpm += cpm_values[i];
            }
            float average_cpm = (float)sum_cpm / cpm_count;

            // Calculate dosage in µSv/h
            float dosage = average_cpm * conversion_factor;

            // Update labels and chart
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "Radiation: %d CPM", radiation_level);
            bsp_display_lock(0);
            lv_label_set_text(label_radiation, buffer);

            snprintf(buffer, sizeof(buffer), "Dosage: %.2f uSv/h", dosage);
            lv_label_set_text(label_dosage, buffer);

            lv_chart_set_next_value(chart, series, radiation_level);

            // Adjust chart and scale range dynamically
            int new_max_value = max_value + (max_value / 10); // Add 10% headroom
            lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, new_max_value);
            lv_scale_set_range(scale, 0, new_max_value);
            bsp_display_unlock();

            // Yield control to the OS
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// ESP-NOW receive callback
static void on_data_recv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    // Ensure the received data is null-terminated
    char received_data[32];
    if (len >= sizeof(received_data)) {
        ESP_LOGE("ESP-NOW", "Received data length exceeds buffer size");
        return;
    }
    memcpy(received_data, data, len);
    received_data[len] = '\0';

    // Convert the received string to an integer
    int radiation_level = atoi(received_data);
    if (radiation_level == 0 && received_data[0] != '0') {
        ESP_LOGE("ESP-NOW", "Failed to convert received data to integer");
        return;
    }

    // Send the data to the processing task
    data_t data_to_send = { .radiation_level = radiation_level };
    if (xQueueSend(data_queue, &data_to_send, portMAX_DELAY) != pdPASS) {
        ESP_LOGE("ESP-NOW", "Failed to send data to queue");
    }
}

// UI setup function
static void setup_ui(lv_obj_t *scr) {
   bsp_display_lock(0);
    // Style initialization
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_text_font(&style, &lv_font_montserrat_28);

    // Create labels
    label_radiation = lv_label_create(scr);
    lv_label_set_text(label_radiation, "Radiation: -- CPM");
    lv_obj_add_style(label_radiation, &style, 0);
    lv_obj_align(label_radiation, LV_ALIGN_TOP_MID, 0, 20);

    label_dosage = lv_label_create(scr);
    lv_label_set_text(label_dosage, "Dosage: -- uSv/h");
    lv_obj_add_style(label_dosage, &style, 0);
    lv_obj_align(label_dosage, LV_ALIGN_TOP_MID, 0, 50);

    // Create chart
    chart = lv_chart_create(scr);
    lv_obj_set_size(chart, 600, 300);
    lv_obj_align(chart, LV_ALIGN_CENTER, 0, 40);
    lv_chart_set_point_count(chart, 100);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    series = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_YELLOW), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000); // Initial range

    // Initialize chart with zero data
    for (int i = 0; i < 100; i++) {
        lv_chart_set_next_value(chart, series, 0);
    }

    // Create scale
    scale = lv_scale_create(scr);
    lv_obj_set_size(scale, 50, 300); // Match the height of the chart
    lv_obj_align_to(scale, chart, LV_ALIGN_OUT_LEFT_MID, -10, 0); // Align to the left of the chart with some padding
    lv_scale_set_mode(scale, LV_SCALE_MODE_VERTICAL_LEFT);
    lv_scale_set_range(scale, 0, 1000); // Initial range
    lv_scale_set_total_tick_count(scale, 10);
    lv_scale_set_major_tick_every(scale, 1);
    lv_scale_set_label_show(scale, true);

    // Adjust scale position to account for chart padding
    lv_coord_t chart_top_padding = lv_obj_get_style_pad_top(chart, LV_PART_MAIN);
    lv_coord_t chart_bottom_padding = lv_obj_get_style_pad_bottom(chart, LV_PART_MAIN);
    lv_obj_set_y(scale, lv_obj_get_y(chart) + chart_top_padding);
    lv_obj_set_height(scale, lv_obj_get_height(chart) - chart_top_padding - chart_bottom_padding);
    bsp_display_unlock();
}

void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize LVGL and other components
    lv_init();
    bsp_display_start();
    bsp_display_lock(0);

    // Initialize Wi-Fi in station mode
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Initialize ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    // Create LVGL objects
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    setup_ui(scr);

    bsp_display_unlock();
    bsp_display_backlight_on();

    // Create a queue to handle received data
    data_queue = xQueueCreate(10, sizeof(data_t));
    if (data_queue == NULL) {
        ESP_LOGE("APP_MAIN", "Failed to create data queue");
        return;
    }

    // Create a task to process the received data
    xTaskCreate(data_processing_task, "data_processing_task", 4096, NULL, 5, NULL);

    while (1) {
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(10)); // Yield control to the OS
    }
}