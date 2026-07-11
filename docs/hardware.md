# Hardware: LCDWIKI ES3C28P

ESP32-S3, 8MB PSRAM, 16MB flash. The pin map below is fixed, confirmed from
vendor docs and on real hardware. Do not change it. Note: the vendor doc swaps
the two I2S data pins; the values below (DOUT = IO8, DIN = IO6) are the
hardware-confirmed ones.

## GPIO map

| Function | Signal | GPIO |
| --- | --- | --- |
| Display ILI9341V (SPI) | CS | IO10 |
| | DC | IO46 |
| | SCLK | IO12 |
| | MOSI | IO11 |
| | MISO | IO13 |
| | Backlight (BL) | IO45 |
| | Reset | tied to chip reset |
| Touch FT6336G (I2C 0x38) | SDA | IO16 |
| | SCL | IO15 |
| | RST | IO18 |
| | INT | IO17 |
| Audio codec ES8311 (I2C 0x18) | SDA | IO16 (shared) |
| | SCL | IO15 (shared) |
| Audio amp FM8002E | Enable | IO1 (active low: low plays, high mutes) |
| I2S | MCLK | IO4 |
| | BCLK | IO5 |
| | DOUT (to codec) | IO8 |
| | WS / LRCK | IO7 |
| | DIN (from mic) | IO6 |
| microSD (SDIO 4-bit) | CLK | IO38 |
| | CMD | IO40 |
| | D0 | IO39 |
| | D1 | IO41 |
| | D2 | IO48 |
| | D3 | IO47 |
| RGB LED | data | IO42 |
| BOOT button | input | IO0 (strapping, never hold at reset) |
| Battery sense | ADC | TODO, confirm on hardware (NOT IO1) |

Free expansion pins: IO2, IO14, IO21. IO3 is a strapping pin, avoid it.

## Shared I2C bus (critical)

The ES8311 codec (0x18) and the FT6336G touch (0x38) share one I2C bus on
IO16 (SDA) and IO15 (SCL). Create a single `i2c_master_bus` in the `board`
component and pass that one handle to both the codec driver and the touch
driver. Initialising the bus twice is the most common crash cause on this board.

## Battery sense

LiPo 1S 3.7V, charged by an onboard TP4054, on a JST 1.25mm 2-pin BAT port.
Battery voltage is read on an ADC pin through a 1:1 divider, so multiply the
reading by 2 to get the real voltage. The exact ADC pin is not confirmed yet.
Keep it as a TODO constant in the `board` component until verified on hardware.

## Power note

IO1 is the amp enable, not the battery ADC. Do not confuse the two.
