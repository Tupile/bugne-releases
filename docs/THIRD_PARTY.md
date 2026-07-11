# Third-party notices

Bugne is MIT licensed (see [LICENSE](../LICENSE)), but the firmware binary
links the third-party components below. Their license texts ship with their
sources: vendored files keep the license in their header, managed components
are fetched by the IDF component manager with their LICENSE file. When
distributing prebuilt binaries (for example the bugne-releases repository),
link or copy this file next to them.

## Vendored in this repository

| Component | Author | License | Source |
|---|---|---|---|
| dr_mp3 | David Reid | Public domain or MIT-0 (choice) | https://github.com/mackron/dr_libs |
| dr_flac | David Reid | Public domain or MIT-0 (choice) | https://github.com/mackron/dr_libs |
| minimp4 | lieff | CC0 | https://github.com/lieff/minimp4 |
| yxml | Yoran Heling | MIT | https://dev.yorhel.nl/yxml |
| DejaVu Sans (converted bitmap font, components/ui/fonts) | DejaVu Fonts project, Bitstream Inc. | Bitstream Vera / DejaVu Fonts license | https://dejavu-fonts.github.io/ |

## Fetched by the IDF component manager (main/idf_component.yml)

| Component | Author | License | Source |
|---|---|---|---|
| LVGL (includes QR-Code-generator by Project Nayuki, MIT) | LVGL Kft | MIT | https://lvgl.io |
| esp_lvgl_port, esp_lcd_ili9341, esp_lcd_touch, esp_lcd_touch_ft5x06 | Espressif | Apache-2.0 | https://components.espressif.com |
| esp_codec_dev, mdns, esp_websocket_client, cmake_utilities | Espressif | Apache-2.0 | https://components.espressif.com |
| esp_audio_codec | Espressif | Espressif Modified MIT | https://components.espressif.com |
| littlefs IDF port | Brian Pugh | MIT | https://github.com/joltwallet/esp_littlefs |
| littlefs core | Arm Limited, Christopher Haster | BSD-3-Clause | https://github.com/littlefs-project/littlefs |
| sendspin-cpp | Sendspin project | Apache-2.0 | https://github.com/sendspin/sendspin-cpp |
| micro-flac, micro-opus | ESPHome | Apache-2.0 | https://github.com/esphome |
| ArduinoJson | Benoit Blanchon | MIT | https://arduinojson.org |

## Framework

| Component | Author | License | Source |
|---|---|---|---|
| ESP-IDF | Espressif Systems | Apache-2.0 | https://github.com/espressif/esp-idf |
