# Bugne quick start

[Version française](quickstart_fr.md)

This guide goes from an empty desk to a child who listens to their first web
radio. It has five steps: buy one board, print a case, flash the firmware once,
connect the device to Wi-Fi, and add the content. The full documentation is in
this repository. Each step below links to the related part.

## 1. What to buy

- The board: an **LCDWIKI ES3C28P**. Use that exact reference. It is an
  ESP32-S3 with 16 MB flash and 8 MB PSRAM, a 2.8 inch capacitive touch screen,
  an audio codec, a microphone, a microSD slot and a USB port. The small
  speaker comes with the board. You do not solder anything. If you wish to
  support the project at no extra cost, you can order the board via this
  [Aliexpress affiliate link](https://s.click.aliexpress.com/e/_c4OZeS8F)
  (or this [alternative link](https://s.click.aliexpress.com/e/_c3MmlBCJ)
  if unavailable). Make sure to select the touch model "ES3C28P". For users
  in France, you can also use this [Amazon affiliate link](https://amzn.to/3RrzKT1).
- A USB data cable and a computer. You need them for the first flash only.
- 4x M3 6mm screws for the board, and 4x M3 10mm screws for the cover. If you
  don't have them, you can find them
  [here](https://s.click.aliexpress.com/e/_c34zawnh).
- Optional: a microSD card (FAT32) for your own music and for offline podcast
  episodes (like [this one](https://s.click.aliexpress.com/e/_c2yej75h) or
  [this one](https://s.click.aliexpress.com/e/_c3ywvSmJ); for users in France,
  you can also use this [Amazon affiliate link](https://amzn.to/3Ta5I6J)).
- The board has a battery port and a charger for a single-cell 3.7 V LiPo.
  This project did not test battery operation, and does not recommend it yet.
  Power the device over USB.

## 2. What to 3D print: the seventies cabinet

<img src="../case/preview_seventies_face.png" alt="Seventies cabinet" height="200">

Print the three parts of the seventies cabinet from the [`case/`](../case)
folder. If you have a BambuLab printer, download them from
[MakerWorld](https://makerworld.com/en/models/3073793-bugne-open-source-internet-radio-podcast-player)
instead:

- `es3c28p_seventies_corps.stl` (body)
- `es3c28p_seventies_capot.stl` (rear cover)
- `es3c28p_seventies_grille.stl` (speaker grille)

Print it face down, front on the bed, with no supports. On a multi-color
printer, use `es3c28p_seventies_corps+grille.step` to print the grille plate in
a second color. A single color works too.

If you do not have a 3D printer, a service such as PCBWay or Craftcloud prints
the case and delivers it. For the seventies model, order
`es3c28p_seventies_corps+grille.step` and `es3c28p_seventies_capot.stl` in PLA.

The same [`case/`](../case) folder holds two other designs, a plain two-piece
case and a vintage radio cabinet, with the CadQuery scripts that generate all
of them.

## 3. Flash the firmware (USB, once)

A new board needs one full flash over USB. All later updates install over Wi-Fi
from the web page, with no cable.

1. Connect the board to your computer over USB. Hold the BOOT button while you
   plug the cable in, to enter bootloader mode.
2. Open the [Web Flasher](https://tupile.github.io/bugne-releases/tools/web-flasher/)
   page with Chrome, Edge or Opera.
3. Click "Installer", select the COM port of the board, and wait for the end of
   the installation.

*(Note: you can also flash offline with `bugne-flash.zip` and `esptool`. The
[README](../README.md) gives the commands.)*

At the end of the installation the device restarts into Bugne.

## 4. Connect it to Wi-Fi (follow the QR code)

1. The device knows no Wi-Fi network yet. It opens its own setup hotspot and
   shows a QR code on the screen.
2. Scan that QR code with your phone. Your phone joins the hotspot
   `Bugne-Setup-XXXX`. The XXXX is unique to your device, and so is the hotspot
   password, which the QR code carries.
3. The web page opens by itself after the phone joins. If it does not, open
   `http://192.168.4.1` in the phone browser.
4. Select your home Wi-Fi network (2.4 GHz) and enter its password. The device
   connects and the hotspot stops.
5. The web page is now on your home network, at `http://bugne-xxxx.local`. Type
   that address, or scan the QR code on the device under Settings, then
   "Config page (QR)".

## 5. Add the first web radios and podcast feeds

Open `http://bugne-xxxx.local` from any phone or computer on the same Wi-Fi.

**Radios tab**: search the public radio-browser.info directory and add a
station with one click. You can also add a station by hand with its name and
its stream URL. A new station shows at once on the Web radios tile of the
device.

<img src="manual/img/en/web-radios.png" width="300">

**Podcasts tab**: the "Find podcasts" box searches the Apple Podcasts
directory. Type a name, then add a result with one click: the page fills in
the title and the RSS address by itself. You can also paste any RSS address by
hand. "Download new" saves the fresh episodes to the microSD card for offline
listening.

<img src="manual/img/en/web-podcasts.png" width="300">

Recommended: on the Settings tab, set a page password. A child then cannot open
the parent settings from their own phone.

## Going further

- [User manual](manual/en.md): everyday use, alarms, quiet hours, the daily
  listening limit, the times-tables game, the tuner (experimental), updates and
  troubleshooting.
- [Hardware notes](hardware.md): GPIO map and board details.
- [README](../README.md): feature list, HTTP API and build instructions.
