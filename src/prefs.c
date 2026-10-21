#include "patch.h"

/* Utilities > Preferences: High Contrast.
 *
 * A sixth row below Load Folder Warn toggles the all-white display mode. The
 * stock screen keeps rows 0..4 and states 0, 1, 10..14
 * and 30; this wrapper extends the browse cursor to row 5, owns edit state 15,
 * and leaves a press to the stock save state, which redraws the page in the
 * new mode. The setting lives in PREFERENCES_RUNTIME_STATE's reserved byte and
 * in prefs.yml as "HighContrast : 0|1", appended after the stock writer and
 * read after the stock loader; stock firmware ignores the unknown key. */

void preferences_ui_stock(u32 *screen);
void preferences_render_brightness_value(u32 inverted, u32 framed);
void preferences_render_screen_saver_timeout(u32 inverted, u32 framed);
void preferences_render_gate_delay_value(u32 inverted, u32 framed);
void preferences_render_auto_audition(u32 inverted, u32 framed);
void preferences_render_load_warning(u32 inverted, u32 framed);
void ui_draw_text_line_at_cursor(u32 *cursor, const char *text, u32 color, u32 attributes);
u32 ui_text_metric_measure(u32 font, const char *text, u32 scale, u32 spacing);
void ui_draw_proportional_text(u32 font, s32 x, s32 y, const char *text, u32 foreground,
                               u32 background, u32 scale, u32 spacing);
void ui_fill_outlined_box(u32 x, u32 y, u32 width, u32 height, u32 color);
extern const char preferences_on_text[], shared_off_text[];

/* About screen: draws stock's "SW Rev" line, then the release version on
 * the blank line under it. Uses a copy of the line cursor so the rest of
 * the stock page does not move. */
#define LINE_CURSOR_WORDS 3u  /* line index, first-line y, line height */
void about_version_line(u32 *cursor, const char *text, u32 color, u32 align)
{
    ui_draw_text_line_at_cursor(cursor, text, color, align);
    u32 copy[LINE_CURSOR_WORDS] = {cursor[0], cursor[1], cursor[2]};
    ui_draw_text_line_at_cursor(copy, patch_version_text, color, align);
}

static const char high_contrast_label[] = "High Contrast:";
static const char high_contrast_key[] = "HighContrast";

static u32 high_contrast(void)
{
    return BYTE(PREFERENCES_RUNTIME_STATE + PREFERENCES_RUNTIME_HIGH_CONTRAST) != 0;
}

static void apply_high_contrast(u32 enabled)
{
    BYTE(PREFERENCES_RUNTIME_STATE + PREFERENCES_RUNTIME_HIGH_CONTRAST) = (u8)enabled;
    display_select_mode(enabled ? DISPLAY_MODE_ALL_WHITE : DISPLAY_MODE_STOCK);
}

/* Mirrors preferences_render_auto_audition one row lower. */
static void render_high_contrast(u32 inverted, u32 framed)
{
    const char *text = high_contrast() ? preferences_on_text : shared_off_text;
    u32 font = UI_RENDER_DEFAULT_FONT_CONTEXT;
    u32 width = ui_text_metric_measure(font, text, 1, 0);
    u32 y = REG32(UI_SCREEN_CONTEXT + UI_SCREEN_CONTENT_Y_ORIGIN) +
        REG32(UI_SCREEN_CONTEXT + UI_SCREEN_CONTENT_ROW_PITCH) * PREFERENCES_HIGH_CONTRAST_LINE;
    if (framed)
        ui_fill_outlined_box(PREFERENCES_ON_OFF_FRAME_X, y - PREFERENCES_VALUE_FRAME_Y_BACK,
                             PREFERENCES_ON_OFF_FRAME_WIDTH,
                             REG32(font + UI_FONT_CONTEXT_BITMAP_ROWS), PREFERENCES_VALUE_FRAME_COLOR);
    ui_draw_proportional_text(font, (s32)(OLED_SIDE - 1 - width), (s32)y, text,
                              inverted ? OLED_PIXEL_BLACK : PREFERENCES_VALUE_FRAME_COLOR,
                              inverted ? PREFERENCES_VALUE_FRAME_COLOR : OLED_PIXEL_BLACK, 1, 0);
}

static void render_values(u32 cursor)
{
    preferences_render_brightness_value(cursor == PREFERENCES_ROW_BRIGHTNESS, 0);
    preferences_render_screen_saver_timeout(cursor == PREFERENCES_ROW_SCREEN_SAVER, 0);
    preferences_render_gate_delay_value(cursor == PREFERENCES_ROW_GATE_DELAY, 0);
    preferences_render_auto_audition(cursor == PREFERENCES_ROW_AUTO_AUDITION, 0);
    preferences_render_load_warning(cursor == PREFERENCES_ROW_LOAD_WARNING, 0);
    render_high_contrast(cursor == PREFERENCES_ROW_HIGH_CONTRAST, 0);
}

/* Edit state 15: encoder steps toggle, the DATA 2 knob sets Off below its
 * midpoint and On above, and a press saves through the stock state 30. */
static void edit_high_contrast(u32 *screen)
{
    u32 pressed = screen[UI_SCREEN_CONFIRM_WORD] != 0;
    s32 steps = (s32)screen[UI_SCREEN_ENCODER_WORD];
    screen[UI_SCREEN_CONFIRM_WORD] = screen[UI_SCREEN_ENCODER_WORD] = 0;
    if (steps) {
        s32 value = steps + (high_contrast() ? 1 : 0);
        apply_high_contrast(value > 0);
        render_high_contrast(1, 1);
    }
    u32 pending = REG32(UI_SCREEN_CONTEXT + UI_SCREEN_PENDING_CONTROL_VALUE);
    if (pending != REG32(UI_SCREEN_CONTEXT + UI_SCREEN_COMMITTED_CONTROL_VALUE)) {
        REG32(UI_SCREEN_CONTEXT + UI_SCREEN_COMMITTED_CONTROL_VALUE) = pending;
        apply_high_contrast(pending >= FLOAT_ONE_HALF_BITS); /* knob position 0..1 */
        render_high_contrast(1, 1);
    }
    if (pressed) screen[UI_SCREEN_STATE_WORD] = PREFERENCES_STATE_SAVE;
}

void patch_preferences_ui(u32 *screen)
{
    u32 state = screen[UI_SCREEN_STATE_WORD];
    volatile u32 *cursor = (volatile u32 *)(UI_SCREEN_CONTEXT + UI_SCREEN_PREFERENCES_CURSOR);
    if (state == PREFERENCES_STATE_EDIT_HIGH_CONTRAST) { edit_high_contrast(screen); return; }
    if (state == PREFERENCES_STATE_BROWSE && screen[UI_SCREEN_ENCODER_WORD]) {
        /* The stock browse clamps to row 4; moves into or out of row 5 are
         * handled here and the stock pass sees no encoder steps. */
        s32 target = (s32)*cursor + (s32)screen[UI_SCREEN_ENCODER_WORD];
        if (target < 0) target = 0;
        if (target > (s32)PREFERENCES_ROW_HIGH_CONTRAST) target = PREFERENCES_ROW_HIGH_CONTRAST;
        if (target == (s32)PREFERENCES_ROW_HIGH_CONTRAST || *cursor == PREFERENCES_ROW_HIGH_CONTRAST) {
            *cursor = (u32)target;
            screen[UI_SCREEN_ENCODER_WORD] = 0;
            render_values((u32)target);
        }
    }
    preferences_ui_stock(screen);
    if (state == PREFERENCES_STATE_DRAW) {
        /* The label takes the blank line above the exit prompt. */
        volatile u32 *line = (volatile u32 *)(UI_SCREEN_CONTEXT + UI_SCREEN_TEXT_LINE);
        u32 after_prompt = *line;
        *line = after_prompt - PREFERENCES_LINES_BELOW_HIGH_CONTRAST;
        ui_draw_text_line_at_cursor((u32 *)line, high_contrast_label, OLED_PIXEL_WHITE, 0);
        *line = after_prompt;
        render_high_contrast(*cursor == PREFERENCES_ROW_HIGH_CONTRAST, 0);
    } else if (state == PREFERENCES_STATE_BROWSE &&
               screen[UI_SCREEN_STATE_WORD] == PREFERENCES_STATE_EDIT_HIGH_CONTRAST) {
        render_high_contrast(1, 1);
    }
}

/* prefs.yml: read after the stock loader, appended after the stock writer. */
static int key_matches(const char *line, const char *key)
{
    for (; *key; ++line, ++key) {
        char a = *line, b = *key;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return *line == ' ' || *line == ':' || *line == '\t';
}

void patch_preferences_after_load(void)
{
    static char text[PREFS_FILE_MAX_BYTES + 1];
    FIL file; u32 done = 0;
    u32 enabled = 0;
    if (!fatfs_open(&file, PREFS_PATH, FA_READ)) {
        if (resource_read_bytes(&file, text, PREFS_FILE_MAX_BYTES, &done)) done = 0;
        resource_close(&file);
    }
    text[done] = 0;
    for (const char *line = text; *line;) {
        if (key_matches(line, high_contrast_key)) {
            const char *value = line + sizeof(high_contrast_key) - 1;
            while (*value == ' ' || *value == '\t' || *value == ':') ++value;
            enabled = *value >= '1' && *value <= '9';
        }
        while (*line && *line != '\n') ++line;
        if (*line) ++line;
    }
    apply_high_contrast(enabled);
}

void patch_preferences_after_write(void)
{
    static const char on_line[] = "HighContrast : 1\n", off_line[] = "HighContrast : 0\n";
    const char *line = high_contrast() ? on_line : off_line;
    FIL file; u32 done;
    if (fatfs_open(&file, PREFS_PATH, FA_WRITE)) return;
    if (!resource_seek_absolute(&file, file.words[FIL_FILE_SIZE / 4]))
        resource_write_bytes(&file, line, sizeof(on_line) - 1, &done);
    resource_close(&file);
}
