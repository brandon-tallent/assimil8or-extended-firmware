#include "ext.h"
#include "mix_fx.h"

/* The mix effect pages, after the stock Pan/Mix page on the Pan/Mix
 * button: Comp and Sends for the selected channel, then Reverb, Delay and
 * Mix Comp for the mix. A setting with a CV route shows its source and
 * amount under its value; the cursor steps value, source, amount. */

int sprintf(char *buffer, const char *format, ...);
char *ui_format_cv_input_selector(char *dest, const signed short *selector, s32 channel);

#define COLOR_GRAY      0x14a5u
#define FIRST_ROW_Y     35      /* below the gain reduction meter */
#define ROW_DY          23      /* a row of settings without CV */
#define METER_Y         25
#define METER_HEIGHT    5
#define METER_SEGMENTS  12      /* 2 dB each */
#define METER_DB_PER_SEGMENT 2u
#define METER_SEGMENT_DX 6
#define METER_TEXT_X    90
#define METER_TEXT_Y    23
#define COLOR_YELLOW    0xc0ffu
#define COLOR_DARK_GRAY 0x0c63u
#define CV_FIRST_ROW_Y  24
#define CV_ROW_DY       31      /* a row of settings with CV */
#define VALUE_DY        11
#define CV_DY           21
#define AMOUNT_DX       28
#define POPUP_DY        6
#define POPUP_Y_MAX     104
#define TEXT_BYTES      24u
#define PERCENT_COARSE  10
#define BPM_COARSE      10
#define DECAY_COARSE    10
#define AMOUNT_COARSE   10
#define AMOUNT_MAX      100
#define HZ_PER_KHZ      1000u
#define MS_PER_S        1000u
#define NO_ROUTE        (-1)

enum kind { PERCENT, FEEDBACK, THRESHOLD, RATIO, ATTACK, RELEASE, KEY, GAIN, TAP, DECAY, DIVISION, LOW_CUT, HIGH_CUT, BPM, ALGO };
enum { PART_VALUE, PART_SOURCE, PART_AMOUNT };
struct field { const char *title, *name; u8 kind, per_channel; u16 offset, stride; u8 low, high; signed char route; };

#define COMP(f) __builtin_offsetof(struct mix_settings, comp[0].f), sizeof(struct mix_comp_set)
#define MASTER(f) __builtin_offsetof(struct mix_settings, master.f), 0
#define BUS(f) __builtin_offsetof(struct mix_settings, f), 0
#define SEND(f) __builtin_offsetof(struct mix_settings, f[0]), 1

static const struct field comp_fields[] = {
    {"RATIO", "Ratio", RATIO, 1, COMP(ratio), 0, MIX_RATIOS - 1, NO_ROUTE},
    {"THRESH", "Threshold", THRESHOLD, 1, COMP(threshold), 0, MIX_THRESHOLD_MAX, NO_ROUTE},
    {"ATTACK", "Attack", ATTACK, 1, COMP(attack), 0, MIX_ATTACKS - 1, NO_ROUTE},
    {"RELEASE", "Release", RELEASE, 1, COMP(release), 0, MIX_RELEASES - 1, NO_ROUTE},
    {"DRIVE", "Drive", PERCENT, 1, COMP(drive), 0, MIX_PERCENT_MAX, NO_ROUTE},
    {"GAIN", "Gain", GAIN, 1, COMP(gain), 0, MIX_GAIN_MAX, NO_ROUTE},
    {"MIX", "Mix", PERCENT, 1, COMP(mix), 0, MIX_PERCENT_MAX, NO_ROUTE},
    {"SIDECHAIN", "Sidechain", KEY, 1, COMP(key), 0, MIX_KEY_INPUT, NO_ROUTE},
};
static const struct field send_fields[] = {
    {"REVERB", "Reverb", PERCENT, 1, SEND(reverb_send), 0, MIX_PERCENT_MAX, MIX_ROUTE_REVERB_SEND},
    {"DELAY", "Delay", PERCENT, 1, SEND(delay_send), 0, MIX_PERCENT_MAX, MIX_ROUTE_DELAY_SEND},
    {"PRE/POST", "Pre/Post", TAP, 1, SEND(send_pre), 0, 1, NO_ROUTE},
};
static const struct field reverb_fields[] = {
    {"ALGO", "Algorithm", ALGO, 0, BUS(reverb_algo), 0, MIX_ALGOS - 1, NO_ROUTE},
    {"DECAY", "Decay", DECAY, 0, BUS(reverb_decay), MIX_DECAY_MIN, MIX_DECAY_MAX, MIX_ROUTE_REVERB_DECAY},
    {"DAMP", "Damp", PERCENT, 0, BUS(reverb_damp), 0, MIX_PERCENT_MAX, MIX_ROUTE_REVERB_DAMP},
    {"LEVEL", "Level", PERCENT, 0, BUS(reverb_level), 0, MIX_PERCENT_MAX, MIX_ROUTE_REVERB_LEVEL},
    {"RATE", "Chorus Rate", PERCENT, 0, BUS(chorus_rate), 0, MIX_PERCENT_MAX, MIX_ROUTE_CHORUS_RATE},
    {"DEPTH", "Chorus Depth", PERCENT, 0, BUS(chorus_depth), 0, MIX_PERCENT_MAX, MIX_ROUTE_CHORUS_DEPTH},
};
static const struct field delay_fields[] = {
    {"TIME", "Time", DIVISION, 0, BUS(delay_division), 0, MIX_DIVISIONS - 1, MIX_ROUTE_DELAY_TIME},
    {"FEEDBACK", "Feedback", FEEDBACK, 0, BUS(delay_feedback), 0, MIX_FEEDBACK_MAX, MIX_ROUTE_DELAY_FEEDBACK},
    {"LOW CUT", "Low Cut", LOW_CUT, 0, BUS(delay_low), 0, MIX_FILTER_STEPS - 1, MIX_ROUTE_DELAY_LOW},
    {"HIGH CUT", "High Cut", HIGH_CUT, 0, BUS(delay_high), 0, MIX_FILTER_STEPS - 1, MIX_ROUTE_DELAY_HIGH},
    {"LEVEL", "Level", PERCENT, 0, BUS(delay_level), 0, MIX_PERCENT_MAX, MIX_ROUTE_DELAY_LEVEL},
    {"TEMPO", "Tempo", BPM, 0, BUS(delay_tempo), 0, MIX_TEMPO_MAX, NO_ROUTE},
};
static const struct field master_fields[] = {
    {"RATIO", "Ratio", RATIO, 0, MASTER(ratio), 0, MIX_RATIOS - 1, NO_ROUTE},
    {"THRESH", "Threshold", THRESHOLD, 0, MASTER(threshold), 0, MIX_THRESHOLD_MAX, NO_ROUTE},
    {"ATTACK", "Attack", ATTACK, 0, MASTER(attack), 0, MIX_ATTACKS - 1, NO_ROUTE},
    {"RELEASE", "Release", RELEASE, 0, MASTER(release), 0, MIX_RELEASES - 1, NO_ROUTE},
    {"DRIVE", "Drive", PERCENT, 0, MASTER(drive), 0, MIX_PERCENT_MAX, NO_ROUTE},
    {"GAIN", "Gain", GAIN, 0, MASTER(gain), 0, MIX_GAIN_MAX, NO_ROUTE},
    {"MIX", "Mix", PERCENT, 0, MASTER(mix), 0, MIX_PERCENT_MAX, NO_ROUTE},
    {"SIDECHAIN", "Sidechain", KEY, 0, MASTER(key), 0, MIX_KEY_INPUT, NO_ROUTE},
};

struct page_def { const char *title; const struct field *fields; u32 count, cv; };
#define PAGE(t, f, cv) {t, f, sizeof(f) / sizeof(f[0]), cv}
enum { PAGE_COMP, PAGE_SENDS, PAGE_REVERB, PAGE_DELAY, PAGE_MASTER, PAGES };
static const struct page_def pages[PAGES] = {
    PAGE("COMP", comp_fields, 0), PAGE("SENDS", send_fields, 1), PAGE("REVERB", reverb_fields, 1),
    PAGE("DELAY", delay_fields, 1), PAGE("MIX COMP", master_fields, 0),
};
static const char *const key_names[MIX_KEY_INPUT + 1] = {"Self", "Ch 1", "Ch 2", "Ch 3", "Ch 4", "Ch 5", "Ch 6", "Ch 7", "Ch 8", "Input"};

static u32 own_revision;
static u8 *value_at(const struct field *f, u32 channel)
{
    return (u8 *)mix_now() + f->offset + (f->per_channel ? channel * f->stride : 0);
}
static s32 clamp(s32 x, s32 low, s32 high) { return x < low ? low : x > high ? high : x; }

static void format(char *out, const struct field *f, u32 v)
{
    switch (f->kind) {
    case PERCENT: case FEEDBACK: sprintf(out, "%d%%", v); break;
    case THRESHOLD: sprintf(out, v ? "-%d dB" : "0 dB", v); break;
    case RATIO: sprintf(out, "%s", mix_ratio_names[v]); break;
    case ATTACK: {
        u32 t = mix_attack_hundredths_ms[v];
        if (t < 10) sprintf(out, "0.0%d ms", t);
        else if (t < 100) sprintf(out, "0.%d ms", t / 10);
        else sprintf(out, "%d ms", t / 100);
        break;
    }
    case RELEASE: {
        u32 ms = mix_release_ms[v];
        if (ms < MS_PER_S) sprintf(out, "%d ms", ms); else sprintf(out, "%d.%d s", ms / MS_PER_S, ms % MS_PER_S / 100);
        break;
    }
    case KEY: sprintf(out, "%s", key_names[v]); break;
    case GAIN: sprintf(out, "+%d dB", v); break;
    case TAP: sprintf(out, v ? "Pre" : "Post"); break;
    case DECAY:
        if (mix_now()->reverb_algo >= MIX_ALGO_GATED) sprintf(out, "%d ms", v * 5);
        else sprintf(out, "%d.%d s", v / 10, v % 10);
        break;
    case ALGO: sprintf(out, "%s", mix_algo_names[v]); break;
    case DIVISION: sprintf(out, "%s", mix_division_names[v]); break;
    case LOW_CUT: if (mix_low_cut_hz[v]) sprintf(out, "%d Hz", mix_low_cut_hz[v]); else sprintf(out, "Off"); break;
    case HIGH_CUT:
        if (mix_high_cut_hz[v]) sprintf(out, "%d kHz", mix_high_cut_hz[v] / HZ_PER_KHZ); else sprintf(out, "Off");
        break;
    default: {
        /* A followed tempo shows what it plays at now. */
        u32 bpm10 = mix_fx_bpm10();
        if (v >= MIX_TEMPO_BPM) sprintf(out, "%d BPM", MIX_BPM_MIN + v - MIX_TEMPO_BPM);
        else if (!bpm10) sprintf(out, v == MIX_TEMPO_MIDI ? "MIDI --" : "Ch %d --", v);
        else if (v == MIX_TEMPO_MIDI) sprintf(out, "MIDI %d.%d", bpm10 / 10, bpm10 % 10);
        else sprintf(out, "Ch%d %d.%d", v, bpm10 / 10, bpm10 % 10);
        break;
    }
    }
}

/* ---- the cursor ---- */

static struct mix_route *route_of(const struct field *f, u32 channel)
{
    if (f->route == NO_ROUTE) return 0;
    struct mix_settings *m = (struct mix_settings *)mix_now();
    return f->per_channel ? &m->send_route[channel][f->route] : &m->bus_route[f->route];
}
static u32 parts_of(const struct field *f) { return f->route == NO_ROUTE ? 1 : 3; }
static u32 cursor_count(const struct page_def *p)
{
    u32 n = 0;
    for (u32 i = 0; i < p->count; ++i) n += parts_of(&p->fields[i]);
    return n;
}
/* The field and part at a cursor position; the field is p->count past the end. */
static u32 field_at(const struct page_def *p, u32 cursor, u32 *part)
{
    for (u32 i = 0; i < p->count; ++i) {
        u32 n = parts_of(&p->fields[i]);
        if (cursor < n) { *part = cursor; return i; }
        cursor -= n;
    }
    *part = PART_VALUE;
    return p->count;
}

/* ---- drawing ---- */

static void heading(s32 x, s32 y, const char *line, u32 gray)
{
    ui_draw_proportional_text(UI_HEADING_FONT, x, y, line, gray ? COLOR_GRAY : COLOR_TEAL, COLOR_BLACK, 1, 0);
}
static void text(s32 x, s32 y, const char *line, u32 selected, u32 gray)
{
    ui_draw_proportional_text(UI_RENDER_DEFAULT_FONT_CONTEXT, x, y, line,
        selected ? COLOR_BLACK : gray ? COLOR_GRAY : COLOR_WHITE, selected ? COLOR_SELECTED : COLOR_BLACK, 1, 0);
}
static void format_source(char *out, s32 source)
{
    signed short selector = (signed short)source;
    ui_format_cv_input_selector(out, &selector, 0);
}
static void format_amount(char *out, s32 amount)
{
    s32 magnitude = amount < 0 ? -amount : amount;
    sprintf(out, "%c%d.%02d", amount < 0 ? '-' : '+', magnitude / 100, magnitude % 100);
}
static s32 field_x(u32 i) { return i & 1u ? CV_COLUMN_X : LEFT_X; }
static s32 field_y(const struct page_def *p, u32 i)
{
    return p->cv ? CV_FIRST_ROW_Y + (s32)(i >> 1) * CV_ROW_DY : FIRST_ROW_Y + (s32)(i >> 1) * ROW_DY;
}
/* Gray: a compressor's own fields while its Ratio is Off; the chorus's
 * Rate and Depth when the algorithm has no chorus; Decay and Damp on the
 * chorus alone. */
static u32 field_gray(const struct page_def *p, u32 i, u32 channel)
{
    const struct field *f = &p->fields[i];
    u32 algo = mix_now()->reverb_algo;
    if (f->offset == __builtin_offsetof(struct mix_settings, chorus_rate) ||
        f->offset == __builtin_offsetof(struct mix_settings, chorus_depth)) return !mix_algo_chorus(algo);
    if (f->offset == __builtin_offsetof(struct mix_settings, reverb_decay) ||
        f->offset == __builtin_offsetof(struct mix_settings, reverb_damp)) return algo == 0;
    /* Drive and Mix act with the compressor off. */
    if (p->fields[0].kind != RATIO || i == 0 || f->kind == PERCENT) return 0;
    return *value_at(&p->fields[0], channel) == 0;
}
/* Decay is the gate's length on the nonlinear algorithms. */
static const char *title_of(const struct field *f)
{
    return f->kind == DECAY && mix_now()->reverb_algo >= MIX_ALGO_GATED ? "GATE" : f->title;
}
/* A compressor's gain reduction: a segment for each 2 dB and the dB. */
static void draw_meter(const struct page_def *p, u32 channel)
{
    char line[TEXT_BYTES];
    u32 on = *value_at(&p->fields[0], channel) != 0;
    u32 db = on ? mix_fx_reduction_db(p->fields[0].per_channel ? channel : MIX_CHANNELS) : 0;
    oled_shadow_fill_rect(LEFT_X, METER_TEXT_Y, OLED_SIDE - LEFT_X, VALUE_DY, COLOR_BLACK);
    for (u32 i = 0; i < METER_SEGMENTS; ++i)
        oled_shadow_fill_rect(LEFT_X + i * METER_SEGMENT_DX, METER_Y, METER_SEGMENT_DX - 1, METER_HEIGHT,
                              on && db >= (i + 1) * METER_DB_PER_SEGMENT ? COLOR_YELLOW : COLOR_DARK_GRAY);
    if (!on) return;
    if (db) sprintf(line, "-%d dB", db); else sprintf(line, "0 dB");
    text(METER_TEXT_X, METER_TEXT_Y, line, 0, 0);
}

static void draw_page(const struct page_def *p, u32 channel, u32 cursor, u32 editing)
{
    char line[TEXT_BYTES];
    u32 part, selected = field_at(p, cursor, &part);
    /* The channel pages name the channel they edit, both of a stereo pair. */
    u32 pair = channel + 1 < MIX_CHANNELS && channel_record(channel + 1)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT;
    if (!p->fields[0].per_channel) heading(LEFT_X, TITLE_Y, p->title, 0);
    else if (pair) { sprintf(line, "%s CH %d+%d", p->title, channel + 1, channel + 2); heading(LEFT_X, TITLE_Y, line, 0); }
    else { sprintf(line, "%s CH %d", p->title, channel + 1); heading(LEFT_X, TITLE_Y, line, 0); }
    if (p->fields[0].kind == RATIO) draw_meter(p, channel);
    for (u32 i = 0; i < p->count; ++i) {
        const struct field *f = &p->fields[i];
        u32 gray = field_gray(p, i, channel), here = i == selected;
        s32 x = field_x(i), y = field_y(p, i);
        heading(x, y, title_of(f), gray);
        format(line, f, *value_at(f, channel));
        text(x, y + VALUE_DY, line, here && part == PART_VALUE, gray);
        const struct mix_route *r = route_of(f, channel);
        if (!r) continue;
        format_source(line, r->source);
        text(x, y + CV_DY, line, here && part == PART_SOURCE, gray);
        format_amount(line, r->amount);
        text(x + AMOUNT_DX, y + CV_DY, line, here && part == PART_AMOUNT, gray);
    }
    if (!editing || selected >= p->count) return;
    const struct field *f = &p->fields[selected];
    const struct mix_route *r = route_of(f, channel);
    s32 y = field_y(p, selected) + (part == PART_VALUE ? VALUE_DY : CV_DY) + POPUP_DY;
    if (y > POPUP_Y_MAX) y = POPUP_Y_MAX;
    if (part == PART_VALUE) {
        format(line, f, *value_at(f, channel));
        ui_render_parameter_row(UI_PARAMETER_ROW_STYLE, 0, y, line, title_of(f), "", 0, POPUP_BACKGROUND_FROM_STYLE, 0);
    } else panel_cv_popup_at(y, part == PART_AMOUNT, f->name, r->source, r->amount, "");
}

/* ---- editing ---- */

static s32 coarse_of(const struct field *f)
{
    return f->kind == PERCENT || f->kind == FEEDBACK ? PERCENT_COARSE : f->kind == BPM ? BPM_COARSE :
           f->kind == DECAY ? DECAY_COARSE : 1;
}
static void changed(void) { mix_fx_update(); ++own_revision; }
static void set(const struct field *f, u32 channel, s32 v)
{
    mix_edit();
    *value_at(f, channel) = (u8)clamp(v, f->low, f->high);
    changed();
}
/* A mix setting's route has no channel of its own: its sources skip the
 * channel-relative CV A to CV C. */
static s32 source_step(const struct field *f, s32 from, s32 delta)
{
    s32 to = clamp(from + delta, EQ_CV_SOURCE_OFF, EQ_CV_SOURCE_MAX);
    if (!f->per_channel && to > EQ_CV_SOURCE_OFF && to < 0) to = delta > 0 ? 0 : EQ_CV_SOURCE_OFF;
    return to;
}
static void step_page(const struct page_def *p, u32 channel, u32 cursor, s32 delta)
{
    u32 part, i = field_at(p, cursor, &part);
    if (i >= p->count || !delta) return;
    const struct field *f = &p->fields[i];
    struct mix_route *r = route_of(f, channel);
    if (part != PART_VALUE) { mix_edit(); r = route_of(f, channel); }
    if (part == PART_SOURCE) { r->source = (signed char)source_step(f, r->source, delta); changed(); }
    else if (part == PART_AMOUNT) { r->amount = (signed char)clamp(panel_step(r->amount, delta, AMOUNT_COARSE, 0), -AMOUNT_MAX, AMOUNT_MAX); changed(); }
    else set(f, channel, panel_step(*value_at(f, channel), delta, coarse_of(f), 0));
}
static s32 linear(s32 low, s32 high, u32 q16) { return low + (s32)((q16 * (u32)(high - low) + 0x8000u) >> 16); }
static void position_page(const struct page_def *p, u32 channel, u32 cursor, u32 q16)
{
    u32 part, i = field_at(p, cursor, &part);
    if (i >= p->count) return;
    const struct field *f = &p->fields[i];
    struct mix_route *r = route_of(f, channel);
    if (part != PART_VALUE) { mix_edit(); r = route_of(f, channel); }
    if (part == PART_SOURCE) { r->source = (signed char)source_step(f, linear(EQ_CV_SOURCE_OFF, EQ_CV_SOURCE_MAX, q16), 0); changed(); }
    else if (part == PART_AMOUNT) { r->amount = (signed char)linear(-AMOUNT_MAX, AMOUNT_MAX, q16); changed(); }
    else set(f, channel, linear(f->low, f->high, q16));
}

static u32 revision(void)
{
    return (own_revision * 31 + panel_selected_channel()) * 31 + mix_fx_bpm10();
}
static void no_sample(u32 channel) { (void)channel; }

/* The meter follows the compressor at the graph's live rate. */
static u32 meter_live(const struct page_def *p, u32 channel) { draw_meter(p, channel); return 1; }
static u32 comp_live(u32 c) { return meter_live(&pages[PAGE_COMP], c); }
static u32 master_live(u32 c) { return meter_live(&pages[PAGE_MASTER], c); }

#define PAGE_CALLBACKS(name, index) \
    static void name##_draw(u32 c, u32 f, u32 e) { draw_page(&pages[index], c, f, e); } \
    static void name##_step(u32 c, u32 f, s32 d) { step_page(&pages[index], c, f, d); } \
    static void name##_position(u32 c, u32 f, u32 q) { position_page(&pages[index], c, f, q); } \
    static u32 name##_count(u32 c) { (void)c; return cursor_count(&pages[index]); }
PAGE_CALLBACKS(comp, PAGE_COMP)
PAGE_CALLBACKS(send, PAGE_SENDS)
PAGE_CALLBACKS(reverb, PAGE_REVERB)
PAGE_CALLBACKS(delay, PAGE_DELAY)
PAGE_CALLBACKS(master, PAGE_MASTER)

/* `fields` is a bound; count gives the cursor positions. */
#define PANEL_PAGE(name, next, live) \
    {3u * sizeof(name##_fields) / sizeof(name##_fields[0]), name##_draw, name##_step, name##_position, revision, \
     no_sample, next, 0, 0, name##_count, 0, live}
static const struct panel_page master_page = PANEL_PAGE(master, 0, master_live);
static const struct panel_page delay_page = PANEL_PAGE(delay, &master_page, 0);
static const struct panel_page reverb_page = PANEL_PAGE(reverb, &delay_page, 0);
static const struct panel_page sends_page = PANEL_PAGE(send, &reverb_page, 0);
static const struct panel_page comp_page = PANEL_PAGE(comp, &sends_page, comp_live);

void mix_page_init(void) { patch_mix_page = &comp_page; }
