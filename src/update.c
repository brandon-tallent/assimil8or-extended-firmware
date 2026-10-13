#include "patch.h"

/* Update mode, entered when an install, restore or reboot begins. The module
 * fades its outputs to silence, stops acting on channel MIDI, and restarts
 * only once silent, so an update needs no coordination with playback. The
 * channel hook (eq.S) applies the gain to all eight channel samples before
 * the direct outputs and Mix/Pan. */

#define OUTPUT_GAIN_FULL          0x7fffffff
#define OUTPUT_FADE_FRAMES        480u /* 10 ms at 48 kHz */
#define SILENT_SETTLE_TICKS       20u  /* output DMA drains the last frames */

s32 patch_output_gain[3] = {
    OUTPUT_GAIN_FULL, OUTPUT_GAIN_FULL, OUTPUT_GAIN_FULL / OUTPUT_FADE_FRAMES
}; /* current, target, step per frame */
static u32 active, silent_since;

int update_active(void) { return active; }

void update_enter(void)
{
    active = 1; silent_since = 0;
    MEMORY_BARRIER();
    patch_output_gain[1] = 0;
}

void update_leave(void)
{
    active = 0;
    MEMORY_BARRIER();
    patch_output_gain[1] = OUTPUT_GAIN_FULL;
}

int update_silent(void)
{
    if (patch_output_gain[0]) { silent_since = 0; return 0; }
    if (!silent_since) silent_since = REG32(TICKS) | 1u;
    return (u32)(REG32(TICKS) - silent_since) >= SILENT_SETTLE_TICKS;
}
