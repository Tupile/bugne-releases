# Bugne quick start

[Version française](quickstart_fr.md)

From an empty desk to a kid listening to their first radio, in five steps:
buy one board, print a case, flash the firmware once, connect it to Wi-Fi,
add radios and podcasts. The full documentation lives in this repository;
each step below links to the relevant part.

## 1. What to buy

- The board: an **LCDWIKI ES3C28P** (use that exact reference). It is an
  ESP32-S3 with 16 MB flash and 8 MB PSRAM, a 2.8 inch capacitive touch
  screen, an audio codec, a microphone, a microSD slot and a USB port.
  The small speaker comes with the board. Nothing to solder.
- A USB data cable and a computer with Python installed (for the first
  flash only).
- A few small self-tapping screws to hold the board and close the case.
- Optional: a microSD card (FAT32) for your own music and offline podcast
  episodes.
- The board has a battery port and charger (single-cell 3.7 V LiPo), but
  battery operation is untested by the project so far and not recommended
  yet: power the device over USB.

## 2. What to 3D print: the seventies cabinet

<img src="../case/preview_seventies_face.png" alt="Seventies cabinet" height="200">

Print the four parts of the seventies cabinet from the [`case/`](../case)
folder:

- `es3c28p_seventies_corps.stl` (body)
- `es3c28p_seventies_capot.stl` (rear cover)
- `es3c28p_seventies_grille.stl` (speaker grille)
- `es3c28p_seventies_pied.stl` (stand)

It prints face down (front on the bed) with no supports. On a multi-color
printer, use `es3c28p_seventies_corps+grille.step` to print the grille
plate in a second color; a single color works too.

Two alternative designs (a plain two-piece case and a vintage radio
cabinet) live in the same [`case/`](../case) folder, along with the
CadQuery scripts that generate all of them.

## 3. Flash the firmware (USB, once)

A brand-new board needs one full flash over USB. All later updates install
over Wi-Fi from the web page, no cable needed.

1. Install esptool: `pip install esptool`.
2. Download `bugne-flash.zip` from the latest release at
   <https://github.com/Tupile/bugne-releases/releases/latest> and unzip it.
3. Connect the board to the computer over USB.
4. In the unzipped folder, run `./flash.sh --erase` (Linux/macOS). On
   Windows, run the `esptool` command written inside `flash.sh`.
5. If no serial port is found, hold the BOOT button while plugging in the
   USB cable, then run the script again.

When the script finishes, the device restarts into Bugne.

## 4. Connect it to Wi-Fi (follow the QR code)

1. Since the device knows no Wi-Fi network yet, it opens its own setup
   hotspot and shows a QR code on screen.
2. Scan that QR code with your phone. It joins the hotspot named
   `Bugne-Setup-XXXX` (the XXXX is unique to your device, and so is the
   hotspot password, which is embedded in the QR code).
3. The configuration page opens by itself after joining (if it does not,
   open `http://192.168.4.1` in the phone browser).
4. Choose your home Wi-Fi network (2.4 GHz) and enter its password. The
   device connects and the hotspot disappears.
5. From now on the configuration page is available on your home network at
   `http://bugne-xxxx.local`: scan the QR shown on the device under
   Settings, then "Config page (QR)", or type the address.

## 5. Add the first web radios and podcasts

Open `http://bugne-xxxx.local` from any phone or computer on the same
Wi-Fi.

**Radios tab**: search the public radio-browser.info directory and add
stations in one tap, or add one manually with its name and direct stream
URL. The stations appear immediately on the device's Web radios tile.

<img src="manual/img/en/web-radios.png" width="300">

**Podcasts tab**: add a podcast with its RSS feed URL. "Download new"
saves fresh episodes to the microSD card for offline listening.

<img src="manual/img/en/web-podcasts.png" width="300">

Recommended: on the Settings tab, set a page password so kids cannot open
the parent settings from their own devices.

## Going further

- [User manual](manual/en.md): everyday use, alarms, quiet hours, the
  times-tables game, the tuner, updates, troubleshooting.
- [Hardware notes](hardware.md): GPIO map and board details.
- [README](../README.md): feature list and build instructions from source.
