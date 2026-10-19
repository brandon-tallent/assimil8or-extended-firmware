/* The pages the extension adds to the module's panel, and the stock drawing
 * they share with the EQ pages. Phase Mod on its own page
 * steps through them in list order (eq_panel.c), which owns the field
 * cursor, the ENTER edit state, the channel strip and the screen transfer.
 * Their callbacks run in the panel and display interrupts. */
#ifndef PANEL_PAGE_H
#define PANEL_PAGE_H
#include "patch.h"

struct panel_page {
    u32 fields;
    void (*draw)(u32 channel, u32 field, u32 editing);
    void (*step)(u32 channel, u32 field, s32 delta);       /* encoder, while editing */
    void (*position)(u32 channel, u32 field, u32 q16);     /* DATA 2, 0..1 as Q16 */
    u32 (*revision)(void);                                 /* changes when the page must redraw */
    void (*sample)(u32 channel);                           /* the Sample button here, in place of a stock take */
    const struct panel_page *next;                         /* the page after this one, or null */
    u32 (*enter)(u32 channel, u32 field);                  /* ENTER, before the stock popup: nonzero if handled */
    void (*hold)(u32 channel, u32 field);                  /* ENTER held, or null */
    u32 (*count)(u32 channel);                             /* the fields now shown, or null for `fields` */
    u32 zoned;                                             /* edits per-zone settings: the zone label and EDIT scope */
    /* Redraws the graph (the EQ graph's box) when CV moved what it shows; nonzero
     * when it drew. Called at the EQ graph's live rate, or null. */
    u32 (*live)(u32 channel);
};
/* The pages Phase Mod steps to from its own page, and those Envelope steps to. */
extern const struct panel_page *volatile patch_panel_page;
extern const struct panel_page *volatile patch_envelope_page;
/* The EDIT scope the EQ pages share: editing a zone's settings (nonzero) or the channel's. */
u32 panel_scope_zone(void);
u32 panel_selected_zone(void);
u32 panel_selected_channel(void);
void panel_toggle_scope(void);

/* eq_panel.c: a page of CV routes in the stock style, two to a row. The
 * cursor counts each route's source, then its amount. */
void panel_cv_route(u32 route, const char *title, s32 source, s32 amount, u32 cursor);
void panel_cv_popup(u32 cursor, const char *name, s32 source, s32 amount, const char *per_volt);
/* One route's source and amount on a line at (x, y) with the amount's bar
 * under it; `selected` is 1 for the source, 2 for the amount. Its popup, the
 * popup's top at y. */
void panel_cv_pair(s32 x, s32 y, s32 source, s32 amount, u32 selected, u32 gray);
void panel_cv_popup_at(s32 y, u32 is_amount, const char *name, s32 source, s32 amount, const char *per_volt);

void ui_draw_proportional_text(u32 font, s32 x, s32 y, const char *text,
                              u32 foreground, u32 background, u32 scale, u32 spacing);
void oled_shadow_fill_rect(u32 x, u32 y, u32 width, u32 height, u32 color);
void oled_enqueue_transfer(u32 kind, u32 x, u32 y, u32 width, u32 height);
void ui_draw_channel_mix_strip(u32 unused, u32 channel);
void ui_render_parameter_row(u32 style, s32 x, s32 y, const char *value, const char *title,
                             const char *suffix, u32 keep_frame, s32 color, u32 right_align);
void ui_draw_zone_label_blinking_when_synced(u32 zone, s32 x, s32 y);
void ui_draw_channel_current_zone_suffix(u32 *task);
/* Shared page objects and fonts. */
#define UI_PARAMETER_ROW_STYLE 0x800ac450u
#define UI_VALUE_BAR_OBJECT 0x800adaccu
#define UI_HEADING_FONT 0x800ac210u         /* UI_RENDER_CONTEXT + UI_RENDER_FONT_DCCC */
#define OLED_TRANSFER_BATCH_GATE 0x80348280u
#define COLOR_BLACK 0x0000u
#define COLOR_WHITE 0xffffu
#define COLOR_TEAL 0x1905u
#define COLOR_SELECTED 0xdbdeu
#define POPUP_BACKGROUND_FROM_STYLE -1
/* Layout (pixels): the page body below the meter band, and its columns. */
#define PAGE_TOP 11u
#define PAGE_HEIGHT (OLED_SIDE - PAGE_TOP)
#define LEFT_X 12
#define TITLE_Y 13
#define ZONE_LABEL_X 107
#define CV_COLUMN_X 70
#endif
