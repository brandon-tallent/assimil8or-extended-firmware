#include "ext.h"

/* The Filter page. Pressing Envelope on the Envelope page opens it.
 * src/eq_panel.c owns the cursor, the edit state and the screen.
 *
 * - EDIT selects the channel's filter or the selected zone's filter.
 * - ENTER opens the stock edit popup.
 * - Hold ENTER on EDIT: the zone uses its channel's filter again.
 * - Hold ENTER on Type: reset the filter to defaults. Hold again: restore it.
 * - A hold acts on release. It does nothing if the encoder turned while
 *   ENTER was held (that is a coarse step, see panel_step).
 * - Stereo pair: pressing the Stereo/Right channel's button selects the
 *   right side. Cutoff then shows and sets the right side's cutoff, stored
 *   as an offset from the master's. The graph shows both sides.
 *
 * Each setting shows its value, with its CV source and amount below it.
 * The filter itself is src/filter.S and src/eq_model.c. */

int sprintf(char *buffer, const char *format, ...);

#define COLOR_GRAY            0x14a5u
#define COLOR_LIGHT_GRAY      0x18c6u
#define COLOR_DARK_GRAY       0x0c63u
#define COLOR_BLUE            0x1f02u   /* byte-swapped RGB565 0x021f: the side not being edited */
#define GRAPH_LEFT            12
#define GRAPH_TOP             24
#define GRAPH_WIDTH           116
#define GRAPH_HEIGHT          23
#define CURVE_POINTS          (GRAPH_WIDTH - 2)
#define GRAPH_DB_TOP          18
#define GRAPH_DB_BOTTOM       (-42)
#define EDIT_X                60     /* EDIT's value on the title line */
#define SECTION_1_Y           49     /* Type and Cutoff */
#define SECTION_2_Y           87     /* Resonance and Extra */
#define VALUE_DY              11
#define CV_DY                 22
#define POPUP_DY              6      /* the popup's top below the selected line */
#define POPUP_Y_MAX           104
#define TEXT_BYTES            24u
#define OWNER_SLOTS_PER_CHANNEL 9u   /* a channel's eight zones, then the channel's own filter */
#define OWNER_CHANNEL_SLOT    8u
/* Coarse step sizes: cutoff 1 semitone, Resonance and Extra 10, CV amount 0.10. */
#define SEMITONES_PER_OCTAVE  12
#define CUTOFF_COARSE         (EQ_STEPS_PER_OCTAVE / SEMITONES_PER_OCTAVE)
#define LEVEL_COARSE          10
#define AMOUNT_COARSE         10

enum { SETTING_TYPE, SETTING_CUTOFF, SETTING_RESONANCE, SETTING_EXTRA, SETTINGS };
enum { PART_VALUE, PART_SOURCE, PART_AMOUNT, PARTS };
#define FIELD_EDIT (SETTINGS*PARTS)
#define FIELD_COUNT (FIELD_EDIT + 1)
enum { HOLD_NONE, HOLD_FOLLOW, HOLD_CLEAR, HOLD_RESTORE };

static const char *const type_names[FILTER_TYPES] = {
    "Off", "LP 24", "LP 12", "BP 24", "BP 12", "HP 24", "HP 12", "Notch 24", "Notch 12", "Band Stop", "Dual L/BP"};
static const struct {
    s32 x, y; u8 route; const char *heading, *title, *rate; s32 per_volt;
} settings[SETTINGS] = {
    {LEFT_X, SECTION_1_Y, FILTER_MOD_TYPE, "TYPE", "Filter Type", "types", FILTER_TYPE_STEPS_PER_VOLT},
    {CV_COLUMN_X, SECTION_1_Y, FILTER_MOD_CUTOFF, "CUTOFF", "Cutoff", "oct", FILTER_CUTOFF_OCTAVES_PER_VOLT},
    {LEFT_X, SECTION_2_Y, FILTER_MOD_RESONANCE, "RES", "Resonance", "steps", FILTER_STEPS_PER_VOLT},
    {CV_COLUMN_X, SECTION_2_Y, FILTER_MOD_EXTRA, "EXTRA", "Extra", "steps", FILTER_STEPS_PER_VOLT},
};
/* Cursor order: across each row, then down: the two values, then their CV
 * sources and amounts; EDIT last, so the page opens on Type. */
#define FIELD(setting, part) ((setting)*PARTS + (part))
static const u8 cursor_order[FIELD_COUNT] = {
    FIELD(SETTING_TYPE, PART_VALUE), FIELD(SETTING_CUTOFF, PART_VALUE),
    FIELD(SETTING_TYPE, PART_SOURCE), FIELD(SETTING_TYPE, PART_AMOUNT),
    FIELD(SETTING_CUTOFF, PART_SOURCE), FIELD(SETTING_CUTOFF, PART_AMOUNT),
    FIELD(SETTING_RESONANCE, PART_VALUE), FIELD(SETTING_EXTRA, PART_VALUE),
    FIELD(SETTING_RESONANCE, PART_SOURCE), FIELD(SETTING_RESONANCE, PART_AMOUNT),
    FIELD(SETTING_EXTRA, PART_SOURCE), FIELD(SETTING_EXTRA, PART_AMOUNT),
    FIELD_EDIT,
};
static u32 setting_of(u32 field) { return field / PARTS; }
static u32 part_of(u32 field) { return field % PARTS; }
/* Extra and its CV route are hidden, and skipped by the cursor, for types
 * that do not use Extra. */
static u32 shown(u32 type, u32 field)
{
    return field == FIELD_EDIT || setting_of(field) != SETTING_EXTRA || filter_extra_name(type) != 0;
}

static void bump(void);
static u32 zone_index(void) { return panel_selected_zone(); }
static u32 following(u32 channel) { return panel_scope_zone() && !filter_zones[channel][zone_index()].override; }
static const struct filter_value *value(u32 channel)
{
    u32 z = zone_index();
    return panel_scope_zone() && filter_zones[channel][z].override ? &filter_zones[channel][z] : &filter_channels[channel];
}
/* Nonzero when the page edits a stereo pair's right side. */
static u32 right_side(void) { return panel_stereo_right(); }
/* Nonzero when the next channel is this channel's Stereo/Right partner. */
static u32 paired(u32 channel)
{
    return channel + 1 < PRESET_CHANNELS && channel_record(channel + 1)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT;
}
static u32 count(u32 channel)
{
    u32 type = value(channel)->type, n = 0;
    for (u32 f = 0; f < FIELD_COUNT; ++f) n += shown(type, f);
    return n;
}
static u32 field_of(u32 channel, u32 cursor)
{
    u32 type = value(channel)->type;
    for (u32 n = 0; n < FIELD_COUNT; ++n)
        if (shown(type, cursor_order[n]) && !cursor--) return cursor_order[n];
    return FIELD_EDIT;
}
static void apply(u32 channel, struct filter_value next)
{
    if (!memcmp(&next, value(channel), sizeof(next))) return;
    if (panel_scope_zone()) { next.override = 1; filter_zones[channel][zone_index()] = next; }
    else filter_channels[channel] = next;
    bump();
}

/* The settings saved by the last reset (hold on Type), for the next hold to
 * restore. held_owner says which filter they belong to (0: none). They are
 * dropped when a preset is saved or loaded, or another filter is reset. */
static struct filter_value held;
static u32 held_owner, held_generation;
static u32 generation(void) { return patch_grain_loaded + patch_extras_saved; }
static u32 owner(u32 channel)
{
    return 1 + channel*OWNER_SLOTS_PER_CHANNEL + (panel_scope_zone() ? zone_index() : OWNER_CHANNEL_SLOT);
}
/* Default settings, keeping the current CV routes. */
static void flat(u32 channel, struct filter_value *next)
{
    const struct filter_value *v = value(channel);
    filter_default(next, channel);
    memcpy(next->source, v->source, sizeof(next->source));
    memcpy(next->amount, v->amount, sizeof(next->amount));
    next->override = v->override;
}
static u32 hold_action(u32 channel, u32 field)
{
    if (field == FIELD_EDIT) return panel_scope_zone() && !following(channel) ? HOLD_FOLLOW : HOLD_NONE;
    if (field != SETTING_TYPE*PARTS + PART_VALUE || following(channel)) return HOLD_NONE;
    struct filter_value next;
    flat(channel, &next);
    if (memcmp(&next, value(channel), sizeof(next))) return HOLD_CLEAR;
    return held_owner == owner(channel) && held_generation == generation() ? HOLD_RESTORE : HOLD_NONE;
}
static u32 hold(u32 channel, u32 cursor)
{
    struct filter_value next;
    switch (hold_action(channel, field_of(channel, cursor))) {
    case HOLD_FOLLOW:
        memset(&filter_zones[channel][zone_index()], 0, sizeof(struct filter_value));
        if (held_owner == owner(channel)) held_owner = 0;
        break;
    case HOLD_CLEAR:
        held = *value(channel); held_owner = owner(channel); held_generation = generation();
        flat(channel, &next);
        apply(channel, next);
        status_show("Filter cleared");
        break;
    case HOLD_RESTORE:
        next = held;
        memcpy(next.source, value(channel)->source, sizeof(next.source));
        memcpy(next.amount, value(channel)->amount, sizeof(next.amount));
        held_owner = 0;
        apply(channel, next);
        status_show("Filter restored");
        break;
    default: return 0;
    }
    bump();
    return 1;
}
static u32 enter(u32 channel, u32 cursor)
{
    if (field_of(channel, cursor) != FIELD_EDIT) return 0;
    panel_toggle_scope();
    return 1;
}

/* Drawing. */
static void heading(s32 x, s32 y, const char *text)
{
    ui_draw_proportional_text(UI_HEADING_FONT, x, y, text, COLOR_TEAL, COLOR_BLACK, 1, 0);
}
static void text(s32 x, s32 y, const char *line, u32 selected, u32 gray)
{
    ui_draw_proportional_text(UI_RENDER_DEFAULT_FONT_CONTEXT, x, y, line,
        selected ? COLOR_BLACK : gray ? COLOR_GRAY : COLOR_WHITE, selected ? COLOR_SELECTED : COLOR_BLACK, 1, 0);
}
static void pixel(s32 x, s32 y, u16 color)
{
    ((volatile u16 *)OLED_SHADOW_FRAMEBUFFER_BASE)[y*OLED_SIDE+x] = color;
}
/* Cutoff text, in the EQ's frequency format: 150.4 Hz, 1.000k. */
static void format_cutoff(char *out, u32 step)
{
    char number[TEXT_BYTES];
    sprintf(out, eq_step_text(number, step) ? "%sk" : "%s Hz", number);
}
static void format(u32 setting, const struct filter_value *v, char *out)
{
    switch (setting) {
    case SETTING_TYPE: sprintf(out, "%s", type_names[v->type]); break;
    case SETTING_CUTOFF: format_cutoff(out, right_side() ? filter_right_cutoff(v) : v->cutoff); break;
    case SETTING_RESONANCE: sprintf(out, "%d", v->resonance); break;
    default: sprintf(out, "%d", v->extra); break;
    }
}
/* On a stereo pair, the Cutoff heading names the side: CUTOFF L or CUTOFF R. */
static const char *setting_heading(u32 setting, const struct filter_value *v, u32 channel)
{
    u32 type = v->type;
    if (setting == SETTING_CUTOFF && right_side()) return "CUTOFF R";
    if (setting == SETTING_CUTOFF && paired(channel)) return "CUTOFF L";
    return setting == SETTING_EXTRA ? filter_extra_name(type) : settings[setting].heading;
}
/* Changes whenever the shown settings change, from any source, so the page redraws. */
static u32 own_revision;
static void bump(void) { own_revision++; }
static u32 revision(void)
{
    u32 sum = own_revision + panel_scope_zone() + 2*right_side(), channel = panel_selected_channel();
    const u8 *bytes = (const u8 *)value(channel);
    for (u32 n = 0; n < sizeof(struct filter_value); ++n) sum = sum*31 + bytes[n];
    return sum + following(channel);
}

static u8 curve[CURVE_POINTS];
static void draw_curve(const struct filter_value *live, s32 top, s32 bottom, s32 middle, u16 color)
{
    if (live->type == FILTER_OFF) memset(curve, middle, sizeof(curve));
    else filter_curve(live, CURVE_POINTS, curve, top, bottom, GRAPH_DB_TOP, GRAPH_DB_BOTTOM);
    for (u32 n = 0; n < CURVE_POINTS; ++n) {
        s32 a = curve[n ? n-1 : 0], b = curve[n];
        if (a > b) { s32 t = a; a = b; b = t; }
        for (s32 y = a; y <= b; ++y) pixel(GRAPH_LEFT+1+(s32)n, y, color);
    }
}
/* With a right-side offset, both curves are drawn: the side being edited in
 * white, the other side in blue underneath. */
static void draw_graph(const struct filter_value *live, u32 gray, u32 pair)
{
    s32 top = GRAPH_TOP+1, bottom = GRAPH_TOP+GRAPH_HEIGHT-2;
    s32 middle = top + (bottom-top)*GRAPH_DB_TOP/(GRAPH_DB_TOP-GRAPH_DB_BOTTOM);
    oled_shadow_fill_rect(GRAPH_LEFT, GRAPH_TOP, GRAPH_WIDTH, GRAPH_HEIGHT, COLOR_LIGHT_GRAY);
    oled_shadow_fill_rect(GRAPH_LEFT+1, top, GRAPH_WIDTH-2, GRAPH_HEIGHT-2, COLOR_BLACK);
    for (s32 x = GRAPH_LEFT+1; x < GRAPH_LEFT+GRAPH_WIDTH-1; x += 2) pixel(x, middle, COLOR_DARK_GRAY);
    /* 100 Hz, 1 kHz and 10 kHz on the 20 Hz..20 kHz log scale. */
    static const u8 decades[] = {26, 64, 102};
    for (u32 d = 0; d < 3; ++d)
        for (s32 y = top; y <= bottom; y += 2) pixel(GRAPH_LEFT+1+decades[d], y, COLOR_DARK_GRAY);
    u16 color = live->type != FILTER_OFF && !gray ? COLOR_WHITE : COLOR_GRAY;
    struct filter_value right = *live;
    right.cutoff = (u16)filter_right_cutoff(live);
    if (!pair || !live->right) { draw_curve(right_side() ? &right : live, top, bottom, middle, color); return; }
    u32 edited = right_side();
    draw_curve(edited ? live : &right, top, bottom, middle, gray ? COLOR_DARK_GRAY : COLOR_BLUE);
    draw_curve(edited ? &right : live, top, bottom, middle, color);
}

/* The settings the graph shows. live() redraws the graph when CV changes them. */
static struct filter_value drawn_live;
static u32 live(u32 channel)
{
    const struct filter_value *v = value(channel);
    struct filter_value now;
    if (!filter_modulated(v)) return 0;
    filter_live(v, channel, &now);
    if (!memcmp(&now, &drawn_live, sizeof(now))) return 0;
    drawn_live = now;
    draw_graph(&now, following(channel), paired(channel));
    return 1;
}

static s32 popup_y(s32 y) { return y > POPUP_Y_MAX ? POPUP_Y_MAX : y; }
static void popup(u32 field, const struct filter_value *v)
{
    char line[TEXT_BYTES];
    u32 setting = setting_of(field), part = part_of(field), r = settings[setting].route;
    const char *title = setting == SETTING_EXTRA ? filter_extra_name(v->type) :
                        setting == SETTING_CUTOFF && right_side() ? "Right Cutoff" : settings[setting].title;
    if (part == PART_VALUE) {
        format(setting, v, line);
        ui_render_parameter_row(UI_PARAMETER_ROW_STYLE, 0, popup_y(settings[setting].y + VALUE_DY + POPUP_DY), line,
                                title, "", 0, POPUP_BACKGROUND_FROM_STYLE, 0);
        return;
    }
    /* An amount's popup gives what it moves per volt, in hundredths of the
     * route's unit. */
    char rate[TEXT_BYTES], per_volt[TEXT_BYTES];
    s32 per = v->amount[r]*settings[setting].per_volt, magnitude = per < 0 ? -per : per;
    s32 whole = magnitude/EQ_CV_AMOUNT_MAX, hundredths = magnitude%EQ_CV_AMOUNT_MAX;
    if (hundredths) sprintf(rate, "%c%d.%02d", per < 0 ? '-' : '+', whole, hundredths);
    else sprintf(rate, "%c%d", per < 0 ? '-' : '+', whole);
    sprintf(per_volt, "   (%s %s/V)", rate, settings[setting].rate);
    panel_cv_popup_at(popup_y(settings[setting].y + CV_DY + POPUP_DY), part == PART_AMOUNT,
                      setting == SETTING_EXTRA ? settings[setting].title : title, v->source[r], v->amount[r], per_volt);
}

static void draw(u32 channel, u32 cursor, u32 editing)
{
    char line[TEXT_BYTES];
    const struct filter_value *v = value(channel);
    u32 gray = following(channel), selected = field_of(channel, cursor);
    heading(LEFT_X, TITLE_Y, "FILTER");
    text(EDIT_X, TITLE_Y, panel_scope_zone() ? "Zone" : "Chan", selected == FIELD_EDIT, 0);
    ui_draw_zone_label_blinking_when_synced(zone_index(), ZONE_LABEL_X, TITLE_Y);
    /* The graph shows the filter as its routes move it now. */
    filter_live(v, channel, &drawn_live);
    draw_graph(&drawn_live, gray, paired(channel));
    for (u32 s = 0; s < SETTINGS; ++s) {
        if (!shown(v->type, s*PARTS)) continue;
        s32 x = settings[s].x, y = settings[s].y;
        u32 r = settings[s].route;
        heading(x, y, setting_heading(s, v, channel));
        format(s, v, line);
        text(x, y + VALUE_DY, line, selected == s*PARTS + PART_VALUE, gray);
        u32 at = selected / PARTS == s && selected != FIELD_EDIT ? part_of(selected) : PART_VALUE;
        panel_cv_pair(x, y + CV_DY, v->source[r], v->amount[r], at, gray);
    }
    if (editing && selected != FIELD_EDIT) popup(selected, v);
}

static s32 clamp(s32 x, s32 low, s32 high) { return x < low ? low : x > high ? high : x; }
static s32 linear(s32 low, s32 high, u32 q16) { return low + (s32)((q16*(u32)(high - low) + 0x8000u) >> 16); }

static void step(u32 channel, u32 cursor, s32 delta)
{
    struct filter_value next = *value(channel);
    u32 field = field_of(channel, cursor);
    if (field == FIELD_EDIT) return;
    u32 setting = setting_of(field), r = settings[setting].route;
    if (part_of(field) == PART_SOURCE)
        next.source[r] = (signed char)clamp(next.source[r] + delta, EQ_CV_SOURCE_OFF, EQ_CV_SOURCE_MAX);
    else if (part_of(field) == PART_AMOUNT)
        next.amount[r] = (signed char)clamp(panel_step(next.amount[r], delta, AMOUNT_COARSE, 0), -EQ_CV_AMOUNT_MAX, EQ_CV_AMOUNT_MAX);
    else switch (setting) {
    case SETTING_TYPE: next.type = (u8)clamp((s32)next.type + delta, FILTER_OFF, FILTER_TYPES - 1); break;
    case SETTING_CUTOFF:
        if (right_side()) next.right = (s16)(clamp(panel_step((s32)filter_right_cutoff(&next), delta, CUTOFF_COARSE, EQ_STEP_1K), 0, EQ_STEP_MAX) - next.cutoff);
        else next.cutoff = (u16)clamp(panel_step(next.cutoff, delta, CUTOFF_COARSE, EQ_STEP_1K), 0, EQ_STEP_MAX);
        break;
    case SETTING_RESONANCE: next.resonance = (u8)clamp(panel_step(next.resonance, delta, LEVEL_COARSE, 0), 0, FILTER_RESONANCE_MAX); break;
    default: next.extra = (u8)clamp(panel_step(next.extra, delta, LEVEL_COARSE, 0), 0, FILTER_EXTRA_MAX); break;
    }
    apply(channel, next);
}
static void position(u32 channel, u32 cursor, u32 q16)
{
    struct filter_value next = *value(channel);
    u32 field = field_of(channel, cursor);
    if (field == FIELD_EDIT) {
        if ((q16 >= 0x8000u) != panel_scope_zone()) panel_toggle_scope();
        return;
    }
    u32 setting = setting_of(field), r = settings[setting].route;
    if (part_of(field) == PART_SOURCE) next.source[r] = (signed char)linear(EQ_CV_SOURCE_OFF, EQ_CV_SOURCE_MAX, q16);
    else if (part_of(field) == PART_AMOUNT) next.amount[r] = (signed char)linear(-EQ_CV_AMOUNT_MAX, EQ_CV_AMOUNT_MAX, q16);
    else switch (setting) {
    case SETTING_TYPE: next.type = (u8)clamp((s32)((q16*FILTER_TYPES) >> 16), FILTER_OFF, FILTER_TYPES - 1); break;
    case SETTING_CUTOFF:
        if (right_side()) next.right = (s16)(linear(0, EQ_STEP_MAX, q16) - next.cutoff);
        else next.cutoff = (u16)linear(0, EQ_STEP_MAX, q16);
        break;
    case SETTING_RESONANCE: next.resonance = (u8)linear(0, FILTER_RESONANCE_MAX, q16); break;
    default: next.extra = (u8)linear(0, FILTER_EXTRA_MAX, q16); break;
    }
    apply(channel, next);
}
static void no_sample(u32 channel) { (void)channel; }

const struct panel_page filter_page = {
    FIELD_COUNT, draw, step, position, revision, no_sample, 0, enter, hold, count, 1, live};

void filter_page_init(void) { patch_envelope_page = &filter_page; }

/* Stock binds a double press of Envelope on every page. It toggles
 * zero-crossing mode for Sample Start/End and Loop edits. While a double
 * press is bound, every single press waits for the 600-tick double-press
 * window, and a second press in that window counts as the double. That made
 * Envelope slow or unresponsive on other pages. The double press is now
 * bound only on the pages that snap to zero crossings. */
void zone_editor_zero_crossing_status_toggle(u32 *context, u32 argument);
#define PANEL_ENVELOPE                 25u   /* FRONT_PANEL_ENVELOPE_RECORD_INDEX */
#define PANEL_DOUBLE                   0x20u /* FRONT_PANEL_RECORD_DOUBLE_CALLBACK */
#define EDITOR_PAGE_STATE              0x58u /* EDITOR_SELECTOR_PAGE_STATE */
#define SAMPLE_CONTROL_PAGE_STATE      0x10u /* ZONE_EDITOR_SAMPLE_CONTROL_PRIMARY_PAGE_STATE */
#define LOOP_CONTROL_PAGE_STATE        0x11u
void filter_page_foreground(void)
{
    u32 slot = FRONT_PANEL_STATE + PANEL_ENVELOPE*FRONT_PANEL_RECORD_SIZE + PANEL_DOUBLE;
    u32 toggle = (u32)zone_editor_zero_crossing_status_toggle, bound = REG32(slot);
    u32 state = REG32(EQ_EDITOR + EDITOR_PAGE_STATE);
    u32 boundary = state == SAMPLE_CONTROL_PAGE_STATE || state == LOOP_CONTROL_PAGE_STATE;
    /* Only the stock toggle or nothing: another binding is a page's own. */
    if (boundary && !bound) REG32(slot) = toggle;
    else if (!boundary && bound == toggle) REG32(slot) = 0;
}
