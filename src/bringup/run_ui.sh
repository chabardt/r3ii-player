#!/bin/sh
sh /usr/data/stopui.sh
sleep 1
/usr/data/r3player > /usr/data/r3player.log 2>&1 &
P=$!
sleep ${1:-40}
kill -9 $P 2>/dev/null
/usr/bin/hiby_player.sh >/dev/null 2>&1 &
