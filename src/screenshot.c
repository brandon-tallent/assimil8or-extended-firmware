#include "patch.h"

/* One screen transfer at a time, serialized by the host. The display task
 * copies a completed frame with audio interrupts enabled; the foreground
 * encodes it. Keep one reusable heap buffer rather than allocating in panel
 * context or reserving 64 KiB in the fixed overlay. */
enum { SCREEN_IDLE, SCREEN_REQUESTED, SCREEN_COPY, SCREEN_ENCODE, SCREEN_READY };
static volatile u32 state;
static u32 request_sequence, encoded_bytes;
static u8 *buffer;

/* Pixel PackBits: token low seven bits plus one is the pixel count. A set
 * high bit repeats the following two bytes; otherwise count literal RGB565
 * pixels follow. Preserve the framebuffer's byte order and every color. */
u32 screenshot_encode(const u16 *pixels, u8 *out)
{
    u32 at = 0, used = 0, total = OLED_SIDE * OLED_SIDE;
    while (at < total) {
        u32 run = 1;
        while (at + run < total && run < SCREENSHOT_LITERAL_PIXELS &&
               pixels[at + run] == pixels[at]) ++run;
        if (run >= 2) {
            out[used++] = (u8)(0x80u | (run - 1));
            out[used++] = (u8)pixels[at];
            out[used++] = (u8)(pixels[at] >> 8);
            at += run;
        } else {
            u32 first = at++;
            while (at < total && at - first < SCREENSHOT_LITERAL_PIXELS) {
                if (at + 1 < total && pixels[at] == pixels[at + 1]) break;
                ++at;
            }
            out[used++] = (u8)(at - first - 1);
            for (u32 i = first; i < at; ++i) {
                out[used++] = (u8)pixels[i];
                out[used++] = (u8)(pixels[i] >> 8);
            }
        }
    }
    return used;
}

/* Empty request captures a new frame and replies with its encoded length.
 * A u16 offset and u8 count read up to 126 encoded bytes. The final chunk
 * ends the transfer; a new capture can also replace an abandoned snapshot. */
void screenshot_command(u32 sequence, const u8 *p, u32 bytes)
{
    if (!bytes) {
        if ((state != SCREEN_IDLE && state != SCREEN_READY) || install_busy()) {
            reply(CMD_SCREENSHOT, sequence, BUSY, 0, 0); return;
        }
        request_sequence = sequence;
        MEMORY_BARRIER();
        state = SCREEN_REQUESTED;
        return;
    }
    u32 status = OK, offset = 0, count = 0;
    if (bytes != 3) status = BAD_LENGTH;
    else if (state != SCREEN_READY) status = BAD_STATE;
    else {
        offset = get_u16(p); count = p[2];
        if (!count || count > REPLY_BYTES - 1 || offset + count > encoded_bytes) {
            status = BAD_RANGE; count = 0;
        }
    }
    reply(CMD_SCREENSHOT, sequence, status,
          count ? buffer + OLED_FRAMEBUFFER_BYTES + offset : 0, count);
    if (!status && offset + count == encoded_bytes) state = SCREEN_IDLE;
}

/* Called after cdisplay_task_service returns, before another display pass
 * can draw. No IRQ masking: the 48 kHz renderer may preempt this copy. */
void screenshot_after_display(void)
{
    if (state != SCREEN_COPY) return;
    volatile const u16 *pixels = (volatile const u16 *)OLED_SHADOW_FRAMEBUFFER_BASE;
    u16 *snapshot = (u16 *)buffer;
    for (u32 i = 0; i < OLED_SIDE * OLED_SIDE; ++i) snapshot[i] = pixels[i];
    MEMORY_BARRIER();
    state = SCREEN_ENCODE;
}

void screenshot_foreground_poll(void)
{
    u32 sequence = request_sequence;
    if (state == SCREEN_REQUESTED) {
        if (!buffer) buffer = malloc(OLED_FRAMEBUFFER_BYTES + SCREENSHOT_ENCODED_BYTES);
        if (!buffer) {
            state = SCREEN_IDLE;
            reply(CMD_SCREENSHOT, sequence, NO_MEMORY, 0, 0);
            return;
        }
        MEMORY_BARRIER();
        state = SCREEN_COPY;
    } else if (state == SCREEN_ENCODE) {
        encoded_bytes = screenshot_encode((const u16 *)buffer, buffer + OLED_FRAMEBUFFER_BYTES);
        u8 length[4]; put_u32(length, encoded_bytes);
        MEMORY_BARRIER();
        state = SCREEN_READY;
        reply(CMD_SCREENSHOT, sequence, OK, length, sizeof(length));
    }
}
