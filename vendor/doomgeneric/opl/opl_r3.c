// OPL driver for the R3 (r3ii): Chocolate Doom's opl_sdl.c with SDL replaced by
// pthreads. Instead of an SDL audio callback, the r3snd mixer thread calls
// OPL_R3_Mix() each period; it runs the OPL timer callbacks (the MIDI sequencer) at
// sample-accurate times and renders Nuked OPL3 output at 48 kHz stereo.
// GPLv2 like the original (Copyright(C) 2005-2014 Simon Howard).

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Emulator: DOSBox DBOPL by default (cheap enough for the X1600 next to Doom: Nuked
// OPL3 measured ~27% CPU on top of the game). Build with -DR3_NUKED for Nuked OPL3.
#ifdef R3_NUKED
#include "opl3.h"
#else
#include "dbopl.h"
typedef Chip opl3_chip;
static Bit32s dbopl_buf[2 * 4096];
static int dbopl_opl3;
static void OPL3_Reset(opl3_chip *c, uint32_t rate)
{
    DBOPL_InitTables();
    Chip__Chip(c);
    Chip__Setup(c, rate);
}
static void OPL3_WriteRegBuffered(opl3_chip *c, uint16_t reg, uint8_t v)
{
    if (reg == 0x105) dbopl_opl3 = v & 1;
    Chip__WriteReg(c, reg, v);
}
static void OPL3_GenerateStream(opl3_chip *c, int16_t *out, uint32_t n)
{
    while (n > 0)
    {
        uint32_t k = n > 4096 ? 4096 : n, i;
        if (dbopl_opl3)
        {
            Chip__GenerateBlock3(c, k, dbopl_buf);
            for (i = 0; i < 2 * k; ++i)
            {
                Bit32s s = dbopl_buf[i];
                out[i] = (int16_t) (s > 32767 ? 32767 : s < -32768 ? -32768 : s);
            }
        }
        else
        {
            Chip__GenerateBlock2(c, k, dbopl_buf);
            for (i = 0; i < k; ++i)
            {
                Bit32s s = dbopl_buf[i];
                int16_t v = (int16_t) (s > 32767 ? 32767 : s < -32768 ? -32768 : s);
                out[2 * i] = out[2 * i + 1] = v;
            }
        }
        out += 2 * k;
        n -= k;
    }
}
#endif
#include "opl.h"
#include "opl_internal.h"
#include "opl_queue.h"

#define R3_OPL_RATE 48000

typedef struct { unsigned int rate, enabled, value; uint64_t expire_time; } opl_timer_t;

static pthread_mutex_t callback_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t callback_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static opl_callback_queue_t *callback_queue;
static uint64_t current_time, pause_offset;
static int opl_r3_paused, opl_ready;
static opl3_chip opl_chip;
static int opl_opl3mode;
static int register_num = 0;
static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125, 0, 0, 0 };
static const int mixing_freq = R3_OPL_RATE;

static void AdvanceTime(unsigned int nsamples)
{
    opl_callback_t callback;
    void *callback_data;
    uint64_t us;

    pthread_mutex_lock(&callback_queue_mutex);
    us = ((uint64_t) nsamples * OPL_SECOND) / mixing_freq;
    current_time += us;
    if (opl_r3_paused)
        pause_offset += us;

    while (!OPL_Queue_IsEmpty(callback_queue)
        && current_time >= OPL_Queue_Peek(callback_queue) + pause_offset)
    {
        if (!OPL_Queue_Pop(callback_queue, &callback, &callback_data))
            break;
        // hold callback_mutex (not the queue mutex) while the callback runs: it may
        // schedule new callbacks, and the game thread uses OPL_Lock() to exclude us.
        pthread_mutex_unlock(&callback_queue_mutex);
        pthread_mutex_lock(&callback_mutex);
        callback(callback_data);
        pthread_mutex_unlock(&callback_mutex);
        pthread_mutex_lock(&callback_queue_mutex);
    }
    pthread_mutex_unlock(&callback_queue_mutex);
}

// Fill `frames` stereo frames of music into buffer (overwrites). Called by r3snd.
void OPL_R3_Mix(int16_t *buffer, unsigned int frames)
{
    unsigned int filled = 0;

    if (!opl_ready)
    {
        memset(buffer, 0, frames * 4);
        return;
    }
    while (filled < frames)
    {
        uint64_t nsamples;

        pthread_mutex_lock(&callback_queue_mutex);
        if (opl_r3_paused || OPL_Queue_IsEmpty(callback_queue))
        {
            nsamples = frames - filled;
        }
        else
        {
            uint64_t next = OPL_Queue_Peek(callback_queue) + pause_offset;
            nsamples = next > current_time ? (next - current_time) * mixing_freq : 0;
            nsamples = (nsamples + OPL_SECOND - 1) / OPL_SECOND;
            if (nsamples > frames - filled)
                nsamples = frames - filled;
        }
        pthread_mutex_unlock(&callback_queue_mutex);

        if (nsamples)
        {
            pthread_mutex_lock(&callback_mutex);
            OPL3_GenerateStream(&opl_chip, buffer + filled * 2, nsamples);
            pthread_mutex_unlock(&callback_mutex);
            filled += nsamples;
        }
        AdvanceTime(nsamples);
    }
}

static void OPL_R3_Shutdown(void)
{
    opl_ready = 0;
    if (callback_queue != NULL)
    {
        OPL_Queue_Destroy(callback_queue);
        callback_queue = NULL;
    }
}

static int OPL_R3_Init(unsigned int port_base)
{
    (void) port_base;
    opl_r3_paused = 0;
    pause_offset = 0;
    current_time = 0;
    callback_queue = OPL_Queue_Create();
    OPL3_Reset(&opl_chip, mixing_freq);
    opl_opl3mode = 0;
    opl_ready = 1;
    return 1;
}

static unsigned int OPL_R3_PortRead(opl_port_t port)
{
    unsigned int result = 0;

    if (port == OPL_REGISTER_PORT_OPL3)
        return 0xff;
    if (timer1.enabled && current_time > timer1.expire_time)
        result |= 0xc0;
    if (timer2.enabled && current_time > timer2.expire_time)
        result |= 0xa0;
    return result;
}

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    if (timer->enabled)
    {
        int tics = 0x100 - timer->value;
        timer->expire_time = current_time + ((uint64_t) tics * OPL_SECOND) / timer->rate;
    }
}

static void WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            timer1.value = value;
            OPLTimer_CalculateEndTime(&timer1);
            break;
        case OPL_REG_TIMER2:
            timer2.value = value;
            OPLTimer_CalculateEndTime(&timer2);
            break;
        case OPL_REG_TIMER_CTRL:
            if (value & 0x80)
            {
                timer1.enabled = 0;
                timer2.enabled = 0;
            }
            else
            {
                if ((value & 0x40) == 0)
                {
                    timer1.enabled = (value & 0x01) != 0;
                    OPLTimer_CalculateEndTime(&timer1);
                }
                if ((value & 0x20) == 0)
                {
                    timer2.enabled = (value & 0x02) != 0;
                    OPLTimer_CalculateEndTime(&timer2);
                }
            }
            break;
        case OPL_REG_NEW:
            opl_opl3mode = value & 0x01;
            /* fall through */
        default:
            OPL3_WriteRegBuffered(&opl_chip, reg_num, value);
            break;
    }
}

static void OPL_R3_PortWrite(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT)
        register_num = value;
    else if (port == OPL_REGISTER_PORT_OPL3)
        register_num = value | 0x100;
    else if (port == OPL_DATA_PORT)
        WriteRegister(register_num, value);
}

static void OPL_R3_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    pthread_mutex_lock(&callback_queue_mutex);
    OPL_Queue_Push(callback_queue, callback, data, current_time - pause_offset + us);
    pthread_mutex_unlock(&callback_queue_mutex);
}

static void OPL_R3_ClearCallbacks(void)
{
    pthread_mutex_lock(&callback_queue_mutex);
    OPL_Queue_Clear(callback_queue);
    pthread_mutex_unlock(&callback_queue_mutex);
}

static void OPL_R3_Lock(void)   { pthread_mutex_lock(&callback_mutex); }
static void OPL_R3_Unlock(void) { pthread_mutex_unlock(&callback_mutex); }
static void OPL_R3_SetPaused(int paused) { opl_r3_paused = paused; }

static void OPL_R3_AdjustCallbacks(float factor)
{
    pthread_mutex_lock(&callback_queue_mutex);
    OPL_Queue_AdjustCallbacks(callback_queue, current_time, factor);
    pthread_mutex_unlock(&callback_queue_mutex);
}

opl_driver_t opl_r3_driver =
{
    "R3",
    OPL_R3_Init,
    OPL_R3_Shutdown,
    OPL_R3_PortRead,
    OPL_R3_PortWrite,
    OPL_R3_SetCallback,
    OPL_R3_ClearCallbacks,
    OPL_R3_Lock,
    OPL_R3_Unlock,
    OPL_R3_SetPaused,
    OPL_R3_AdjustCallbacks,
};
