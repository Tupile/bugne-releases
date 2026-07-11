#!/usr/bin/env bash
# Flash a fresh Bugne firmware to an ESP32-S3 (LCDWIKI ES3C28P) over USB.
#
# Run this on the machine the device is plugged into. It expects the four
# release binaries next to this script (bootloader.bin, partition-table.bin,
# ota_data_initial.bin, bugne.bin) - that is how the release bundle is packed.
#
# Usage:
#   ./flash.sh [PORT] [--erase]
#     PORT     serial port (default: auto-detect /dev/ttyACM0 or /dev/ttyUSB0)
#     --erase  wipe the whole flash first (clears Wi-Fi credentials and config;
#              recommended for a clean first install on a new device)
#
# Needs esptool. If it is not found, install it: pip install esptool
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
PORT=""
ERASE=0
for a in "$@"; do
    case "$a" in
        --erase) ERASE=1 ;;
        *)       PORT="$a" ;;
    esac
done

if [ -z "$PORT" ]; then
    for p in /dev/ttyACM0 /dev/ttyUSB0 /dev/tty.usbmodem* /dev/tty.usbserial*; do
        [ -e "$p" ] && { PORT="$p"; break; }
    done
fi
if [ -z "$PORT" ]; then
    echo "No serial port found. Pass it explicitly, e.g.: ./flash.sh /dev/ttyACM0"
    exit 1
fi

# Pick whatever esptool is available.
if   command -v esptool.py >/dev/null 2>&1; then ESPTOOL="esptool.py"
elif command -v esptool    >/dev/null 2>&1; then ESPTOOL="esptool"
else ESPTOOL="python3 -m esptool"
fi

for f in bootloader.bin partition-table.bin ota_data_initial.bin bugne.bin; do
    [ -f "$DIR/$f" ] || { echo "Missing $f next to this script."; exit 1; }
done

echo "Flashing Bugne to $PORT ..."
if [ "$ERASE" = "1" ]; then
    echo "Erasing flash first (Wi-Fi credentials and config will be cleared)..."
    $ESPTOOL --chip esp32s3 -p "$PORT" erase_flash
fi

$ESPTOOL --chip esp32s3 -p "$PORT" -b 460800 --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
    0x0     "$DIR/bootloader.bin" \
    0x8000  "$DIR/partition-table.bin" \
    0xf000  "$DIR/ota_data_initial.bin" \
    0x20000 "$DIR/bugne.bin"

echo
echo "Done. The device reboots into Bugne."
echo "With no Wi-Fi stored it raises the 'Bugne-Setup-XXXX' hotspot: scan the QR"
echo "codes on screen to join it and open the config page, then set Wi-Fi."
