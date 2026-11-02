# r3ii-player — minimalist drop-in music UI for HiBy R3 II

A clean, minimalist replacement music UI for the HiBy R3 II, installed as a
**reversible drop-in** beside the stock firmware. Later packaged as a flashable
`.upt` image.

## Target hardware (from stock firmware `r3ii.upt`, 2024-12-20)

| Property | Value |
|----------|-------|
| SoC | Ingenic (MIPS32r2, XBurst), little-endian |
| ABI | O32, **hard-float**, `e_flags=0x70001005` |
| libc | glibc 2.16 (Ingenic 2015.02), `ld-2.16.so` |
| Kernel | Linux 3.10.14 |
| RAM | 32 MB (`mem=32M`) — memory is tight |
| Display | `/dev/fb0` framebuffer (res/format TBD on device) |
| Touch | GT9xx capacitive, `/dev/input/eventX` |
| Audio | ALSA (`libasound`), HiBy-driven DAC |
| Stock UI | `hiby_player` — SDL 1.2 app (we replace this) |

## Hardware controls, audio routing & resume

- **Power key:** short press toggles **Now Playing ↔ previous screen**; **long press**
  = power on/off (sleep). **Volume rocker** = volume. A side key = **play/pause**.
- **Resume:** on power-off, persist the current item + position (+ playing state) to
  `/data`; on boot, restore it so it feels like resuming — the player opens on Now
  Playing with the same track.
- **Primary use case:** iPhone → **Bluetooth (A2DP sink)** → R3 II → **3.5mm jack**
  → powered speaker. The R3 II is a Bluetooth DAC feeding a wired speaker.
- **Quick Switch** panel (reachable by tapping the status bar — one or two taps from
  anywhere), two selectors:
  - **Output:** Headphone **Jack** ↔ **BT speaker** (fast switch between remembered
    speakers).
  - **Source:** **SD library** ↔ **Bluetooth-in (iPhone)**.
- **BT speaker works out of the box:** once paired, the remembered speaker
  **auto-connects on boot** (BlueZ bond + auto-reconnect); no menu diving.
- Audio streams to **one** BT device at a time (A2DP/bluealsa limit) — remember many,
  switch fast; not simultaneous. Source vs output are separate roles.

## Performance is the #1 requirement

User's top priority: **no lag** — fast boot, smooth scrolling — with a **large**
music collection. The binding constraint is **32 MB RAM** on a single ~1 GHz MIPS core.

Design rules (non-negotiable):
- **No boot-time library scan / media DB.** Music and Podcasts are *on-demand
  directory browsers*: `readdir` one folder when it's opened. Boot and nav cost are
  independent of collection size. (This is the legacy player's main stall.)
- **Never hold the collection in RAM.** Virtualized lists — only visible rows exist
  as widgets; one directory's entries in memory at a time; lazy sort for huge folders.
- **Partial/dirty-rect rendering** to `/dev/fb0` (LVGL), never full-screen blits.
- **Lazy cover art**: decode only visible thumbnails; cache as small RGB565 on
  `/data`; evict. Never bulk-decode a folder.
- **Single static binary** — no dynamic-linker startup cost (legacy links ~30 .so).
- Optional later: a tiny *incremental* folder cache (by dir mtime) on `/data` to make
  re-opening big folders instant — only if measurements show it's needed.
- Keep animations minimal; target a steady, tear-free redraw via fb pan/double-buffer.

## Strategy: avoid the old glibc

Build **fully-static mipsel binaries** (target: `mips32r2`, O32, hard-float) so
we depend only on the stable kernel syscall ABI, not glibc 2.16. Likely a musl
cross-toolchain; fallback is a glibc toolchain using the extracted rootfs as
sysroot.

## Injection point (reversible)

`/etc/init.d/S92_03_start_music_player` runs `/ui_data/player` if it exists, else
the stock `hiby_player`. So we install our binary at `/ui_data/player`. Undo =
delete that file. No stock files touched.

## Architecture

- **One static binary.** UI + audio + library index in a single process (32 MB RAM).
- **UI:** LVGL on fbdev + evdev. Screens: Artists → Albums → Songs → Now Playing
  (with album cover) → minimal Settings.
- **Audio:** miniaudio (ALSA backend) + decoders FLAC / MP3 / Vorbis / Opus / WAV,
  output to the hardware DAC device.
- **Browsing model:** both Music and Podcasts are **directory browsers** that mirror
  the SD folders directly (no tag database needed). Tags are read only to display
  track title/duration and cover; grouping comes from the folder layout.
- **SD card layout (two sources):**
  - `music/` → browsed as its own directory tree (Artist/Album/Song if laid out so).
  - `others/` → **Podcasts**: kids' stories & podcasts, one folder per show
    (`bestioles`, `Coluche`, `Francois Perusse`, `oli`, …).
  - Cover art per folder from `folder.jpg`/`cover.jpg` or embedded tag.
  - Resume-position memory per item (long stories/episodes).
- **Bluetooth (both directions):**
  - Transmit (A2DP **source**) → pair/connect a HiFi BT speaker / headphones.
  - Receive (A2DP **sink**, "Bluetooth DAC") → iPhone/iPod streams into the R3 II.
  - Driven via BlueZ `bluetoothd` (D-Bus or `bluetoothctl`) + bluealsa (stock has it).
- **Games (main-menu item):** Tetris (written from scratch) and Doom
  (doomgeneric; **user supplies the WAD** — no copyrighted data shipped).
  - **Doom WADs** (user's own, 2024 KEX re-release): use classic IWADs
    `doom.wad` / `doom2.wad` (also plutonia/tnt + PWADs sigil/nerve). Skip the
    ID24 "Legacy of Rust" files (`id1.wad`, `id24res.wad`, …) — need the KEX engine.
  - **Engine:** doomgeneric; implement `DG_Init/DrawFrame/SleepMs/GetTicksMs/GetKey`.
    Render 320×200 → nearest ×1.5 → 480×300 letterbox; extend port with **mouse-event
    injection** for analog turning.
  - **Layout:** portrait — game on top, touch control deck below.
  - **Input:** multitouch (evdev `ABS_MT_*`) left thumbstick (move+strafe, auto-run);
    **swipe-right = analog turn**; on-screen Fire; physical keys: play/pause = Use,
    volume rocker = weapon prev/next, power short = Esc/menu.

## Minimalist design

- **Main menu (4 items):** Music · Podcasts · Games · Settings.
- **Music** → browses the `music/` **directory tree as-is** ("same as the
  directories there"): folder → subfolder → tracks → player. If the user's folders
  are Artist/Album/Song, that's exactly what shows. Cover from folder art or tag.
- **Podcasts** → browses the `others/` directory tree (e.g. `bestioles`, `Coluche`,
  `Francois Perusse`, `oli`, …): folder → items → player, with
  **resume-from-last-position** per item.
- **Games** → Doom (user-supplied WAD), Tetris (from scratch).
- **Theme:** two skins — **Cream** (default retro) and **Dark = black + orange**.
  Token-driven; selectable under Settings → Display.
- **Settings** (5 items):
  - **Set BT sink** — enable/receive: device acts as A2DP sink / "Bluetooth DAC"
    for an iPhone/iPod streaming in.
  - **Set BT devices** — scan/pair/connect A2DP output (HiFi speaker, headphones).
    **Remembers all pairings** (BlueZ bonds in `/var/lib/bluetooth`); one marked
    **auto-reconnect**, reconnected on boot. Audio streams to **one** device at a
    time (A2DP/bluealsa limit) — many remembered, fast switch, not simultaneous.
  - **Sounds** — volume, gain/output options (and EQ if feasible).
  - **Display** — brightness, screen timeout, (rotation if needed).
  - **Hour / date-time** — set the clock.
- Player screen: large album cover, title/artist, play/pause, prev/next, seek,
  volume. Nothing else.
- Settings screen: Bluetooth only (transmit pair + receive toggle).

## Milestones

0. **Device access** — adb up; probe fb0 (res/bpp/format), touch evdev range,
   ALSA device names, BlueZ/bluealsa layout. *(needs device)*
1. **Toolchain + bring-up** — static mipsel hard-float cross-compiler; "hello
   framebuffer" + touch-dump; verify on device via adb push/run.
2. **Core player** — LVGL skeleton + miniaudio playing one file; Now Playing + cover.
3. **Directory browsers** — `music/` and `others/` folder navigation, cover art,
   track metadata, resume positions for podcasts.
4. **Transport + theme** — play/pause/seek/next/prev/volume; minimalist theme.
5. **Settings** — Set BT sink, Set BT devices, Sounds, Display, Hour/date-time.
6. **Games** — Tetris, then Doom.
7. **Packaging** — reversible drop-in installer; then flashable `.upt`.

## Risks / open questions

- DAC gain/digital-filter control lives in `hiby_player`; plain ALSA gives audio
  but maybe not every hardware feature. Confirm on device.
- BT routing depends on how HiBy wired bluealsa; confirm on device.
- Display rotation/pixel format must be confirmed on hardware.
- Flashing `.upt` can brick the device → drop-in first, flash only once proven.

## Status

- [x] Analyzed stock `r3ii.upt`, extracted rootfs, pinned toolchain target.
- [ ] adb access to device (blocked: adb not installed + device USB mode).
- [ ] Toolchain acquisition (blocked: sandbox denied external download).
