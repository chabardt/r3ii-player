#ifndef R3_PAD_H
#define R3_PAD_H
/* Bluetooth game controllers / remotes (Joy-Con, Pro Controller, DS4, 8BitDo...).
 * BlueZ turns a paired HID controller into /dev/input/eventN (uhid + hid-generic);
 * pad.c hot-plugs those nodes and turns their buttons into a few logical actions. */
enum { PA_UP, PA_DOWN, PA_LEFT, PA_RIGHT, PA_A, PA_B, PA_X, PA_Y,
       PA_L, PA_R, PA_PLUS, PA_MINUS, PA_HOME, PA_N };

void pad_poll(void);                 /* call from the main loop (hotplug + read) */
int  pad_next(int*action);           /* pop a press (with auto-repeat for directions); 1 if any */
int  pad_held(int action);           /* is it held right now */
float pad_stick_x(void);             /* -1..1, 0 in the dead zone */
int  pad_count(void);                /* connected controllers */
const char* pad_name(int i);
int  pad_battery(int i,int*charging);  /* 0..100 (Joy-Con: 25 % steps), -1 unknown */
/* last raw event, for the "test buttons" page: type/code/value + device name */
void pad_last_raw(int*type,int*code,int*value,const char**dev);
#endif
