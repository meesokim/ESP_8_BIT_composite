/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/dac_continuous.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"

#define millis esp_timer_get_time 

static const char *TAG = "dac_tvout";

#include "ESP_8_BIT_GFX.h"

// A list of 8-bit color values that work well in a cycle.
uint8_t colorCycle[] = {
    0xFF, // White
    0xFE, // Lowering blue
    0xFD,
    0xFC, // No blue
    0xFD, // Raising blue
    0xFE,
    0xFF, // White
    0xF3, // Lowering green
    0xE7,
    0xE3, // No green
    0xE7, // Raising green
    0xF3,
    0xFF, // White
    0x9F, // Lowering red
    0x5F,
    0x1F, // No red
    0x5F, // Raising red
    0x9F,
    0xFF
};

#define GPIO_OUTPUT_IO_0    15
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_OUTPUT_IO_0))
// Create an instance of the graphics library
ESP_8_BIT_GFX videoOut(false /* = NTSC */, 8 /* = RGB332 color */);

void setup() {
    //zero-initialize the config structure.
    gpio_config_t io_conf = {};
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    //disable pull-up mode
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    //configure GPIO with the given settings
    gpio_config(&io_conf);
    gpio_set_level(GPIO_NUM_15, 0);
    videoOut.begin();
}

void loop() {
    // Wait for the next frame to minimize chance of visible tearing
    // videoOut.waitForFrame();

    // Get the current time and calculate a scaling factor
    unsigned long time = millis();
    float partial_second = (float)(time % 1000)/1000.0;

    // Use time scaling factor to calculate coordinates and colors
    uint8_t movingX = (uint8_t)(255*partial_second);
    uint8_t invertX = 255-movingX;
    uint8_t movingY = (uint8_t)(239*partial_second);
    uint8_t invertY = 239-movingY;

    uint8_t cycle = colorCycle[(uint8_t)(17*partial_second)];
    uint8_t invertC = 0xFF-cycle;

    // Clear screen
    videoOut.fillScreen(0);

    // Draw one rectangle
    videoOut.drawLine(movingX, 0,       255,     movingY, cycle);
    videoOut.drawLine(255,     movingY, invertX, 239,     cycle);
    videoOut.drawLine(invertX, 239,     0,       invertY, cycle);
    videoOut.drawLine(0,       invertY, movingX, 0,       cycle);

    // Draw a rectangle with inverted position and color
    videoOut.drawLine(invertX, 0,       255,     invertY, invertC);
    videoOut.drawLine(255,     invertY, movingX, 239,     invertC);
    videoOut.drawLine(movingX, 239,     0,       movingY, invertC);
    videoOut.drawLine(0,       movingY, invertX, 0,       invertC);

    // Draw text in the middle of the screen
    videoOut.setCursor(25, 80);
    videoOut.setTextColor(invertC);
    videoOut.setTextSize(2);
    videoOut.setTextWrap(false);
    videoOut.print("Adafruit GFX API");
    videoOut.setCursor(110, 120);
    videoOut.setTextColor(0xFF);
    videoOut.print("on");
    videoOut.setCursor(30, 160);
    videoOut.setTextColor(cycle);
    videoOut.print("ESP_8_BIT video");
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "DAC tvout start");
    ESP_LOGI(TAG, "--------------------------------------");
    setup();
    int r = 0;
    while(true) {
        loop();
        // vTaskDelay(1000 / portTICK_PERIOD_MS);
        gpio_set_level(GPIO_NUM_15, r++%2);
    }
}
