#!/bin/sh
# stop the stock UI without triggering its reboot-on-exit (kill script first)
SELF=$$
for d in /proc/[0-9]*; do
  pid=${d#/proc/}; [ "$pid" = "$SELF" ] && continue
  c=$(tr "\0" " " < "$d/cmdline" 2>/dev/null)
  case "$c" in *hiby_player.sh*) kill -9 "$pid" 2>/dev/null;; esac
done
kill -9 $(pidof hiby_player 2>/dev/null) 2>/dev/null
sleep 1
/usr/data/fbtest "${1:-20}" > /usr/data/fbtest.log 2>&1
# restore stock UI
/usr/bin/hiby_player.sh >/dev/null 2>&1 &
