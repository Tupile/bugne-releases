# Releasing an update on GitHub

Devices can install the latest GitHub release from their web page
(Firmware tab: check, then one-click install). Publish workflow:

1. Bump `version.txt` (the app version embedded in the image) and commit.
2. Build, then create a GitHub release on the public releases repo
   (Tupile/bugne-releases, see GH_OTA_URL in web_config.c), tag it the same
   version (e.g. `v1.0.1`) and attach `build/bugne.bin` as an asset named
   exactly `bugne.bin`. Also attach the first-flash bundle as a second
   asset named exactly `bugne-flash.zip`: a zip of `bootloader.bin`,
   `partition-table.bin`, `ota_data_initial.bin`, `bugne.bin` (from
   `build/`) and `tools/flash.sh` (as `flash.sh`), all at the zip root.

The device compares the release binary's embedded version with its own:
any difference offers the update (so publishing an older version offers a
deliberate downgrade). A crash-looping image is rolled back automatically.
