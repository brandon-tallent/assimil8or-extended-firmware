#include "ext.h"

/* The module's Grain and Grain CV pages, which Phase Mod steps to from its
 * own page (src/eq_panel.c owns the cursor, the edit state and the screen).
 * ENTER opens the stock edit popup. Grain shows On for the channel that
 * plays grains; the other settings belong to the one engine, whichever
 * channel is selected. Sample on either page resamples the selected channel
 * (resample.c). */

int sprintf(char *buffer, const char *format, ...);

#define ROW_PITCH              23
#define CELL_VALUE_DY          11
#define POPUP_DY               17
#define POPUP_Y_MAX            104      /* the popup's panel stays on the screen */
#define NARROW_COLUMN_WIDTH    38       /* three to a row, as the stock Bits page's last row */
#define TEXT_BYTES             24u
#define SIZE_OCTAVES_Q16       784190u  /* log2(20000 / 5): DATA 2 sweeps Size by ratio */
#define CENTS_PER_OCTAVE       1200u
#define STEP_RATIO_SHIFT       5u       /* Size, Spray and Detune step by 1/32 of their value */
#define STEP_COARSE            5        /* Overlap, Scan, Slide and Skew */
#define TENTHS                 10
#define HUNDREDTHS             100
#define ROUTE_VOLTS            5        /* a route's span is five volts at +1.00 */

/* The Grain page: three rows of two cells, then two rows of three. */
enum { WIDE = 2, NARROW = 3 };
static const struct cell { u8 field, row, column, columns; const char *heading, *title; } cells[GRAIN_FIELDS] = {
    {GRAIN_ON, 0, 0, WIDE, "GRAIN", "Grain"}, {GRAIN_SIZE, 0, 1, WIDE, "SIZE", "Grain Size"},
    {GRAIN_OVERLAP, 1, 0, WIDE, "OVERLAP", "Overlap"}, {GRAIN_SPRAY, 1, 1, WIDE, "SPRAY", "Spray"},
    {GRAIN_DETUNE, 2, 0, WIDE, "DETUNE", "Detune"}, {GRAIN_SLIDE, 2, 1, WIDE, "SLIDE", "Slide"},
    {GRAIN_SCAN, 3, 0, NARROW, "SCAN", "Scan"}, {GRAIN_PAN, 3, 1, NARROW, "PAN", "Pan Spread"},
    {GRAIN_SHAPE, 3, 2, NARROW, "SHAPE", "Shape"}, {GRAIN_SKEW, 4, 0, NARROW, "SKEW", "Skew"},
    {GRAIN_OCTAVE, 4, 1, NARROW, "OCT", "Octave Up"}, {GRAIN_REVERSE, 4, 2, NARROW, "RVRS", "Reverse"},
};
/* The Grain CV page's routes. */
static const struct { const char *heading, *title, *unit; } routes[GRAIN_ROUTES] = {
    [GRAIN_ROUTE_SIZE] = {"SIZE", "Size", "oct"}, [GRAIN_ROUTE_OVERLAP] = {"OVERLAP", "Overlap", "oct"},
    [GRAIN_ROUTE_SPRAY] = {"SPRAY", "Spray", "ms"}, [GRAIN_ROUTE_DETUNE] = {"DETUNE", "Detune", "cents"},
    [GRAIN_ROUTE_SLIDE] = {"SLIDE", "Slide", "%"}, [GRAIN_ROUTE_SCAN] = {"SCAN", "Scan", "%"},
    [GRAIN_ROUTE_SHAPE] = {"SHAPE", "Shape", "%"}, [GRAIN_ROUTE_PAN] = {"PAN", "Pan", "%"},
};

/* A signed value in tenths or hundredths. */
static void format_fraction(char *out, s32 value, s32 unit)
{
    s32 magnitude = value < 0 ? -value : value;
    sprintf(out, unit == TENTHS ? "%c%d.%d" : "%c%d.%02d", value < 0 ? '-' : '+', magnitude / unit, magnitude % unit);
}

/* The value and its unit, as the popup shows them apart. */
static void format(u32 field, s32 value, char *out, const char **unit)
{
    *unit = " %";
    switch (field) {
    case GRAIN_ON: sprintf(out, value ? "On" : "Off"); *unit = ""; break;
    case GRAIN_SIZE: sprintf(out, "%d.%d", value / TENTHS, value % TENTHS); *unit = " ms"; break;
    case GRAIN_OVERLAP: sprintf(out, "%d.%02d", value / HUNDREDTHS, value % HUNDREDTHS); *unit = " grains"; break;
    case GRAIN_SPRAY: sprintf(out, "%d", value); *unit = " ms"; break;
    case GRAIN_DETUNE: sprintf(out, "%d", value); *unit = " cents"; break;
    case GRAIN_SLIDE: format_fraction(out, value, TENTHS); break;
    case GRAIN_SKEW: sprintf(out, "%+d", value); *unit = ""; break;
    default: sprintf(out, "%d", value); break;
    }
}

static s32 popup_y(s32 y) { return y > POPUP_Y_MAX ? POPUP_Y_MAX : y; }

static void draw(u32 channel, u32 cursor, u32 editing)
{
    char text[TEXT_BYTES], line[TEXT_BYTES];
    const char *unit;
    for (u32 n = 0; n < GRAIN_FIELDS; ++n) {
        const struct cell *c = &cells[n];
        s32 x = c->columns == WIDE ? (c->column ? CV_COLUMN_X : LEFT_X) : LEFT_X + c->column * NARROW_COLUMN_WIDTH;
        s32 y = TITLE_Y + c->row * ROW_PITCH;
        ui_draw_proportional_text(UI_HEADING_FONT, x, y, c->heading, COLOR_TEAL, COLOR_BLACK, 1, 0);
        format(c->field, grain_value(channel, c->field), text, &unit);
        sprintf(line, c->field == GRAIN_OVERLAP ? "%s" : "%s%s", text, unit);
        ui_draw_proportional_text(UI_RENDER_DEFAULT_FONT_CONTEXT, x, y + CELL_VALUE_DY, line,
                                  n == cursor ? COLOR_BLACK : COLOR_WHITE, n == cursor ? COLOR_SELECTED : COLOR_BLACK, 1, 0);
    }
    if (!editing) return;
    const struct cell *c = &cells[cursor];
    format(c->field, grain_value(channel, c->field), text, &unit);
    ui_render_parameter_row(UI_PARAMETER_ROW_STYLE, 0, popup_y(TITLE_Y + c->row * ROW_PITCH + POPUP_DY), text, c->title, unit, 0,
                            POPUP_BACKGROUND_FROM_STYLE, 0);
}

static void step(u32 channel, u32 cursor, s32 delta)
{
    u32 field = cells[cursor].field;
    s32 value = grain_value(channel, field), unit = 1;
    if (field == GRAIN_ON) { grain_request(channel, field, delta > 0); return; }
    if (field == GRAIN_SIZE || field == GRAIN_SPRAY || field == GRAIN_DETUNE)
        unit = value >> STEP_RATIO_SHIFT ? value >> STEP_RATIO_SHIFT : 1;
    else if (field == GRAIN_OVERLAP || field == GRAIN_SCAN || field == GRAIN_SLIDE || field == GRAIN_SKEW) unit = STEP_COARSE;
    grain_request(channel, field, value + delta * unit);
}

static s32 linear(s32 low, s32 high, u32 q16) { return low + (s32)((q16 * (u32)(high - low) + 0x8000u) >> 16); }

static void position(u32 channel, u32 cursor, u32 q16)
{
    u32 field = cells[cursor].field;
    s32 low = grain_low(field), high = grain_high(field);
    if (field == GRAIN_SIZE) {
        /* The same ratio per turn across 0.5 ms to 2 s. */
        u32 octaves = (u32)(((u64)q16 * SIZE_OCTAVES_Q16) >> 16), whole = octaves >> 16;
        u32 ratio = grain_ratio_q16((s32)(((octaves & 0xffffu) * CENTS_PER_OCTAVE) >> 16));
        grain_request(channel, field, (s32)(((u64)((u32)low << whole) * ratio) >> 16));
        return;
    }
    grain_request(channel, field, linear(low, high, q16));
}

/* Grain CV: the cursor counts each route's source, then its amount. */
static void draw_routes(u32 channel, u32 cursor, u32 editing)
{
    for (u32 r = 0; r < GRAIN_ROUTES; ++r)
        panel_cv_route(r, routes[r].heading, grain_value(channel, GRAIN_REQUEST_ROUTE + 2 * r),
                       grain_value(channel, GRAIN_REQUEST_ROUTE + 2 * r + 1), cursor);
    if (!editing) return;
    /* What a volt adds at this amount. */
    u32 r = cursor / 2;
    s32 amount = grain_value(channel, GRAIN_REQUEST_ROUTE + 2 * r + 1), span = grain_routes[r].span;
    char rate[TEXT_BYTES], per_volt[TEXT_BYTES];
    if (!span) format_fraction(rate, amount, HUNDREDTHS);
    else if (r == GRAIN_ROUTE_SLIDE) format_fraction(rate, amount * span / (EQ_CV_AMOUNT_MAX * ROUTE_VOLTS), TENTHS);
    else sprintf(rate, "%+d", amount * span / (EQ_CV_AMOUNT_MAX * ROUTE_VOLTS));
    sprintf(per_volt, "   (%s %s/V)", rate, routes[r].unit);
    panel_cv_popup(cursor, routes[r].title, grain_value(channel, GRAIN_REQUEST_ROUTE + cursor - (cursor & 1u)), amount, per_volt);
}

static void step_route(u32 channel, u32 cursor, s32 delta)
{
    grain_request(channel, GRAIN_REQUEST_ROUTE + cursor, grain_value(channel, GRAIN_REQUEST_ROUTE + cursor) + delta);
}

static void position_route(u32 channel, u32 cursor, u32 q16)
{
    grain_request(channel, GRAIN_REQUEST_ROUTE + cursor,
                  cursor & 1u ? linear(-EQ_CV_AMOUNT_MAX, EQ_CV_AMOUNT_MAX, q16) : linear(EQ_CV_SOURCE_OFF, EQ_CV_SOURCE_MAX, q16));
}

static const struct panel_page routes_page = {2 * GRAIN_ROUTES, draw_routes, step_route, position_route, grain_revision, resample_button, 0};
const struct panel_page grain_page = {GRAIN_FIELDS, draw, step, position, grain_revision, resample_button, &routes_page};
