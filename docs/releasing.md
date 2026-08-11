# Releasing an update on GitHub

A device installs the latest GitHub release from its own web page. Open the
Settings tab, check for a release, then install it with one click.

Publish a release in two steps:

1. Bump `version.txt`, which carries the app version embedded in the image, and
   commit it.
2. Build, then create a GitHub release on the public releases repository
   (Tupile/bugne-releases, see `GH_OTA_URL` in `web_config.c`). Tag it with the
   same version, `v1.0.1` for example. Attach `build/bugne.bin` as an asset
   named exactly `bugne.bin`. Attach the first-flash bundle as a second asset
   named exactly `bugne-flash.zip`. That zip holds `bootloader.bin`,
   `partition-table.bin`, `ota_data_initial.bin` and `bugne.bin` from `build/`,
   plus `tools/flash.sh` renamed to `flash.sh`, all at the root of the zip.

The device compares the version embedded in the release binary with its own
version. Any difference offers the update, so an older version published on
purpose offers a downgrade. The bootloader reverts an image that crashes at
boot.
