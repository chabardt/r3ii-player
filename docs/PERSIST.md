# Persistent install (r3proii.upt)

The device's OTA (from its own /etc/ota_bin/*.sh) accepts an ISO9660 `.upt`:
`ota_config.in` (current_version=0) + `ota_v0/` { `ota_update.in`, `ota_v0.ok`,
chain-hashed chunks `rootfs.squashfs.NNNN.<prevmd5>` + `xImage.NNNN.<prevmd5>`,
manifests `ota_md5_*.<overallmd5>` }. Chunk size 520997. Build: `scripts/build_upt.sh
<rootfs.squashfs> <kernel.img> <out.upt>`.

rootfs = squashfs `mksquashfs -comp lzo -Xalgorithm lzo1x_999 -Xcompression-level 9
-b 131072 -noappend`. Source = dump of the device's live rootfs (mtd2), modified so
`/usr/bin/hiby_player.sh` runs `/usr/data/r3player` in a loop (fallback to stock).
r3player lives on `/usr/data` (mtd6 userdata, untouched by a rootfs flash) → update it
later with `adb push` + kill (the loop restarts it), no reflash.

Partitions: mtd1 kernel / mtd2 rootfs = slot A (45MB, active); mtd3 kernel2 / mtd4
rootfs2 = slot B (24MB, recovery, intact). mtd5 "ota" string picks the slot. The
Vol-Up+Power recovery updater writes the main OS to slot A (45MB) — our 32MB rootfs fits.

Flash: copy image to SD root as `r3proii.upt`, power off, hold Vol-Up + press Power,
wait for success, reboot. Rollback: flash HiBy's official stock `.upt` the same way.
Validate with the stock-rebuild image first.
