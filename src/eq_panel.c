#include "eq.h"
#include "panel_page.h"
/* Pages that stand in for a stock channel page while shown, in the stock
 * channel-page style: teal headings, the channel strip, the
 * corner zone label, CV source/amount rows with bars, and the stock ENTER
 * edit popup. Mutate steps Bits -> EQ -> EQ CV -> Bits; Phase Mod steps
 * Phase Mod -> the extension's pages (panel_page.h) -> Phase Mod. */
void eq_bits_stock(u32 *context, u32 argument);
void eq_render_stock(u32 *task);
void phase_entry_stock(u32 *context, u32 argument);
void phase_render_stock(u32 *task);
void phase_panel_entry(u32 *context, u32 argument);
void envelope_entry_stock(u32 *context, u32 argument);
void envelope_render_stock(u32 *task);
void envelope_panel_entry(u32 *context, u32 argument);
void pitch_entry_stock(u32 *context, u32 argument);
void pitch_render_stock(u32 *task);
void eq_value_bar(u32 object, s32 x, s32 y, u32 value_bits); /* entry.S: float in s0 */
char *ui_format_cv_input_selector(char *dest, const signed short *selector, s32 channel);
static char *format_source(char *dest, s32 source)
{
    signed short selector = (signed short)source;
    return ui_format_cv_input_selector(dest, &selector, 0);
}
int sprintf(char *buffer, const char *format, ...);

/* Stock editor record (SAMPLE_SERVICE_RECORD) fields. */
#define EDITOR_SELECTOR_ADJUST_CALLBACK 0x08u
#define EDITOR_SELECTOR_ADJUST_WORD 0x0cu   /* EDITOR_SELECTOR_TRANSIENT_WORD_0C */
#define EDITOR_SELECTOR_PAGE_STATE 0x58u
#define ZONE_EDITOR_SELECTED_CHANNEL 0x5cu
#define ZONE_EDITOR_SELECTED_ZONE 0x60u
#define BITS_PAGE_STATE 12u                 /* ZONE_EDITOR_BIT_PAGE_STATE */
#define PHASE_PAGE_STATE 15u                /* ZONE_OPERATION_PANEL_PAGE_STATE */
#define PITCH_PAGE_STATE 9u                 /* ZONE_EDITOR_PITCH_PAGE_STATE */
/* Front-panel records and callback slots (FRONT_PANEL_RECORD_*). */
#define PANEL_SAMPLE 12u                    /* FRONT_PANEL_SAMPLE_BUTTON_RECORD_INDEX */
#define PANEL_PRESS 0x00u
#define PANEL_PHASE_MOD 17u                 /* FRONT_PANEL_BINDING_PHASE_MODE_RECORD */
#define PANEL_PITCH 15u                     /* the Pitch button's front-panel record */
#define PANEL_MUTATE 18u
#define PANEL_ENVELOPE 25u                  /* FRONT_PANEL_ENVELOPE_RECORD_INDEX */
#define PANEL_DOUBLE 0x20u                  /* FRONT_PANEL_RECORD_DOUBLE_CALLBACK */
#define PANEL_ENTER 27u                     /* FRONT_PANEL_TRANSIENT_EDITOR_RECORD_INDEX */
#define PANEL_SHORT_RELEASE 0x08u
#define PANEL_HOLD 0x10u
#define PANEL_ACCEPTED_RELEASE 0x18u      /* FRONT_PANEL_RECORD_ACCEPTED_RELEASE_CALLBACK: let go after the hold */
#define PANEL_CALLBACK_SLOTS_END 0x28u
#define PANEL_HOLD_TICKS 0x30u             /* FRONT_PANEL_RECORD_HOLD_TICKS, a halfword */
#define PANEL_SELECTOR_HOLD_TICKS 750u     /* FRONT_PANEL_SELECTOR_HOLD_TICKS, as stock value screens */
#define PANEL_PRESSED 0x34u                 /* FRONT_PANEL_RECORD_INPUT_CURRENT */
#define PANEL_INPUT_PREVIOUS 0x35u
#define COLOR_GRAY 0x14a5u
#define COLOR_LIGHT_GRAY 0x18c6u
#define COLOR_DARK_GRAY 0x0c63u
#define COLOR_YELLOW 0xc0ffu
/* Layout (pixels). */
#define GRAPH_LEFT 12
#define GRAPH_TOP 24
#define GRAPH_WIDTH 116
#define GRAPH_HEIGHT 31
#define CURVE_POINTS (GRAPH_WIDTH - 2)
#define COLUMN_WIDTH 38
#define BAND_HEADING_Y 58
#define FREQUENCY_Y 69
#define GAIN_Y 80
#define Q_Y 91
#define SETTINGS_HEADING_Y 103
#define SETTINGS_Y 114
#define BAND_POPUP_Y 72
#define SETTINGS_POPUP_Y 104
#define HOLD_HINT_DX 16
#define CV_BLOCK_Y 13
#define CV_BLOCK_PITCH 27
#define CV_VALUE_DY 11
#define CV_BAR_DY 23
#define CV_AMOUNT_DX 28
#define CV_POPUP_DY 17
#define CV_BAR_SCALE 0.05f                 /* +1.00 fills the bar's half width */
#define MARKER_SIZE 3
#define CURVE_REFRESH_TICKS 40u            /* live curve under CV, ~25 Hz */
/* Push-turn steps: a semitone of frequency, a dB, an octave of Q, 10 Hz of
 * high-pass and 0.10 of a CV amount. */
#define SEMITONES_PER_OCTAVE 12
#define FREQUENCY_COARSE (EQ_STEPS_PER_OCTAVE/SEMITONES_PER_OCTAVE)
#define GAIN_COARSE 10
#define HPF_COARSE 10
#define AMOUNT_COARSE 10

/* PAGE_EXTRA and above are the extension's pages, in list order: those Phase
 * Mod steps to, from PAGE_ENVELOPE those Envelope steps to, and from
 * PAGE_PITCH those Pitch steps to. */
enum { PAGE_NONE, PAGE_EQ, PAGE_CV, PAGE_EXTRA, PAGE_ENVELOPE = 64, PAGE_PITCH = 128 };
enum { FIELD_LOW_FREQUENCY, FIELD_MID_FREQUENCY, FIELD_HIGH_FREQUENCY,
       FIELD_LOW_GAIN, FIELD_MID_GAIN, FIELD_HIGH_GAIN, FIELD_LOW_Q, FIELD_MID_Q, FIELD_HIGH_Q,
       FIELD_HPF, FIELD_ON, FIELD_SCOPE, EQ_FIELD_COUNT };
#define CV_FIELD_COUNT (EQ_MOD_COUNT*2)

static u32 page, field, edit_zone, value_edit, revision, drawn_revision, opening_press;
static u32 turned;                          /* the encoder turned while ENTER was held */
static u32 stereo_right;                    /* the Stereo/Right button selected the right side */
static u32 host = BITS_PAGE_STATE;          /* the stock page state ours are shown under */
static u32 curve_at, extra_revision;
static struct eq_value drawn_live;
static u8 curve[CURVE_POINTS];

static u32 editor(u32 offset) { return REG32(EQ_EDITOR + offset); }
u32 eq_panel_active(void) { return page != PAGE_NONE && editor(EDITOR_SELECTOR_PAGE_STATE) == host; }
/* The extension's page being shown, or null: none shown, or the extension left. */
static const struct panel_page *extra(void)
{
    u32 first = page >= PAGE_PITCH ? PAGE_PITCH : page >= PAGE_ENVELOPE ? PAGE_ENVELOPE : PAGE_EXTRA;
    const struct panel_page *shown = page < PAGE_EXTRA ? 0 : page >= PAGE_PITCH ? patch_pitch_page :
                                     page >= PAGE_ENVELOPE ? patch_envelope_page : patch_panel_page;
    for (u32 n = first; shown && n < page; ++n) shown = shown->next;
    return shown;
}
static u32 channel(void) { return editor(ZONE_EDITOR_SELECTED_CHANNEL) & 7; }
static u32 zone(void) { return editor(ZONE_EDITOR_SELECTED_ZONE) & 7; }
static u32 following(void) { return page < PAGE_EXTRA && edit_zone && !eq_zones[channel()][zone()].override; }
static struct eq_value *value(void)
{
    u32 ch = channel(), z = zone();
    return edit_zone && eq_zones[ch][z].override ? &eq_zones[ch][z] : &eq_channels[ch];
}
static u32 field_count(void)
{
    const struct panel_page *shown = extra();
    return shown ? shown->count ? shown->count(channel()) : shown->fields : page == PAGE_CV ? CV_FIELD_COUNT : EQ_FIELD_COUNT;
}
static u32 stereo_right_of(u32 ch)
{
    return ch + 1 < 8 && BYTE(EQ_CHANNEL_RECORDS + (ch + 1)*EQ_CHANNEL_BYTES + EQ_CHANNEL_MODE) == EQ_CHANNEL_MODE_STEREO_RIGHT;
}
u32 panel_stereo_right(void) { return stereo_right && stereo_right_of(channel()); }
void eq_panel_selection_changed(void)
{
    if (!eq_panel_active()) return;
    revision++;
    /* Stock selects the master instead and shows a notice. Here the press
     * selected the right side, which the page already shows, so clear the
     * notice. */
    if (panel_stereo_right() && BYTE(UI_OPERATION_STATUS_STATE + UI_OPERATION_STATUS_MODE) == UI_OPERATION_STATUS_MODE_TIMED)
        BYTE(UI_OPERATION_STATUS_STATE + UI_OPERATION_STATUS_MODE) = UI_OPERATION_STATUS_MODE_IDLE;
}
/* Called on a channel button press, before stock handles it. button: 0-7. */
void eq_panel_channel_gesture(u32 *context, u32 button)
{
    (void)context;
    u32 mutate = FRONT_PANEL_STATE + PANEL_MUTATE*FRONT_PANEL_RECORD_SIZE;
    /* Stock checks the channel buttons before Mutate. After a Mutate+channel
     * mute chord, mark Mutate's release as handled so it does not also
     * change the page. */
    if (editor(EDITOR_SELECTOR_PAGE_STATE) == BITS_PAGE_STATE && BYTE(mutate + PANEL_PRESSED))
        BYTE(mutate + PANEL_INPUT_PREVIOUS) = 1;
    else if (eq_panel_active() && button < 8) {
        stereo_right = button && stereo_right_of(button - 1);
        revision++;
    }
}
static void bind(u32 button, u32 offset, void (*callback)(u32 *, u32))
{
    REG32(FRONT_PANEL_STATE + button*FRONT_PANEL_RECORD_SIZE + offset) = (u32)callback;
    REG32(FRONT_PANEL_STATE + button*FRONT_PANEL_RECORD_SIZE + offset + 4) = 0;
}
static void show(u32 next);
static void leave(void);
u32 panel_coarse(void) { return BYTE(FRONT_PANEL_STATE + PANEL_ENTER*FRONT_PANEL_RECORD_SIZE + PANEL_PRESSED) != 0; }
s32 panel_step(s32 value, s32 delta, s32 coarse, s32 anchor)
{
    if (!panel_coarse() || coarse <= 1 || !delta) return value + delta;
    /* Round to the grid point at or below (up when going down), then step. */
    s32 offset = value - anchor;
    s32 below = offset >= 0 ? offset/coarse*coarse : -((coarse - 1 - offset)/coarse*coarse);
    if (delta < 0 && below != offset) below += coarse;
    return anchor + below + delta*coarse;
}
/* Envelope pressed on its own page or one of the extension's: the next page,
 * and after the last the Envelope page again. A second press inside the
 * double-press window is the stock zero-crossing toggle, which belongs to the
 * loop pages; here the double binding is dropped so a press acts at once. */
static void envelope_panel_step(u32 *context, u32 argument)
{
    page = eq_panel_active() && page >= PAGE_ENVELOPE ? page + 1 : PAGE_ENVELOPE;
    if (extra()) show(page); else envelope_panel_entry(context, argument);
}
void envelope_panel_entry(u32 *context, u32 argument)
{
    envelope_entry_stock(context, argument);
    leave();
    if (patch_envelope_page) {
        bind(PANEL_ENVELOPE, PANEL_SHORT_RELEASE, envelope_panel_step);
        bind(PANEL_ENVELOPE, PANEL_DOUBLE, 0);
    }
}
/* Pitch pressed on its own page or one of the extension's: the next page,
 * and after the last the Pitch page again. From any other page it is the
 * stock Pitch page. */
void pitch_panel_entry(u32 *context, u32 argument);
static void pitch_panel_step(u32 *context, u32 argument)
{
    u32 here = editor(EDITOR_SELECTOR_PAGE_STATE) == PITCH_PAGE_STATE;
    page = eq_panel_active() && page >= PAGE_PITCH ? page + 1 : PAGE_PITCH;
    if (here && extra()) show(page); else pitch_panel_entry(context, argument);
}
/* In place of the stock Pitch page entry: with the extension's pages to
 * show, a second press steps to them. A Stereo/Right channel keeps the stock
 * notice. */
void pitch_panel_entry(u32 *context, u32 argument)
{
    pitch_entry_stock(context, argument);
    leave();
    if (patch_pitch_page && editor(EDITOR_SELECTOR_PAGE_STATE) == PITCH_PAGE_STATE) {
        bind(PANEL_PITCH, PANEL_SHORT_RELEASE, pitch_panel_step);
        bind(PANEL_PITCH, PANEL_DOUBLE, 0);
    }
}
static void apply_value(struct eq_value *next);
/* The value being edited, flat: the EQ page's fields at their defaults, its
 * CV routes (the EQ CV page's) kept. */
static void flat(struct eq_value *next)
{
    const struct eq_value *v = value();
    eq_default(next);
    memcpy(next->source, v->source, sizeof(next->source));
    memcpy(next->amount, v->amount, sizeof(next->amount));
    next->override = v->override;
}
/* The settings a hold on EQ flattened, and whose they were (0: none),
 * until a hold brings them back, a preset is saved or loaded, or another
 * EQ is flattened. */
static struct eq_value held;
static u32 held_owner;
#define OWNER_SLOTS_PER_CHANNEL 9u /* a channel's eight zones, then the channel's own EQ */
#define OWNER_CHANNEL_SLOT      8u
static u32 owner(void)
{
    return 1 + channel()*OWNER_SLOTS_PER_CHANNEL + (edit_zone ? zone() : OWNER_CHANNEL_SLOT);
}
void eq_panel_forget(void) { held_owner = 0; }
/* What holding ENTER does here: on Edit, a zone follows its channel again;
 * on EQ, the channel (or the zone's own EQ) goes flat, or flat goes back
 * to the settings it replaced. */
enum { HOLD_NONE, HOLD_FOLLOW, HOLD_CLEAR, HOLD_RESTORE };
static u32 hold_action(void)
{
    if (page != PAGE_EQ || following()) return HOLD_NONE;
    if (field == FIELD_SCOPE) return edit_zone ? HOLD_FOLLOW : HOLD_NONE;
    if (field != FIELD_ON) return HOLD_NONE;
    struct eq_value next;
    flat(&next);
    if (memcmp(&next, value(), sizeof(next))) return HOLD_CLEAR;
    return held_owner == owner() ? HOLD_RESTORE : HOLD_NONE;
}
/* ENTER pressed: clear the turned flag. If the encoder turns before ENTER
 * is released, the release does nothing. */
static void enter_press(u32 *context, u32 argument) { (void)context; (void)argument; turned = 0; }
/* Hold time reached: do nothing yet. The hold action runs on release
 * (clear_hold), and only if the encoder did not turn. */
static void enter_held(u32 *context, u32 argument) { (void)context; (void)argument; }
static void clear_hold(u32 *context, u32 argument)
{
    (void)context; (void)argument;
    if (!eq_panel_active() || turned) return;
    const struct panel_page *shown = extra();
    if (shown) {
        if (shown->hold && shown->hold(channel(), field)) { value_edit = 0; revision++; }
        return;
    }
    struct eq_value next;
    switch (hold_action()) {
    case HOLD_FOLLOW:
        memset(&eq_zones[channel()][zone()], 0, sizeof(struct eq_value));
        if (held_owner == owner()) held_owner = 0;
        break;
    case HOLD_CLEAR:
        held = *value(); held_owner = owner();
        flat(&next); apply_value(&next);
        status_show("EQ cleared");
        break;
    case HOLD_RESTORE:
        /* CV routes set while flat stay. */
        next = held;
        memcpy(next.source, value()->source, sizeof(next.source));
        memcpy(next.amount, value()->amount, sizeof(next.amount));
        held_owner = 0;
        apply_value(&next);
        status_show("EQ restored");
        break;
    default: return;
    }
    value_edit = 0;
    revision++;
}
void sample_setup_capture_state_transaction(u32 *context, u32 argument);
/* Sample on the extension's page is the page's; anywhere else it is stock. */
static void sample_press(u32 *context, u32 argument)
{
    const struct panel_page *shown = extra();
    if (eq_panel_active() && shown && shown->sample) shown->sample(channel());
    else sample_setup_capture_state_transaction(context, argument);
}
u32 panel_scope_zone(void) { return edit_zone; }
u32 panel_selected_zone(void) { return zone(); }
u32 panel_selected_channel(void) { return channel(); }
void panel_toggle_scope(void) { edit_zone ^= 1; revision++; }
static void enter_release(u32 *context, u32 argument)
{
    (void)context; (void)argument;
    if (turned) return;
    const struct panel_page *shown = extra();
    if (shown && shown->enter && shown->enter(channel(), field)) { revision++; return; }
    /* ENTER on Edit switches scope; elsewhere it opens or closes the popup. */
    if (page == PAGE_EQ && field == FIELD_SCOPE) edit_zone ^= 1;
    else value_edit ^= 1;
    revision++;
}
void eq_panel_after_bits(u32 *context)
{
    (void)context;
    page = PAGE_NONE; value_edit = 0;
    /* Mutate opens Bits on its press; that press's release must not also
     * advance to EQ. */
    opening_press = BYTE(FRONT_PANEL_STATE + PANEL_MUTATE*FRONT_PANEL_RECORD_SIZE + PANEL_PRESSED) != 0;
    if (editor(EDITOR_SELECTOR_PAGE_STATE) == BITS_PAGE_STATE) bind(PANEL_MUTATE, PANEL_SHORT_RELEASE, eq_panel_enter);
}
/* Show one of our pages over the stock page being shown: the encoder, ENTER
 * and Sample become ours until that page's entry restores its bindings. */
static void show(u32 next)
{
    host = editor(EDITOR_SELECTOR_PAGE_STATE);
    page = next;
    field = value_edit = stereo_right = 0;
    REG32(EQ_EDITOR + EDITOR_SELECTOR_ADJUST_CALLBACK) = (u32)eq_panel_select;
    REG32(EQ_EDITOR + EDITOR_SELECTOR_ADJUST_WORD) = 0;
    bind(PANEL_SAMPLE, PANEL_PRESS, sample_press);
    /* Remove the stock value/hold callbacks without introducing a
     * double-press binding, which would defer every short release. */
    for (u32 offset = 0; offset < PANEL_CALLBACK_SLOTS_END; offset += 8) bind(PANEL_ENTER, offset, 0);
    bind(PANEL_ENTER, PANEL_PRESS, enter_press);
    bind(PANEL_ENTER, PANEL_SHORT_RELEASE, enter_release);
    bind(PANEL_ENTER, PANEL_HOLD, enter_held);
    bind(PANEL_ENTER, PANEL_ACCEPTED_RELEASE, clear_hold);
    *(volatile u16 *)(FRONT_PANEL_STATE + PANEL_ENTER*FRONT_PANEL_RECORD_SIZE + PANEL_HOLD_TICKS) = PANEL_SELECTOR_HOLD_TICKS;
    turned = 0;
    REG32(CDISPLAY_TASK_OBJECT + UI_PAGE_RENDER_PREVIOUS_PAGE) = ~0u;
    revision++;
}
/* Back to the stock page. It shares our pages' page number, so its renderer
 * sees no change unless the previous page is invalidated. */
static void leave(void)
{
    page = PAGE_NONE; value_edit = 0;
    REG32(CDISPLAY_TASK_OBJECT + UI_PAGE_RENDER_PREVIOUS_PAGE) = ~0u;
}
void eq_panel_enter(u32 *context, u32 argument)
{
    (void)argument;
    if (page == PAGE_NONE && opening_press) { opening_press = 0; return; }
    if (eq_panel_active() && page == PAGE_CV) {
        eq_bits_stock(context, 0);
        eq_panel_after_bits(context);
        leave();
        return;
    }
    show(eq_panel_active() ? page + 1 : PAGE_EQ);
    bind(PANEL_MUTATE, PANEL_SHORT_RELEASE, eq_panel_enter);
}
/* Phase Mod pressed on its own page or one of the extension's: the next
 * page, and after the last the Phase Mod page again. */
static void phase_panel_step(u32 *context, u32 argument)
{
    page = eq_panel_active() && page >= PAGE_EXTRA ? page + 1 : PAGE_EXTRA;
    if (extra()) show(page); else phase_panel_entry(context, argument);
}
/* In place of the stock Phase Mod page entry, which binds a second press of
 * the button to the Home page: with the extension's pages to show, that
 * press steps to them instead. */
void phase_panel_entry(u32 *context, u32 argument)
{
    phase_entry_stock(context, argument);
    leave();
    if (patch_panel_page) bind(PANEL_PHASE_MOD, PANEL_PRESS, phase_panel_step);
}

static void apply_value(struct eq_value *next)
{
    eq_limit(next);
    if (!memcmp(next, value(), sizeof(*next))) return;
    if (edit_zone) { next->override = 1; eq_zones[channel()][zone()] = *next; }
    else eq_channels[channel()] = *next;
    revision++;
}
static s32 clamp(s32 x, s32 low, s32 high) { return x < low ? low : x > high ? high : x; }
/* Encoder steps and DATA 2 positions: each field takes a 0..1 position
 * across its range, and steps by one unit. */
static void set_position(struct eq_value *v, u32 f, float x)
{
    if (page == PAGE_CV) {
        u32 m = f/2;
        if (f%2) v->amount[m] = (signed char)((s32)(x*2*EQ_CV_AMOUNT_MAX+0.5f)-EQ_CV_AMOUNT_MAX);
        else v->source[m] = (signed char)((s32)(x*(EQ_CV_SOURCE_MAX-EQ_CV_SOURCE_OFF)+0.5f)+EQ_CV_SOURCE_OFF);
    } else if (f <= FIELD_HIGH_FREQUENCY) v->frequency[f] = (u16)(x*EQ_STEP_MAX+0.5f);
    else if (f <= FIELD_HIGH_GAIN) {
        s32 limit = eq_gain_limit(v, f-FIELD_LOW_GAIN);
        v->gain[f-FIELD_LOW_GAIN] = (signed short)((s32)(x*2*limit+0.5f)-limit);
    } else if (f <= FIELD_HIGH_Q) v->q[f-FIELD_LOW_Q] = (u8)(x*EQ_Q_STEPS+0.5f);
    else if (f == FIELD_HPF) {
        s32 hpf = (s32)(x*(EQ_HPF_MAX-EQ_HPF_MIN+1)+0.5f);
        v->hpf = (u16)(hpf ? hpf+EQ_HPF_MIN-1 : 0);
    } else v->on = x >= 0.5f;
}
static void step(struct eq_value *v, u32 f, s32 delta)
{
    if (page == PAGE_CV) {
        u32 m = f/2;
        if (f%2) v->amount[m] = (signed char)clamp(panel_step(v->amount[m], delta, AMOUNT_COARSE, 0), -EQ_CV_AMOUNT_MAX, EQ_CV_AMOUNT_MAX);
        else v->source[m] = (signed char)clamp(v->source[m]+delta, EQ_CV_SOURCE_OFF, EQ_CV_SOURCE_MAX);
    } else if (f <= FIELD_HIGH_FREQUENCY)
        v->frequency[f] = (u16)clamp(panel_step(v->frequency[f], delta, FREQUENCY_COARSE, EQ_STEP_1K), 0, EQ_STEP_MAX);
    else if (f <= FIELD_HIGH_GAIN) {
        s32 limit = eq_gain_limit(v, f-FIELD_LOW_GAIN);
        v->gain[f-FIELD_LOW_GAIN] = (signed short)clamp(panel_step(v->gain[f-FIELD_LOW_GAIN], delta, GAIN_COARSE, 0), -limit, limit);
    } else if (f <= FIELD_HIGH_Q)
        v->q[f-FIELD_LOW_Q] = (u8)clamp(panel_step(v->q[f-FIELD_LOW_Q], delta, EQ_Q_STEPS_PER_OCTAVE, 1), EQ_Q_OWN, EQ_Q_STEPS);
    else if (f == FIELD_HPF) {
        /* Off, then 20 Hz at 1; push-turn lands on whole tens of hertz. */
        s32 hpf = clamp(panel_step(v->hpf ? v->hpf-(s32)EQ_HPF_MIN+1 : 0, delta, HPF_COARSE, 1), 0, EQ_HPF_MAX-EQ_HPF_MIN+1);
        v->hpf = (u16)(hpf ? hpf+EQ_HPF_MIN-1 : 0);
    } else if (delta) v->on = delta > 0;
}
void eq_panel_select(u32 *context, s32 delta)
{
    (void)context;
    const struct panel_page *shown = extra();
    if (page >= PAGE_EXTRA && !shown) return;
    if (panel_coarse()) turned = 1;
    if (shown && value_edit) shown->step(channel(), field, delta);
    else if (value_edit && !(page == PAGE_EQ && field == FIELD_SCOPE)) {
        struct eq_value next = *value();
        step(&next, field, delta);
        apply_value(&next);
    } else {
        field = (u32)clamp((s32)field+delta, 0, (s32)field_count()-1);
        value_edit = 0;
    }
    revision++;
}
/* DATA 2 sets the selected field from its position as soon as it moves,
 * as the stock pages' setters do. */
void eq_panel_knob(u32 *context, u32 bits)
{
    (void)context;
    union { u32 bits; float value; } knob = { .bits = bits };
    float x = knob.value;
    if (x != x) return;
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    if (page >= PAGE_EXTRA) {
        const struct panel_page *shown = extra();
        if (shown) shown->position(channel(), field, (u32)(x*65536.0f));
        return;
    }
    if (page == PAGE_EQ && field == FIELD_SCOPE) {
        u32 next = x >= 0.5f;
        if (next != edit_zone) { edit_zone = next; revision++; }
        return;
    }
    struct eq_value next = *value();
    set_position(&next, field, x);
    apply_value(&next);
}

/* Drawing. */
static void heading(s32 x, s32 y, const char *text)
{
    ui_draw_proportional_text(UI_HEADING_FONT, x, y, text, COLOR_TEAL, COLOR_BLACK, 1, 0);
}
static void value_text(s32 x, s32 y, const char *text, u32 selected)
{
    ui_draw_proportional_text(UI_RENDER_DEFAULT_FONT_CONTEXT, x, y, text,
        selected ? COLOR_BLACK : following() ? COLOR_GRAY : COLOR_WHITE,
        selected ? COLOR_SELECTED : COLOR_BLACK, 1, 0);
}
static void pixel(s32 x, s32 y, u16 color)
{
    ((volatile u16 *)OLED_SHADOW_FRAMEBUFFER_BASE)[y*OLED_SIDE+x] = color;
}
#define STEP_TENTHS_BELOW 20000u   /* centihertz: 200 Hz */
#define STEP_KHZ_FROM 100000u      /* 1 kHz */
#define STEP_KHZ_HUNDREDTHS_FROM 1000000u
#define CENTIHERTZ_PER_TENTH 10u
#define CENTIHERTZ_PER_HZ 100u
#define CENTIHERTZ_PER_KHZ_HUNDREDTH 1000u
u32 eq_step_text(char *out, u32 step)
{
    u32 c = eq_step_centihertz(step);
    if (c < STEP_TENTHS_BELOW) {
        u32 tenths = (c + CENTIHERTZ_PER_TENTH/2)/CENTIHERTZ_PER_TENTH;
        sprintf(out, "%d.%d", tenths/10, tenths%10);
        return 0;
    }
    u32 hz = (c + CENTIHERTZ_PER_HZ/2)/CENTIHERTZ_PER_HZ;
    if (c < STEP_KHZ_FROM) sprintf(out, "%d", hz);
    else if (c < STEP_KHZ_HUNDREDTHS_FROM) sprintf(out, "%d.%03d", hz/1000, hz%1000);
    else { u32 h = (c + CENTIHERTZ_PER_KHZ_HUNDREDTH/2)/CENTIHERTZ_PER_KHZ_HUNDREDTH; sprintf(out, "%d.%02d", h/100, h%100); }
    return c >= STEP_KHZ_FROM;
}
/* A band's frequency with its unit: 150.4 Hz, 1.000k. */
static void format_frequency(char *out, u32 step)
{
    char number[12];
    u32 khz = eq_step_text(number, step);
    sprintf(out, khz ? "%sk" : "%s Hz", number);
}
static void format_signed(char *out, s32 x, u32 decimals)
{
    s32 magnitude = x < 0 ? -x : x;
    if (decimals == 1) sprintf(out, "%c%d.%d", x < 0 ? '-' : '+', magnitude/10, magnitude%10);
    else sprintf(out, "%c%d.%02d", x < 0 ? '-' : '+', magnitude/100, magnitude%100);
}
static s32 graph_x(float position) { return GRAPH_LEFT+1+(s32)(position*(CURVE_POINTS-1)+0.5f); }
static void draw_graph(const struct eq_value *live)
{
    s32 top = GRAPH_TOP+1, bottom = GRAPH_TOP+GRAPH_HEIGHT-2, middle = (top+bottom)/2;
    /* Frame, 0 dB rule and decade marks, as in the stock waveform boxes. */
    oled_shadow_fill_rect(GRAPH_LEFT, GRAPH_TOP, GRAPH_WIDTH, GRAPH_HEIGHT, COLOR_LIGHT_GRAY);
    oled_shadow_fill_rect(GRAPH_LEFT+1, top, GRAPH_WIDTH-2, GRAPH_HEIGHT-2, COLOR_BLACK);
    for (s32 x = GRAPH_LEFT+1; x < GRAPH_LEFT+GRAPH_WIDTH-1; x += 2) pixel(x, middle, COLOR_DARK_GRAY);
    static const u16 decades[] = {100, 1000, 10000};
    for (u32 d = 0; d < 3; ++d)
        for (s32 y = top; y <= bottom; y += 2) pixel(graph_x(eq_frequency_position(decades[d])), y, COLOR_DARK_GRAY);
    struct eq_value shape = *live;
    shape.on = 1; /* EQ Out still shows its settings, grayed */
    eq_curve(&shape, CURVE_POINTS, curve, top, bottom);
    u16 color = live->on && !following() ? COLOR_WHITE : COLOR_GRAY;
    for (u32 n = 0; n < CURVE_POINTS; ++n) {
        s32 a = curve[n ? n-1 : 0], b = curve[n];
        if (a > b) { s32 t = a; a = b; b = t; }
        for (s32 y = a; y <= b; ++y) pixel(GRAPH_LEFT+1+(s32)n, y, color);
    }
    /* Band handles on the curve; the band being edited is yellow. */
    s32 selected_band = page == PAGE_EQ && field <= FIELD_HIGH_Q ? (s32)(field%3)
        : page == PAGE_CV && field/2 < EQ_MOD_HPF ? (s32)(field/4) : -1;
    for (u32 band = 0; band < 3; ++band) {
        s32 x = graph_x(eq_step_position(live->frequency[band])), y = curve[x-GRAPH_LEFT-1];
        x = clamp(x-1, GRAPH_LEFT+1, GRAPH_LEFT+GRAPH_WIDTH-1-MARKER_SIZE);
        y = clamp(y-1, top, bottom+1-MARKER_SIZE);
        oled_shadow_fill_rect((u32)x, (u32)y, MARKER_SIZE, MARKER_SIZE,
                              (s32)band == selected_band ? COLOR_YELLOW : COLOR_TEAL);
    }
}
/* A band's shape: its own (Shelf, or the Mid bell's Auto Q) or a bell's Q. */
static void format_q(char *out, const struct eq_value *v, u32 band, const char *prefix)
{
    if (!v->q[band]) { sprintf(out, band == EQ_BAND_MID ? "Auto" : "Shelf"); return; }
    u32 q = eq_q_hundredths(v->q[band]);
    if (q < 1000) sprintf(out, "%s%d.%02d", prefix, q/100, q%100);
    else sprintf(out, "%s%d.%d", prefix, q/100, q%100/10);
}
static void draw_eq_page(u32 *task, const struct eq_value *v)
{
    char line[24];
    heading(LEFT_X, TITLE_Y, "EQ");
    ui_draw_zone_label_blinking_when_synced(zone(), ZONE_LABEL_X, TITLE_Y);
    ui_draw_channel_current_zone_suffix(task);
    static const char *const bands[3] = {"LOW", "MID", "HIGH"};
    for (u32 band = 0; band < 3; ++band) {
        s32 x = LEFT_X+(s32)band*COLUMN_WIDTH;
        heading(x, BAND_HEADING_Y, bands[band]);
        format_frequency(line, v->frequency[band]);
        value_text(x, FREQUENCY_Y, line, field == FIELD_LOW_FREQUENCY+band);
        format_signed(line, v->gain[band], 1);
        value_text(x, GAIN_Y, line, field == FIELD_LOW_GAIN+band);
        format_q(line, v, band, "Q");
        value_text(x, Q_Y, line, field == FIELD_LOW_Q+band);
    }
    heading(LEFT_X, SETTINGS_HEADING_Y, "HPF");
    heading(LEFT_X+COLUMN_WIDTH, SETTINGS_HEADING_Y, "EQ");
    /* On EDIT, a zone with its own EQ shows how to let it follow again;
     * the hint starts in the gap after the EQ heading to fit. (On EQ the
     * hint goes to the status line: see eq_panel_select.) */
    if (field == FIELD_SCOPE && hold_action() == HOLD_FOLLOW)
        heading(LEFT_X+2*COLUMN_WIDTH-HOLD_HINT_DX, SETTINGS_HEADING_Y, "HOLD:CLR");
    else heading(LEFT_X+2*COLUMN_WIDTH, SETTINGS_HEADING_Y, "EDIT");
    if (v->hpf) sprintf(line, "%d Hz", v->hpf); else sprintf(line, "Off");
    value_text(LEFT_X, SETTINGS_Y, line, field == FIELD_HPF);
    value_text(LEFT_X+COLUMN_WIDTH, SETTINGS_Y, v->on ? "On" : "Off", field == FIELD_ON);
    ui_draw_proportional_text(UI_RENDER_DEFAULT_FONT_CONTEXT, LEFT_X+2*COLUMN_WIDTH, SETTINGS_Y,
        edit_zone ? "Zone" : "Chan", field == FIELD_SCOPE ? COLOR_BLACK : COLOR_WHITE,
        field == FIELD_SCOPE ? COLOR_SELECTED : COLOR_BLACK, 1, 0);
}
static const char *const mod_headings[EQ_MOD_COUNT] = {
    "LOW FREQ", "LOW GAIN", "MID FREQ", "MID GAIN", "HI FREQ", "HI GAIN", "HPF"};
static const char *const mod_titles[EQ_MOD_COUNT] = {
    "Low Freq", "Low Gain", "Mid Freq", "Mid Gain", "High Freq", "High Gain", "HPF"};
/* A CV source and amount on one line at (x, y), the amount's bar under it.
 * `selected` is 1 for the source, 2 for the amount, else 0. */
void panel_cv_pair(s32 x, s32 y, s32 source, s32 amount, u32 selected, u32 gray)
{
    char line[24];
    for (u32 n = 0; n < 2; ++n) {
        u32 on = selected == n + 1;
        if (n) format_signed(line, amount, 2); else format_source(line, source);
        ui_draw_proportional_text(UI_RENDER_DEFAULT_FONT_CONTEXT, x + (s32)n*CV_AMOUNT_DX, y, line,
            on ? COLOR_BLACK : gray ? COLOR_GRAY : COLOR_WHITE, on ? COLOR_SELECTED : COLOR_BLACK, 1, 0);
    }
    union { float value; u32 bits; } bar = { .value = amount*CV_BAR_SCALE };
    eq_value_bar(UI_VALUE_BAR_OBJECT, x+CV_AMOUNT_DX, y+CV_BAR_DY-CV_VALUE_DY, bar.bits);
}
/* A page of CV routes, two to a row: a route's heading, source, amount and
 * bar. The page's cursor counts source then amount for each route. */
void panel_cv_route(u32 route, const char *title, s32 source, s32 amount, u32 cursor)
{
    s32 x = route%2 ? CV_COLUMN_X : LEFT_X, y = CV_BLOCK_Y+(s32)(route/2)*CV_BLOCK_PITCH;
    heading(x, y, title);
    panel_cv_pair(x, y+CV_VALUE_DY, source, amount, cursor == route*2 ? 1 : cursor == route*2+1 ? 2 : 0, following());
}
/* The edit popup for a route's source or amount, its top at y; `per_volt`
 * follows an amount. */
void panel_cv_popup_at(s32 y, u32 is_amount, const char *name, s32 source, s32 amount, const char *per_volt)
{
    char value_line[24], title[24];
    sprintf(title, "%s Mod %s", name, is_amount ? "Amount" : "Source");
    if (is_amount) format_signed(value_line, amount, 2); else format_source(value_line, source);
    ui_render_parameter_row(UI_PARAMETER_ROW_STYLE, 0, y, value_line, title, is_amount ? per_volt : "", 0,
                            POPUP_BACKGROUND_FROM_STYLE, 0);
}
/* The edit popup for the cursor's field on a page of routes. */
void panel_cv_popup(u32 cursor, const char *name, s32 source, s32 amount, const char *per_volt)
{
    panel_cv_popup_at(CV_BLOCK_Y+(s32)(cursor/4)*CV_BLOCK_PITCH+CV_POPUP_DY, cursor%2, name, source, amount, per_volt);
}
static void draw_cv_page(const struct eq_value *v)
{
    for (u32 m = 0; m < EQ_MOD_COUNT; ++m) panel_cv_route(m, mod_headings[m], v->source[m], v->amount[m], field);
}
static const char *const field_titles[EQ_FIELD_COUNT] = {
    "Low Frequency", "Mid Frequency", "High Frequency", "Low Gain", "Mid Gain", "High Gain",
    "Low Q", "Mid Q", "High Q", "High-Pass Filter", "EQ", "Edit"};
static void draw_popup(const struct eq_value *v)
{
    char value_line[24], suffix[24], title[24];
    suffix[0] = 0;
    if (page == PAGE_CV) {
        u32 m = field/2;
        char per_volt[12];
        if (m%2 && m != EQ_MOD_HPF) {
            format_signed(per_volt, v->amount[m]*EQ_CV_GAIN_TENTHS_PER_VOLT/EQ_CV_AMOUNT_MAX, 1);
            sprintf(suffix, "   (%s dB/V)", per_volt);
        } else {
            format_signed(per_volt, v->amount[m], 2);
            sprintf(suffix, "   (%s oct/V)", per_volt);
        }
        panel_cv_popup(field, mod_titles[m], v->source[m], v->amount[m], suffix);
        return;
    }
    s32 y = field < FIELD_HPF ? BAND_POPUP_Y : SETTINGS_POPUP_Y;
    sprintf(title, "%s", field_titles[field]);
    if (field <= FIELD_HIGH_FREQUENCY) {
        sprintf(suffix, eq_step_text(value_line, v->frequency[field]) ? " kHz" : " Hz");
    } else if (field <= FIELD_HIGH_GAIN) {
        format_signed(value_line, v->gain[field-FIELD_LOW_GAIN], 1);
        sprintf(suffix, " dB");
    } else if (field <= FIELD_HIGH_Q) format_q(value_line, v, field-FIELD_LOW_Q, "");
    else if (field == FIELD_HPF) {
        if (v->hpf) { sprintf(value_line, "%d", v->hpf); sprintf(suffix, " Hz"); }
        else sprintf(value_line, "Off");
    } else sprintf(value_line, v->on ? "On" : "Off");
    ui_render_parameter_row(UI_PARAMETER_ROW_STYLE, 0, y, value_line, title, suffix, 0,
                            POPUP_BACKGROUND_FROM_STYLE, 0);
}
static void render(u32 *task)
{
    const struct panel_page *shown = extra();
    if (shown && shown->revision() != extra_revision) { extra_revision = shown->revision(); revision++; }
    const struct eq_value *v = value();
    u32 full = REG32(CDISPLAY_TASK_OBJECT + UI_PAGE_RENDER_PREVIOUS_PAGE) == ~0u;
    static u32 drawn_status, drawn_channel, drawn_zone;
    static struct eq_value drawn_value;
    u32 status = BYTE(UI_OPERATION_STATUS_STATE + UI_OPERATION_STATUS_MODE);
    u32 ch = channel(), z = zone(), now = REG32(TICKS);
    u32 settled = !full && drawn_status == status && drawn_revision == revision && drawn_channel == ch &&
        drawn_zone == z && !memcmp(&drawn_value, v, sizeof(*v));
    /* The graph follows CV live at a bounded rate; the fields show the
     * stored settings, as stock pages do. */
    struct eq_value live = *v;
    if (page == PAGE_EQ && eq_modulated(v)) {
        if (!settled || (u32)(now - curve_at) >= CURVE_REFRESH_TICKS) { curve_at = now; eq_modulate(v, ch, &live); }
        else live = drawn_live;
    }
    if (settled && shown && shown->live && (u32)(now - curve_at) >= CURVE_REFRESH_TICKS) {
        curve_at = now;
        REG32(OLED_TRANSFER_BATCH_GATE)++;
        u32 drew = shown->live(ch);
        REG32(OLED_TRANSFER_BATCH_GATE)--;
        if (drew) oled_enqueue_transfer(OLED_TRANSFER_KIND_RECT, GRAPH_LEFT, GRAPH_TOP, GRAPH_WIDTH, GRAPH_HEIGHT);
    }
    u32 graph_only = settled && memcmp(&drawn_live, &live, sizeof(live));
    if (settled && !graph_only) {
        if (page == PAGE_EQ || (shown && shown->zoned)) ui_draw_channel_current_zone_suffix(task);
        return;
    }
    drawn_status = status; drawn_revision = revision; drawn_channel = ch; drawn_zone = z;
    drawn_value = *v; drawn_live = live;
    REG32(OLED_TRANSFER_BATCH_GATE)++;
    if (graph_only) {
        draw_graph(&live);
        REG32(OLED_TRANSFER_BATCH_GATE)--;
        oled_enqueue_transfer(OLED_TRANSFER_KIND_RECT, GRAPH_LEFT, GRAPH_TOP, GRAPH_WIDTH, GRAPH_HEIGHT);
        return;
    }
    /* Stock parameter pages leave rows 0..10 to the meter/status band. */
    oled_shadow_fill_rect(0, PAGE_TOP, OLED_SIDE, PAGE_HEIGHT, COLOR_BLACK);
    ui_draw_channel_mix_strip(0, ch);
    if (page == PAGE_EQ) { draw_graph(&live); draw_eq_page(task, v); }
    else if (page == PAGE_CV) draw_cv_page(v);
    else {
        shown->draw(ch, field, value_edit);
        if (shown->zoned) ui_draw_channel_current_zone_suffix(task);
    }
    if (value_edit && !shown) draw_popup(v);
    REG32(OLED_TRANSFER_BATCH_GATE)--;
    oled_enqueue_transfer(OLED_TRANSFER_KIND_RECT, 0, PAGE_TOP, OLED_SIDE, PAGE_HEIGHT);
}
void eq_panel_render(u32 *task)
{
    if (eq_panel_active()) render(task); else eq_render_stock(task);
}
void envelope_panel_render(u32 *task)
{
    if (eq_panel_active() && !extra()) leave();
    if (eq_panel_active()) render(task); else envelope_render_stock(task);
}
void pitch_panel_render(u32 *task)
{
    if (eq_panel_active() && !extra()) leave();
    if (eq_panel_active()) render(task); else pitch_render_stock(task);
}
void phase_panel_render(u32 *task)
{
    /* The extension left while its page was shown. */
    if (eq_panel_active() && !extra()) leave();
    if (eq_panel_active()) render(task); else phase_render_stock(task);
}
