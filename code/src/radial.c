/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2022 Jacek Fedorynski
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "bsp/board.h"
#include "tusb.h"

#include "pico/stdlib.h"
#include "pico/bootrom.h"

#include "hardware/gpio.h"
#include "hardware/adc.h"

#define BUTTON_PIN 28
#define SENSOR1_PIN 26
#define SENSOR2_PIN 27
#define SENSOR1_INPUT 0
#define SENSOR2_INPUT 1

#define DIAL_REPORT_ID 1
#define CLICKS_PER_360_DEGREES 48
#define UNITS_PER_CLICK (3600/CLICKS_PER_360_DEGREES)

// These IDs are bogus. If you want to distribute any hardware using this,
// you will have to get real ones.
#define USB_VID 0xCAFE
#define USB_PID 0xBAEA

tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,

    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x00,

    .bNumConfigurations = 0x01
};

// https://docs.microsoft.com/en-us/windows-hardware/design/component-guidelines/radial-controller-sample-report-descriptors
uint8_t const desc_hid_report[] = {
    0x05, 0x01,                 // USAGE_PAGE (Generic Desktop)
    0x09, 0x0e,                 // USAGE (System Multi-Axis Controller)
    0xa1, 0x01,                 // COLLECTION (Application)
    0x85, DIAL_REPORT_ID,       //   REPORT_ID (Radial Controller)
    0x05, 0x0d,                 //   USAGE_PAGE (Digitizers)
    0x09, 0x21,                 //   USAGE (Puck)
    0xa1, 0x00,                 //   COLLECTION (Physical)
    0x05, 0x09,                 //     USAGE_PAGE (Buttons)
    0x09, 0x01,                 //     USAGE (Button 1)
    0x95, 0x01,                 //     REPORT_COUNT (1)
    0x75, 0x01,                 //     REPORT_SIZE (1)
    0x15, 0x00,                 //     LOGICAL_MINIMUM (0)
    0x25, 0x01,                 //     LOGICAL_MAXIMUM (1)
    0x81, 0x02,                 //     INPUT (Data,Var,Abs)
    0x05, 0x01,                 //     USAGE_PAGE (Generic Desktop)
    0x09, 0x37,                 //     USAGE (Dial)
    0x95, 0x01,                 //     REPORT_COUNT (1)
    0x75, 0x0f,                 //     REPORT_SIZE (15)
    0x55, 0x0f,                 //     UNIT_EXPONENT (-1)
    0x65, 0x14,                 //     UNIT (Degrees, English Rotation)
    0x36, 0xf0, 0xf1,           //     PHYSICAL_MINIMUM (-3600)
    0x46, 0x10, 0x0e,           //     PHYSICAL_MAXIMUM (3600)
    0x16, 0xf0, 0xf1,           //     LOGICAL_MINIMUM (-3600)
    0x26, 0x10, 0x0e,           //     LOGICAL_MAXIMUM (3600)
    0x81, 0x06,                 //     INPUT (Data,Var,Rel)
    0xc0,                       //   END_COLLECTION
    0xc0,                       // END_COLLECTION
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#define EPNUM_HID 0x81

uint8_t const desc_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0, 100),

    // Interface number, string index, protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 1)
};

char const *string_desc_arr[] = {
    (const char[]) {0x09, 0x04},        // 0: is supported language is English (0x0409)
    "Arasaka",                  // 1: Manufacturer
    "Radial Controller",        // 2: Product
};

// HID report
typedef struct __attribute__ ((packed)) {
    uint16_t button:1;
    int16_t rotation:15;
} hid_report_t;

hid_report_t report;

int encoder_rotation = 0;
int prev_state1 = 0;

void encoder_task(void) {
    adc_select_input(SENSOR1_INPUT);
    uint16_t val1 = adc_read();
    adc_select_input(SENSOR2_INPUT);
    uint16_t val2 = adc_read();

    int state1 = prev_state1;
    if (val1 > 2670) {
        state1 = 1;
    }
    if (val1 < 1602) {
        state1 = -1;
    }

    int state2 = 0;
    if (val2 > 2048) {
        state2 = 1;
    }
    if (val2 < 2048) {
        state2 = -1;
    }

    if (prev_state1 != state1) {
        if (state1 == state2) {
            encoder_rotation -= UNITS_PER_CLICK;
        }
        if (state1 == -state2) {
            encoder_rotation += UNITS_PER_CLICK;
        }
    }

    prev_state1 = state1;
}

inline float away_from_zero(float x) {
    if (x > 0) {
        return ceilf(x);
    }
    if (x < 0) {
        return floorf(x);
    }
    return x;
}

void hid_task(void) {
    if (!tud_hid_ready()) {
        return;
    }

    report.button = !gpio_get(BUTTON_PIN);

    // Scroll doesn't work very well if we send 7.5 degrees of rotation
    // in one report. So we spread small chunks over several reports, which
    // makes the software happy. It adds some delay, but it's negligible
    // in practice. It wouldn't be necessary if we had a high-resolution
    // rotary encoder.
    report.rotation = away_from_zero(5.0f * encoder_rotation / UNITS_PER_CLICK);
    encoder_rotation -= report.rotation;

    tud_hid_report(DIAL_REPORT_ID, &report, sizeof(report));
}

void pins_init(void) {
    adc_init();
    adc_gpio_init(SENSOR1_PIN);
    adc_gpio_init(SENSOR2_PIN);
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);

    // reset into BOOTSEL if button pressed while plugging in
    // (convenient during development)
    sleep_ms(50);
    if (!gpio_get(BUTTON_PIN)) {
        reset_usb_boot(0, 0);
    }
}

void report_init(void) {
    memset(&report, 0, sizeof(report));
}

int main(void) {
    stdio_init_all();
    board_init();
    pins_init();
    report_init();
    tusb_init();

    while (1) {
        tud_task();             // tinyusb device task
        encoder_task();
        hid_task();
    }

    return 0;
}

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *) &desc_device;
}

// Invoked when received GET HID REPORT DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf) {
    return desc_hid_report;
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t * buffer, uint16_t reqlen) {
    return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
}

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    return desc_configuration;
}

static uint16_t _desc_str[32];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        // Note: the 0xEE index string is a Microsoft OS 1.0 Descriptors.
        // https://docs.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors

        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])))
            return NULL;

        const char *str = string_desc_arr[index];

        // Cap at max char
        chr_count = strlen(str);
        if (chr_count > 31)
            chr_count = 31;

        // Convert ASCII string into UTF-16
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    // first byte is length (including header), second byte is string type
    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);

    return _desc_str;
}
