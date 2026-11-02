#!/bin/sh
# Run ON the device (via: adb push probe.sh /tmp && adb shell sh /tmp/probe.sh)
# Gathers the hardware facts the UI/audio layers need. Read-only; changes nothing.
echo "===== uname";            uname -a
echo "===== framebuffer /dev/fb0"; ls -l /dev/fb*
cat /sys/class/graphics/fb0/virtual_size 2>/dev/null && echo " (virtual_size WxH)"
cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null && echo " (bpp)"
cat /sys/class/graphics/fb0/stride 2>/dev/null && echo " (stride)"
cat /sys/class/graphics/fb0/rotate 2>/dev/null && echo " (rotate)"
echo "===== input devices (touch = GT9xx)"; cat /proc/bus/input/devices 2>/dev/null
echo "===== ALSA cards";         cat /proc/asound/cards 2>/dev/null; ls /dev/snd 2>/dev/null
echo "===== ALSA pcm list";      cat /proc/asound/pcm 2>/dev/null
echo "===== Bluetooth: bluez/bluealsa present?"
for b in bluetoothd bluealsa bluealsa-aplay bluetoothctl hciconfig hcitool; do
  command -v $b >/dev/null 2>&1 && echo "  have: $b"
done
hciconfig -a 2>/dev/null | head -20
echo "===== /ui_data (our install dir) writable?"
ls -ld /ui_data 2>/dev/null || echo "  /ui_data missing (create it)"
echo "===== mounts (find SD card: music/ + others/)"
mount | grep -iE 'sd|mmc|media|mnt' 2>/dev/null
echo "===== date/time + rtc"
date; ls /dev/rtc* 2>/dev/null
echo "===== mem"
head -3 /proc/meminfo
echo "===== DONE"
