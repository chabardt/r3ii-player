doomgeneric (GPLv2, see LICENSE) from https://github.com/ozkl/doomgeneric
@ e257a09527062bfb986b6e5e0780839b6cbd5b71.

Trimmed for the R3: other platform backends, SDL/Allegro sound+music and their helpers
(gusconf, mus2mid, icon) removed. Built by scripts/build_doom.sh with
-DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200; the R3 backend is src/doom/r3doom.c.
No game data is included: the player copies their own WADs to Games/Doom on the SD card.
