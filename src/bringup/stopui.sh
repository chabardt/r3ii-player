#!/bin/sh
SELF=$$
for d in /proc/[0-9]*; do pid=${d#/proc/}; [ "$pid" = "$SELF" ] && continue
  c=$(tr "\0" " " < "$d/cmdline" 2>/dev/null)
  case "$c" in *hiby_player.sh*|*/usr/bin/hiby_player*) kill -9 "$pid" 2>/dev/null;; esac
done
