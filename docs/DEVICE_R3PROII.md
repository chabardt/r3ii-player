# HiBy R3 Pro II — verified hardware profile

All facts below are confirmed on the real device over ADB (root shell), build
`R3PROII 1.2`, kernel built Jul 23 2025. This supersedes the R3 II (`r3ii.upt`)
analysis for anything hardware-specific.

## SoC / CPU / OS
- **SoC:** Ingenic **X1600** (`ingenic,x1600_halley6_module_base`), XBurst2.
- **CPU ABI:** MIPS32r2, **O32, hard-float, little-endian** (`e_flags=0x70001005`);
  the X1600 **has an FPU**. Our musl cross-toolchain (`-march=mips32r2 -mabi=32
  -mhard-float`, static, `e_flags=0x70001007`) is verified to run here.
- **Kernel:** Linux **4.4.94+**, PREEMPT.
- **Userland:** Buildroot 2020.02, **glibc 2.22** (`ld-2.22.so`). We build static, so
  libc version is irrelevant.
- **RAM:** TBD (probe `/proc/meminfo`); treat as tight.

## Storage / filesystems
- **Rootfs `/`:** **squashfs, read-only**, 100% full (~32 MB). Cannot write to `/`.
- **`/usr/data`:** **ubifs, read-write** (`/dev/ubi1_0`), ~36 MB, ~23 MB free. Our
  writable area; holds BT pairings, `user.ini`, `theme_id`, `eq.ini`,
  `usrlocal_media.db` (~19 MB media scan), wifi config.
- **SD card:** `/usr/data/mnt/sd_0` (vfat), i.e. the card mounts **under /usr/data**
  (not `/mnt/sd_0` like the R3 II). The user's card holds `Music/` and `Others/`.

## Display
- **`/dev/fb0`**, driver `jzfb`. Visible **480×720**, **16bpp RGB565**, stride **960**.
- `yres_virtual=1440` (double-buffered) → can pan for vsync/double buffering.

## Touch — `/dev/input/event1` (`goodix-ts`)
- **MT protocol type A**: `ABS_MT_POSITION_X`(53), `ABS_MT_POSITION_Y`(54),
  `ABS_MT_TOUCH_MAJOR`(48), `ABS_MT_TRACKING_ID`(57), then `SYN_MT_REPORT`(2) +
  `SYN_REPORT`(0). `BTN_TOUCH`(330) = press(1)/release(0).
- **Coordinates are direct screen pixels, no scaling**: X 0–479, Y 0–719
  (corner taps: TL 56,38 · TR 464,35 · BR 447,710 · BL 43,708).

## Physical keys
- `event0` `md-gpio-keys`, `event2` `jz adc keyboard`, `event3` `earpods_adc`
  (power / volume / multifunction / earpod remote). Map exact codes later.

## Audio
- ALSA: `controlC0`, playback **`pcmC0D0p`** (DAC), capture `pcmC0D1c`, `pcmC0D1p`.

## UI launch / dev access
- Launcher: `/etc/init.d/S92_03_start_music_player` → `/usr/bin/hiby_player.sh` →
  runs `/usr/bin/hiby_player` then **`reboot` on exit** (kill the script first or it
  reboots). No `/ui_data` here.
- **ADB:** enabled by Settings → About → tap "About" 10× ("Test Mode"). Works on
  stock. ADB and USB-mass-storage share one interface (drive hidden while ADB on).
- Dev loop: `adb push` to `/usr/data`, kill the player (script+binary via `/proc`
  cmdline scan — avoid matching your own shell), run our binary, restart
  `hiby_player.sh` after.

## Standalone bring-up — RESOLVED
A standalone app (player killed) must do two things at startup:
1. **`ioctl(fb, FBIOBLANK, FB_BLANK_UNBLANK)`** — killing `hiby_player` blanks the
   panel and the gt9xx driver suspends with it; unblanking wakes panel + touch.
   (`gt9xx_touch` is a loaded module, always present; it's the fb-blank that gates it.)
2. **Handle double buffering:** `yres_virtual = 2*yres`. The panel may show the page
   we're not drawing. Pan to our page with `FBIOPAN_DISPLAY` (xoffset/yoffset=0) and/or
   draw to both pages. Confirmed: with unblank + pan, drawing is visible and **touch
   works fully with the player killed** (verified full-screen pixel-accurate taps).
- Backlight: `/sys/class/backlight/backlight_pwm0/brightness` (0..?, was 5).

## Deployment (persistent)
- Rootfs is squashfs → persistent install needs a **squashfs repack + `.upt` flash**
  (community tool `hiby-modding/hiby-r3proii-custom-firmware` uses `mksquashfs` +
  `xorriso`; update file named **`r3proii.upt`** on SD root, flashed by holding
  **Vol-Up + Power**). Rollback = HiBy official stock `.upt`. Get the correct R3 Pro II
  stock firmware for base + rollback.
