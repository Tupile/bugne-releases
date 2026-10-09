#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    JPEG_IMAGE_FORMAT_RGB565,
} esp_jpeg_image_format_t;

typedef enum {
    JPEG_IMAGE_SCALE_0,
    JPEG_IMAGE_SCALE_1_2,
    JPEG_IMAGE_SCALE_1_4,
    JPEG_IMAGE_SCALE_1_8,
} esp_jpeg_image_scale_t;

typedef struct {
    uint8_t *indata;
    uint32_t indata_size;
    esp_jpeg_image_format_t out_format;
    esp_jpeg_image_scale_t out_scale;
    uint8_t *outbuf;
    uint32_t outbuf_size;
} esp_jpeg_image_cfg_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    size_t output_len;
} esp_jpeg_image_output_t;

extern esp_err_t esp_jpeg_get_image_info(const esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *out);
extern esp_err_t esp_jpeg_decode(const esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *out);
