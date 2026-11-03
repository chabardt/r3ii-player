#ifndef R3_COVER_H
#define R3_COVER_H
#include "lvgl.h"
/* Album art fitted (aspect kept, no crop) into boxw x boxh, as an in-memory RGB565 image.
 * Source: a cover image in dir, else the art embedded in track (ID3/FLAC/MP4).
 * Returns NULL if none. The previous result stays valid until the call after next. */
const lv_img_dsc_t* cover_load(const char*dir,const char*track,int boxw,int boxh);
#endif
