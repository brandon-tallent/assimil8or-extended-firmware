#include "ext.h"

/* The Filter page, which Envelope steps to from its own page (src/eq_panel.c
 * owns the cursor, the edit state and the screen). It edits the channel's
 * filter, or a zone's own, as the EQ pages do: EDIT chooses which, ENTER
 * opens the stock edit popup, and holding ENTER on EDIT lets a zone follow
 * its channel again. Holding it on Type puts the filter back to its
 * defaults, and again brings the settings back. Each setting is a stock
 * section: its value, then its CV source and amount under it. The model is
 * src/filter.S and src/eq_model.c. */

int sprintf(char *buffer, const char *format, ...);

#define COLOR_GRAY            0x14a5u
#define COLOR_LIGHT_GRAY      0x18c6u
#define COLOR_DARK_GRAY       0x0c63u
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
#define RESONANCE_COARSE      2      /* steps of the resonance per click */
#define EXTRA_COARSE          2
#define OWNER_SLOTS_PER_CHANNEL 9u   /* a channel's eight zones, then the channel's own filter */
#define OWNER_CHANNEL_SLOT    8u

/* The cursor runs through each setting's value, CV source and CV amount,
 * then EDIT. */
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
static u32 setting_of(u32 field) { return field / PARTS; }
static u32 part_of(u32 field) { return field % PARTS; }
/* Extra and its route show only for the types that use Extra; the cursor
 * skips them for the others. */
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
static u32 count(u32 channel)
{
    u32 type = value(channel)->type, n = 0;
    for (u32 f = 0; f < FIELD_COUNT; ++f) n += shown(type, f);
    return n;
}
static u32 field_of(u32 channel, u32 cursor)
{
    u32 type = value(channel)->type;
    for (u32 f = 0; f < FIELD_COUNT; ++f)
        if (shown(type, f) && !cursor--) return f;
    return FIELD_EDIT;
}
static void apply(u32 channel, struct filter_value next)
{
    if (!memcmp(&next, value(channel), sizeof(next))) return;
    if (panel_scope_zone()) { next.override = 1; filter_zones[channel][zone_index()] = next; }
    else filter_channels[channel] = next;
    bump();
}

/* The settings a hold on Type put back to the defaults, and whose they were
 * (0: none), until a hold restores them, the preset is saved or loaded,
 * or another filter is cleared. */
static struct filter_value held;
static u32 held_owner, held_generation;
static u32 generation(void) { return patch_grain_loaded + patch_extras_saved; }
static u32 owner(u32 channel)
{
    return 1 + channel*OWNER_SLOTS_PER_CHANNEL + (panel_scope_zone() ? zone_index() : OWNER_CHANNEL_SLOT);
}
/* The defaults, with the routes kept. */
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
static void hold(u32 channel, u32 cursor)
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
    default: return;
    }
    bump();
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
static void format_frequency(char *out, u32 f)
{
    if (f < 1000) sprintf(out, "%d Hz", f);
    else if (f < 10000) sprintf(out, "%d.%02dk", f/1000, (f%1000+5)/10 > 99 ? 99 : (f%1000+5)/10);
    else sprintf(out, "%d.%dk", f/1000, (f%1000+50)/100 > 9 ? 9 : (f%1000+50)/100);
}
static void format(u32 setting, const struct filter_value *v, char *out)
{
    switch (setting) {
    case SETTING_TYPE: sprintf(out, "%s", type_names[v->type]); break;
    case SETTING_CUTOFF: format_frequency(out, v->cutoff); break;
    case SETTING_RESONANCE: sprintf(out, "%d", v->resonance); break;
    default: sprintf(out, "%d", v->extra); break;
    }
}
static const char *setting_heading(u32 setting, u32 type)
{
    return setting == SETTING_EXTRA ? filter_extra_name(type) : settings[setting].heading;
}
/* The page redraws when the settings it shows change, whoever changed them. */
static u32 own_revision;
static void bump(void) { own_revision++; }
static u32 revision(void)
{
    u32 sum = own_revision + panel_scope_zone(), channel = panel_selected_channel();
    const u8 *bytes = (const u8 *)value(channel);
    for (u32 n = 0; n < sizeof(struct filter_value); ++n) sum = sum*31 + bytes[n];
    return sum + following(channel);
}

static u8 curve[CURVE_POINTS];
static void draw_graph(const struct filter_value *live, u32 gray)
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
    if (live->type == FILTER_OFF) memset(curve, middle, sizeof(curve));
    else filter_curve(live, CURVE_POINTS, curve, top, bottom, GRAPH_DB_TOP, GRAPH_DB_BOTTOM);
    u16 color = live->type != FILTER_OFF && !gray ? COLOR_WHITE : COLOR_GRAY;
    for (u32 n = 0; n < CURVE_POINTS; ++n) {
        s32 a = curve[n ? n-1 : 0], b = curve[n];
        if (a > b) { s32 t = a; a = b; b = t; }
        for (s32 y = a; y <= b; ++y) pixel(GRAPH_LEFT+1+(s32)n, y, color);
    }
}

/* The setting the graph shows; live redraws it when the routes move it. */
static struct filter_value drawn_live;
static u32 live(u32 channel)
{
    const struct filter_value *v = value(channel);
    struct filter_value now;
    if (!filter_modulated(v)) return 0;
    filter_live(v, channel, &now);
    if (!memcmp(&now, &drawn_live, sizeof(now))) return 0;
    drawn_live = now;
    draw_graph(&now, following(channel));
    return 1;
}

static s32 popup_y(s32 y) { return y > POPUP_Y_MAX ? POPUP_Y_MAX : y; }
static void popup(u32 field, const struct filter_value *v)
{
    char line[TEXT_BYTES];
    u32 setting = setting_of(field), part = part_of(field), r = settings[setting].route;
    const char *title = setting == SETTING_EXTRA ? filter_extra_name(v->type) : settings[setting].title;
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
    draw_graph(&drawn_live, gray);
    for (u32 s = 0; s < SETTINGS; ++s) {
        if (!shown(v->type, s*PARTS)) continue;
        s32 x = settings[s].x, y = settings[s].y;
        u32 r = settings[s].route;
        heading(x, y, setting_heading(s, v->type));
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
        next.amount[r] = (signed char)clamp(next.amount[r] + delta, -EQ_CV_AMOUNT_MAX, EQ_CV_AMOUNT_MAX);
    else switch (setting) {
    case SETTING_TYPE: next.type = (u8)clamp((s32)next.type + delta, FILTER_OFF, FILTER_TYPES - 1); break;
    case SETTING_CUTOFF: next.cutoff = eq_frequency_step((u16)next.cutoff, delta); break;
    case SETTING_RESONANCE: next.resonance = (u8)clamp((s32)next.resonance + delta*RESONANCE_COARSE, 0, FILTER_RESONANCE_MAX); break;
    default: next.extra = (u8)clamp((s32)next.extra + delta*EXTRA_COARSE, 0, FILTER_EXTRA_MAX); break;
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
    case SETTING_CUTOFF: next.cutoff = eq_frequency_from_q16(q16); break;
    case SETTING_RESONANCE: next.resonance = (u8)linear(0, FILTER_RESONANCE_MAX, q16); break;
    default: next.extra = (u8)linear(0, FILTER_EXTRA_MAX, q16); break;
    }
    apply(channel, next);
}
static void no_sample(u32 channel) { (void)channel; }

const struct panel_page filter_page = {
    FIELD_COUNT, draw, step, position, revision, no_sample, 0, enter, hold, count, 1, live};

void filter_page_init(void) { patch_envelope_page = &filter_page; }

/* Stock binds a double press of Envelope everywhere: zero-crossing mode for
 * Sample Start/End and Loop boundary edits. A record with a double callback
 * holds every single press until the 600-tick double window closes, and a
 * second press inside it is taken as the double, so Envelope from any other
 * page answered late or not at all. The double press stays bound on the
 * pages whose boundary edits snap to zero crossings and nowhere else. */
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
