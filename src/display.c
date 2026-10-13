#include "patch.h"

/* High-contrast display modes.
 *
 * Text white normalizes every proportional glyph: the brighter of its
 * foreground and background becomes white and the other black, so ordinary
 * text is white on black and reverse-video selections stay inverted.
 * Everything white also turns every remaining non-black pixel white as each
 * shadow rectangle is queued for the panel, which covers lines, meters, and
 * waveforms. Both change the shadow image itself, so screenshots show them. */

static u32 display_mode = DISPLAY_MODE_STOCK;

static u32 luminance(u32 color)
{
    u32 rgb565 = ((color & BYTE_MASK) << 8) | ((color >> 8) & BYTE_MASK); /* stored byte-swapped */
    u32 red = rgb565 >> RGB565_RED_SHIFT;
    u32 green = (rgb565 >> RGB565_GREEN_SHIFT) & RGB565_GREEN_MASK;
    u32 blue = rgb565 & RGB565_BLUE_MASK;
    /* Rec. 601 weights, scaled for 5-6-5 component ranges. */
    return red * LUMA_RED_WEIGHT + green * LUMA_GREEN_WEIGHT + blue * LUMA_BLUE_WEIGHT;
}

/* colors[0] and colors[1] are the glyph call's foreground and background
 * stack arguments. Idempotent, so repeated calls from one text run agree. */
void display_glyph_colors(u32 *colors)
{
    if (display_mode == DISPLAY_MODE_STOCK) return;
    u32 foreground = colors[0] & OLED_PIXEL_MASK, background = colors[1] & OLED_PIXEL_MASK;
    if (foreground == background) return;
    int light_text = luminance(foreground) >= luminance(background);
    colors[0] = light_text ? OLED_PIXEL_WHITE : OLED_PIXEL_BLACK;
    colors[1] = light_text ? OLED_PIXEL_BLACK : OLED_PIXEL_WHITE;
}

void display_before_enqueue(u32 kind, u32 x, u32 y, u32 width, u32 height)
{
    if (display_mode != DISPLAY_MODE_ALL_WHITE || kind != OLED_TRANSFER_KIND_RECT) return;
    if (x >= OLED_SIDE || y >= OLED_SIDE) return;
    if (width > OLED_SIDE - x) width = OLED_SIDE - x;
    if (height > OLED_SIDE - y) height = OLED_SIDE - y;
    for (u32 row = y; row < y + height; ++row) {
        volatile u16 *pixel = (volatile u16 *)OLED_SHADOW_FRAMEBUFFER_BASE + row * OLED_SIDE + x;
        for (u32 column = 0; column < width; ++column, ++pixel)
            if (*pixel) *pixel = OLED_PIXEL_WHITE;
    }
}

/* Select a mode without forcing a redraw; the caller redraws. */
void display_select_mode(u32 mode)
{
    display_mode = mode;
}

int display_set_mode(u32 mode)
{
    if (mode > DISPLAY_MODE_ALL_WHITE) return 0;
    display_select_mode(mode);
    /* An impossible previous page makes the next display pass redraw the
     * whole current page in the new mode. */
    REG32(CDISPLAY_TASK_OBJECT + UI_PAGE_RENDER_PREVIOUS_PAGE) = ~0u;
    return 1;
}
