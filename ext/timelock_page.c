#include "ext.h"

/* The Time Lock page, after Pitch on the Pitch page. */

int sprintf(char *buffer, const char *format, ...);
char *ui_format_cv_input_selector(char *dest, const signed short *selector, s32 channel);

#define COLOR_GRAY          0x14a5u
#define ROW_DY              11
#define LOCK_Y              13
#define TEMPO_Y             39
#define BEATS_Y             76
#define POPUP_DY            6
#define POPUP_Y_MAX         104
#define TEXT_BYTES          24u
#define SPEED_COARSE        10     /* a push-turn moves Speed 1% */
#define PERCENT_TENTHS      10
#define BPM_TENTHS          10

enum { FIELD_LOCK, FIELD_TEMPO, FIELD_SPEED, FIELD_INPUT, FIELD_PPQN, FIELD_BEATS, FIELDS };
static const char *const tempo_names[TIME_LOCK_TEMPOS] = {"Ch 1", "Own", "Speed", "MIDI clock", "CV clock"};
/* A channel's own Tempo; channel 1 has no Ch 1 and shows it as Own. */
static u32 tempo_at(u32 c) { return !c && time_lock_tempo[c] == TIME_LOCK_TEMPO_CHANNEL_1 ? TIME_LOCK_TEMPO_OWN : time_lock_tempo[c]; }
static u32 tempo_first(u32 c) { return c ? TIME_LOCK_TEMPO_CHANNEL_1 : TIME_LOCK_TEMPO_OWN; }
static const u8 ppqn_steps[] = {1, 2, 4, 8, 12, 16, 24, 48, 96};
#define PPQN_STEPS (sizeof(ppqn_steps) / sizeof(ppqn_steps[0]))

static u32 zone_of(void) { return panel_selected_zone(); }
static s32 clamp(s32 x, s32 low, s32 high) { return x < low ? low : x > high ? high : x; }
static u32 ppqn_of(u32 c) { return time_lock_ppqn[c] ? time_lock_ppqn[c] : TIME_LOCK_PPQN_DEFAULT; }
static u32 ppqn_index(u32 c)
{
    u32 i = 0;
    while (i + 1 < PPQN_STEPS && ppqn_steps[i] < ppqn_of(c)) ++i;
    return i;
}

/* The fields shown, in cursor order: Speed only with Speed, the clock's
 * input and pulses only with a CV clock. */
static u32 fields_shown(u32 c, u8 *out)
{
    u32 n = 0, tempo = tempo_at(c);
    for (u32 f = 0; f < FIELDS; ++f) {
        if (f == FIELD_SPEED && tempo != TIME_LOCK_TEMPO_SPEED) continue;
        if ((f == FIELD_INPUT || f == FIELD_PPQN) && tempo != TIME_LOCK_TEMPO_CV) continue;
        out[n++] = (u8)f;
    }
    return n;
}
static u32 field_at(u32 c, u32 cursor)
{
    u8 shown[FIELDS];
    u32 n = fields_shown(c, shown);
    return cursor < n ? shown[cursor] : FIELDS;
}
static u32 count(u32 channel) { u8 shown[FIELDS]; return fields_shown(channel, shown); }

/* ---- text ---- */

static void heading(s32 x, s32 y, const char *line)
{
    ui_draw_proportional_text(UI_HEADING_FONT, x, y, line, COLOR_TEAL, COLOR_BLACK, 1, 0);
}
static void text(s32 x, s32 y, const char *line, u32 selected, u32 gray)
{
    ui_draw_proportional_text(UI_RENDER_DEFAULT_FONT_CONTEXT, x, y, line,
        selected ? COLOR_BLACK : gray ? COLOR_GRAY : COLOR_WHITE, selected ? COLOR_SELECTED : COLOR_BLACK, 1, 0);
}
static void format_bpm(char *out, u32 bpm10) { sprintf(out, "%d.%d BPM", bpm10 / BPM_TENTHS, bpm10 % BPM_TENTHS); }
static void format_speed(char *out, s32 speed)
{
    s32 percent = TIME_LOCK_SPEED_UNITY + speed;
    sprintf(out, "%d.%d%%", percent / PERCENT_TENTHS, percent % PERCENT_TENTHS);
}
static void format_input(char *out, u32 c)
{
    signed short selector = (signed short)(time_lock_input[c] + EQ_CV_SOURCE_OFF);
    ui_format_cv_input_selector(out, &selector, (s32)c);
}
static void format_beats(char *out, const struct timelock_tempo *t)
{
    if (!t->beats) sprintf(out, "Auto");
    else sprintf(out, t->guessed ? "Auto %d" : "%d", t->beats);
}

/* A field's value as the popup and the page show it. */
static void value_text(char *out, u32 c, u32 f, const struct timelock_tempo *t)
{
    switch (f) {
    case FIELD_LOCK: sprintf(out, time_lock_channels[c] ? "On" : "Off"); break;
    case FIELD_TEMPO: sprintf(out, "%s", tempo_names[tempo_at(c)]); break;
    case FIELD_SPEED: format_speed(out, time_lock_speed[c]); break;
    case FIELD_INPUT: format_input(out, c); break;
    case FIELD_PPQN: sprintf(out, "%d PPQN", ppqn_of(c)); break;
    default: format_beats(out, t); break;
    }
}

static s32 row_y(u32 f)
{
    return f == FIELD_LOCK ? LOCK_Y + ROW_DY : f == FIELD_TEMPO ? TEMPO_Y + ROW_DY :
           f == FIELD_BEATS ? BEATS_Y + ROW_DY : TEMPO_Y + 2 * ROW_DY;
}
static s32 popup_y(s32 y) { return y > POPUP_Y_MAX ? POPUP_Y_MAX : y; }
static void popup(u32 c, u32 f, const struct timelock_tempo *t)
{
    static const char *const titles[FIELDS] = {"Time Lock", "Tempo", "Speed", "Clock In", "PPQN", "Beats"};
    char line[TEXT_BYTES];
    value_text(line, c, f, t);
    ui_render_parameter_row(UI_PARAMETER_ROW_STYLE, 0, popup_y(row_y(f) + POPUP_DY), line, titles[f], "", 0,
                            POPUP_BACKGROUND_FROM_STYLE, 0);
}

static void draw(u32 channel, u32 cursor, u32 editing)
{
    char line[TEXT_BYTES];
    u32 c = channel, zone = zone_of(), f = field_at(c, cursor), tempo = tempo_at(c);
    u32 gray = !timelock_wanted(c), tempo_gray = gray && c; /* channel 1's tempo serves the others */
    struct timelock_tempo t;
    timelock_tempo(c, zone, &t);
    heading(LEFT_X, LOCK_Y, "TIME LOCK");
    ui_draw_zone_label_blinking_when_synced(zone, ZONE_LABEL_X, LOCK_Y);
    value_text(line, c, FIELD_LOCK, &t);
    text(LEFT_X, LOCK_Y + ROW_DY, line, f == FIELD_LOCK, 0);

    heading(LEFT_X, TEMPO_Y, "TEMPO");
    value_text(line, c, FIELD_TEMPO, &t);
    text(LEFT_X, TEMPO_Y + ROW_DY, line, f == FIELD_TEMPO, tempo_gray);
    if (t.bpm10) format_bpm(line, t.bpm10); else sprintf(line, t.clocked ? "No clock" : "");
    text(CV_COLUMN_X, TEMPO_Y + ROW_DY, line, 0, tempo_gray);
    if (tempo == TIME_LOCK_TEMPO_SPEED) {
        value_text(line, c, FIELD_SPEED, &t);
        text(LEFT_X, TEMPO_Y + 2 * ROW_DY, line, f == FIELD_SPEED, tempo_gray);
    } else if (tempo == TIME_LOCK_TEMPO_CV) {
        value_text(line, c, FIELD_INPUT, &t);
        text(LEFT_X, TEMPO_Y + 2 * ROW_DY, line, f == FIELD_INPUT, tempo_gray);
        value_text(line, c, FIELD_PPQN, &t);
        text(CV_COLUMN_X, TEMPO_Y + 2 * ROW_DY, line, f == FIELD_PPQN, tempo_gray);
    }

    heading(LEFT_X, BEATS_Y, "BEATS");
    value_text(line, c, FIELD_BEATS, &t);
    text(LEFT_X, BEATS_Y + ROW_DY, line, f == FIELD_BEATS, gray);
    if (t.own_bpm10) { format_bpm(line, t.own_bpm10); text(CV_COLUMN_X, BEATS_Y + ROW_DY, line, 0, gray); }
    if (editing && f < FIELDS) popup(c, f, &t);
}

static u32 own_revision;
static u32 revision(void)
{
    u32 c = panel_selected_channel(), zone = zone_of();
    struct timelock_tempo t;
    timelock_tempo(c, zone, &t);
    u32 sum = own_revision * 31 + zone;
    sum = sum * 31 + time_lock_channels[c];
    sum = sum * 31 + time_lock_tempo[c];
    sum = sum * 31 + (u32)time_lock_speed[c];
    sum = sum * 31 + time_lock_input[c];
    sum = sum * 31 + time_lock_ppqn[c];
    sum = sum * 31 + time_lock_beats[c][zone];
    return ((sum * 31 + t.beats) * 31 + t.own_bpm10) * 31 + t.bpm10;
}

/* ---- editing ---- */

static void set_beats(u32 c, s32 beats) { time_lock_beats[c][zone_of()] = (u8)clamp(beats, 0, TIME_LOCK_BEATS_MAX); }
static u32 beats_now(u32 c)
{
    struct timelock_tempo t;
    timelock_tempo(c, zone_of(), &t);
    return t.beats;
}

static void step(u32 channel, u32 cursor, s32 delta)
{
    u32 c = channel, f = field_at(c, cursor);
    if (!delta) return;
    switch (f) {
    case FIELD_LOCK: time_lock_channels[c] = delta > 0; break;
    case FIELD_TEMPO: time_lock_tempo[c] = (u8)clamp((s32)tempo_at(c) + (delta > 0 ? 1 : -1), (s32)tempo_first(c), TIME_LOCK_TEMPOS - 1); break;
    case FIELD_SPEED:
        time_lock_speed[c] = (s16)clamp(panel_step(time_lock_speed[c], delta, SPEED_COARSE, 0), TIME_LOCK_SPEED_MIN, TIME_LOCK_SPEED_MAX);
        break;
    case FIELD_INPUT:
        time_lock_input[c] = (u8)clamp((s32)time_lock_input[c] + delta, 0, EQ_CV_SOURCE_MAX - EQ_CV_SOURCE_OFF);
        break;
    case FIELD_PPQN: time_lock_ppqn[c] = ppqn_steps[clamp((s32)ppqn_index(c) + (delta > 0 ? 1 : -1), 0, PPQN_STEPS - 1)]; break;
    case FIELD_BEATS: {
        /* From Auto the guess is the starting point; below 1 is Auto again. */
        s32 beats = (s32)beats_now(c);
        if (panel_coarse()) beats = delta > 0 ? beats * 2 : beats / 2;
        else beats += delta;
        set_beats(c, beats);
        break;
    }
    default: return;
    }
    ++own_revision;
}

static s32 linear(s32 low, s32 high, u32 q16) { return low + (s32)((q16 * (u32)(high - low) + 0x8000u) >> 16); }
static void position(u32 channel, u32 cursor, u32 q16)
{
    u32 c = channel, f = field_at(c, cursor);
    switch (f) {
    case FIELD_LOCK: time_lock_channels[c] = q16 >= 0x8000u; break;
    case FIELD_TEMPO: time_lock_tempo[c] = (u8)linear((s32)tempo_first(c), TIME_LOCK_TEMPOS - 1, q16); break;
    case FIELD_SPEED: time_lock_speed[c] = (s16)linear(TIME_LOCK_SPEED_MIN, TIME_LOCK_SPEED_MAX, q16); break;
    case FIELD_INPUT: time_lock_input[c] = (u8)linear(0, EQ_CV_SOURCE_MAX - EQ_CV_SOURCE_OFF, q16); break;
    case FIELD_PPQN: time_lock_ppqn[c] = ppqn_steps[linear(0, PPQN_STEPS - 1, q16)]; break;
    case FIELD_BEATS: set_beats(c, linear(0, TIME_LOCK_BEATS_MAX, q16)); break;
    default: return;
    }
    ++own_revision;
}

/* Hold ENTER: Tempo follows channel 1 again, Beats is Auto again, or Speed is 100% again. */
static u32 hold(u32 channel, u32 cursor)
{
    u32 c = channel, f = field_at(c, cursor);
    if (f == FIELD_BEATS && time_lock_beats[c][zone_of()]) time_lock_beats[c][zone_of()] = 0;
    else if (f == FIELD_SPEED && time_lock_speed[c]) time_lock_speed[c] = 0;
    else if (f == FIELD_TEMPO && c && time_lock_tempo[c] != TIME_LOCK_TEMPO_CHANNEL_1) time_lock_tempo[c] = TIME_LOCK_TEMPO_CHANNEL_1;
    else return 0;
    ++own_revision;
    return 1;
}
static void no_sample(u32 channel) { (void)channel; }

const struct panel_page timelock_page = {
    FIELDS, draw, step, position, revision, no_sample, 0, 0, hold, count, 1, 0};

void timelock_page_init(void) { patch_pitch_page = &timelock_page; }
