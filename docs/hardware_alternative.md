# Hardware alternative (HYPOTHETICAL - not built)

Status: exploratory only. The project continues on the current hardware
(LCDWIKI ES3C28P). This document captures a design study for a possible custom
or alternative board, with cost estimates. Nothing here is committed to.

Minimum target: ESP32-S3, touch display, microSD, audio out (speaker and/or
headphone jack). Soldering or a third-party PCB fab is acceptable.

All prices are indicative (LCSC / JLCPCB, January 2026), in euros, ex tax and
shipping. Add roughly +20-30% delivered to France (shipping + VAT + duties).
They are estimates, not quotes, and fluctuate.

## Hard constraint: firmware compatibility
The existing firmware needs:
- ESP32-S3 with 8 MB PSRAM + 16 MB flash (module WROOM-1 N16R8). Non negotiable:
  mbedTLS, LVGL and Wi-Fi/LWIP buffers live in PSRAM. Many cheap boards have
  2 MB or none and would not run.
- Display ILI9341 or ST7789 (SPI), supported by esp_lcd -> zero porting.
- Capacitive touch FT6336/FT5x06 -> drop-in. CST816S or GT911 work too but need
  a touch-driver swap.

## Options by effort

### Option A: ready-made all-in-one board (little/no soldering)
- Waveshare ESP32-S3-Touch-LCD-2.8 (N16R8): closest to the current board. Verify
  8 MB PSRAM and the touch controller. Reliable vendor, good docs.
- M5Stack CoreS3: S3, IPS touch 2", microSD, built-in speaker + amp, enclosure
  included. Polished, slightly premium.
- Espressif ESP32-S3-BOX-3: official kit, 2.4" touch, ES8311 codec + speaker,
  excellent audio/support. Fixed enclosure, no headphone jack.
- Guition / JC ESP32-S3 (e.g. JC3248W535): cheapest (AliExpress), variable QC/docs.
Typical: ~15-25 EUR/u, nothing to build.

### Option B: assembly from modules (some soldering, no PCB design)
- ESP32-S3-DevKitC-1 (N16R8) + ILI9341 2.8" + FT6236 module + audio + microSD SPI.
Best effort/flexibility for a few units. Material ~16-20 EUR/u in small qty.

### Option C: custom PCB (cheapest at volume, most work)
Module ESP32-S3-WROOM-1-N16R8 + the rest in SMD, assembled by JLCPCB. Detailed
below. Only beats Option A on cost from ~50 units, or for clean integration
(connectors, battery, enclosure).

## Audio paths
- Speaker only, simplest/most reliable: MAX98357A (I2S class-D amp, ~3 W). No I2C
  codec -> removes the project's #1 crash cause (codec init on the shared I2C bus).
- Headphone jack: PCM5102A (I2S DAC). No I2C either.
- Keep ES8311 codec: zero audio-side porting, but keeps the I2C codec and its risk.
Trade-off without a codec: volume must be done in software (scale PCM samples);
there is no hardware volume/mute register.

Is a small speaker good enough? Yes for Bugne (mostly speech + casual music), on
par with commercial kids players (Toniebox/Yoto are mono too). The amp is not the
limit: the speaker driver and a small sealed back chamber in the enclosure matter
far more than the amp choice. For real music quality, the headphone/line output
(PCM5102A) beats any tiny built-in speaker.

## Recommended config for Option C: dual output (speaker + headphone)
Both MAX98357A and PCM5102A are I2S sinks wired in parallel on one I2S bus.
Speaker for everyday use, headphone/line jack for quality. Jack-detect switch
mutes the speaker in hardware.

## BOM (per board) - dual output

On-board (placed by JLCPCB):

| Ref | Component | Footprint | LCSC (indicative) | Qty/board |
|---|---|---|---|---|
| U1 | ESP32-S3-WROOM-1 N16R8 | Module | C2913209 | 1 |
| U2 | MAX98357A (speaker amp) | TQFN-16 | C910544 | 1 |
| U5 | PCM5102A (headphone DAC) | TSSOP-20 | C107071 | 1 |
| U3 | AMS1117-3.3 (LDO) | SOT-223 | C6186 | 1 |
| U4 | TP4056 (LiPo charger) | ESOP-8 | C16581 | 1 |
| D1 | B5819W (Schottky path) | SOD-123 | C8598 | 1 |
| J1 | USB-C 16P | SMD | C165948 | 1 |
| J2 | microSD push-pull | SMD | C91145 | 1 |
| SW1/SW2 | tactile buttons BOOT + RST | SMD-2P | C318884 | 2 |
| C* | 100nF / 1uF / 10uF / 22uF | 0603/0805 | C14663,C15849,C15850,C45783 | ~17 |
| R* | 10k/5.1k/1.2k/1k/100R/100k/0R + SDIO pull-ups | 0603 | see notes | ~17 |
| LED1 | charge LED | 0603 | C2286 | 1 |

Off-board (sourced + mounted separately, mark DNP in the JLCPCB tool):

| Ref | Component | Qty/board |
|---|---|---|
| LS1 | speaker 3W 4ohm ~40mm (good quality) | 1 |
| J4 | 3.5mm jack with detect switch | 1 |
| J5 | JST-PH 2P battery connector | 1 |
| J3 | 2.8" ILI9341 + FT6336 display module | 1 |
Note: the microSD CARD itself is a consumable, not in the BOM (user provides it).

A ready-to-edit CSV starting point lives alongside this study (bugne_pcb_bom.csv
in the working scratch). The exact LCSC numbers must be checked for stock and
footprint in the JLCPCB tool, and a real schematic + review is required before fab.

## Cost estimates

### Material (per board, ex assembly)
| Block | ~10 u | ~100 u |
|---|---|---|
| Card material (S3, amp, LDO, charger, USB-C, microSD socket, passives, PCB) | ~9.1 | ~6.3 |
| Headphone add-on (PCM5102A + jack + caps) | ~2.5 | ~1.7 |

### Assembly (JLCPCB PCBA)
One-time fixed ~25 USD (setup + stencil + ~5 extended-part feeders at ~3 USD),
plus ~0.2 USD/board placement.
- 10 u: ~2.9 EUR/u
- 50 u: ~1.0 EUR/u
- 100 u: ~0.5 EUR/u

### Display: pre-soldered/module vs bare panel
- Bare panel + FFC: ~5-7 EUR (10 u) / ~4-5 EUR (100 u). Flex slides into an FFC
  connector placed by JLCPCB (no fine soldering) but manual handling + some scrap.
- Pre-soldered module (driver+touch on its own board, plug-in): ~7-9 EUR (10 u) /
  ~5.5-6.5 EUR (100 u). Removes the FFC connector and all flex handling.
- Difference: ~+2 to +3 EUR/u for the pre-soldered/module route.

### All-in per unit (material + assembly, ex tax)
| | 10 u | 50 u | 100 u |
|---|---|---|---|
| Bare panel (FFC) | ~18-19 | ~12-13 | ~11.3 |
| Pre-soldered module | ~20-23 | ~16-17 | ~12.8-14.5 |
(+20-30% delivered to France.)

### Bottom line
- At ~100 u: ~11-15 EUR/u material+assembly (~14-18 EUR/u delivered) -> custom PCB
  clearly beats a ready board.
- At ~10 u: ~18-23 EUR/u -> barely better than a ready board; the assembly fixed
  cost still dominates.
- Pre-soldered display: ~+2-3 EUR/u but less scrap and faster mounting.

## Wiring (dual output) - reuses the documented pin map
Pins reused from the current board so the firmware ports with minimal change.
Deltas vs the current board are flagged.

### Power tree
- USB-C VBUS (5V) -> TP4056 VIN (charge) AND -> system rail via D1 (Schottky).
- Battery JST-PH (J5) -> TP4056 BAT. PROG = 1.2k (~1A).
- VSYS = OR of (VBUS via D1) and (battery).
- AMS1117-3.3: IN = VSYS, OUT = 3V3 (10uF in / 22uF out).
- 3V3 powers: S3, PCM5102A, display, microSD, touch.
- MAX98357A VDD = VSYS (3-5V) for more output (~2-3W).

### ESP32-S3 (U1)
- 3V3 + GND, 100nF x3 + 10uF decoupling.
- EN: 10k pull-up + 1uF to GND. SW2 (EN->GND) = reset.
- IO0 (BOOT): 10k pull-up + SW1 (IO0->GND).
- USB native: IO19 = D-, IO20 = D+ -> USB-C. CC1/CC2 = 5.1k to GND each.

### I2S audio (TX only) -> both sinks in parallel
Delta: no ES8311, no I2C audio, no mic.
- IO5 = BCLK -> MAX98357A BCLK + PCM5102A BCK
- IO7 = WS/LRCK -> MAX98357A LRC + PCM5102A LRCK
- IO8 = DOUT -> MAX98357A DIN + PCM5102A DIN
- IO4 = MCLK: not needed (PCM5102A internal PLL) -> free
- IO6 = ex-mic DIN -> free

### Speaker amp MAX98357A (U2)
- VDD=VSYS, GND, I2S as above, 100nF+10uF.
- GAIN floating = 9 dB.
- SD: R6 100k to VDD -> mono (L+R)/2, enabled. Jack switch shorts SD to GND when a
  plug is inserted -> speaker muted in hardware. Optional: route IO1 to SD for
  firmware force-mute.
- OUT+ / OUT- -> speaker (bridge; do NOT tie OUT- to GND).

### Headphone DAC PCM5102A (U5)
- 3V3 supplies + decoupling, BCK/LRCK/DIN from the same I2S lines.
- SCK -> GND (internal PLL). FMT -> GND (I2S). XSMT -> 10k pull-up (un-muted).
  FLT/DEMP -> GND.
- OUTL/OUTR -> 100R series -> 10uF coupling -> jack tip/ring. Sleeve = GND.

### SPI display ILI9341V (same pins)
CS=IO10, DC=IO46, SCLK=IO12, MOSI=IO11, MISO=IO13, BL=IO45. Display RESET -> board
reset line (as today) or a free GPIO.

### I2C touch FT6336G (same pins, now alone on the bus)
SDA=IO16, SCL=IO15, RST=IO18, INT=IO17. Add 4.7k pull-ups SDA->3V3 and SCL->3V3 if
the display module does not include them.

### microSD SDIO 4-bit (same pins)
CLK=IO38, CMD=IO40, D0=IO39, D1=IO41, D2=IO48, D3=IO47.
IMPORTANT: add 10k pull-ups to 3V3 on CMD, D0, D1, D2, D3. The current board's SD
timeout (send_op_cond 0x107) is consistent with missing/weak SDIO pull-ups -> fix
by design here.

### Buttons / LED / battery sense
- RGB LED (WS2812) data = IO42 + 100nF. BOOT button IO0, RESET button on EN.
- Battery sense (deferred in firmware, but wire the divider): IO1 is free here (amp
  enable moved to the jack switch) and is ADC1_CH0 -> divider 2x 100k VBAT->GND,
  midpoint to IO1 (1:1, x2 in software), 100nF to GND.

### Free / reserved pins
- Free: IO2, IO14, IO21, IO6, IO4.
- Avoid as runtime I/O at reset (strapping): IO0, IO3, IO45, IO46.

## Firmware impact summary
- Volume becomes software (PCM scaling); no hardware volume/mute register.
- I2C audio init disappears (less crash risk).
- Two "free" fixes by design: SDIO pull-ups (likely fixes SD detection) and a
  defined battery ADC pin (IO1).
- Display/touch/SD/I2S pins unchanged -> minimal driver porting.
