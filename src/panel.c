#include "patch.h"

/* Remote front panel.
 *
 * Buttons: the stock dispatcher reads every button through one
 * gpio_read_input_bit call and treats a low pin as pressed. That call is
 * redirected here with the button record, so a held remote button reads
 * pressed and the stock debounce, hold, double-press, and chord logic run
 * unchanged. Encoder: the dispatcher turns changes of a four-bit position,
 * maintained by the audio IRQ's quadrature decoder, into steps; a remote
 * step moves that position by one. Steps are spaced beyond the stock
 * acceleration window so each arrives as exactly one step. */

u32 gpio_read_input_bit(u32 base, u32 pin);

static struct {
    u32 active, sequence;
    u32 held, release_at;
    s32 steps;
    u32 next_step_at, done_at;
} action;

u32 panel_pin_read(u32 base, u32 pin, u32 record)
{
    u32 level = gpio_read_input_bit(base, pin);
    if (!action.held) return level;
    u32 index = (record - FRONT_PANEL_STATE) / FRONT_PANEL_RECORD_SIZE;
    return index < FRONT_PANEL_RECORD_COUNT && (action.held & (1u << index)) ? 0 : level;
}

/* Returns a result code; OK means the action started and will reply when the
 * dispatcher has seen it complete. */
u32 panel_start(u32 sequence, u32 buttons, u32 hold_ticks, s32 steps)
{
    if (action.active) return BUSY;
    if (buttons >> FRONT_PANEL_RECORD_COUNT || hold_ticks > PANEL_HOLD_MAX_TICKS ||
        steps > PANEL_STEPS_MAX || steps < -(s32)PANEL_STEPS_MAX || (!buttons && !steps) ||
        (buttons && !hold_ticks)) return BAD_RANGE;
    u32 now = REG32(TICKS);
    action.sequence = sequence;
    action.steps = steps;
    action.release_at = now + hold_ticks;
    action.next_step_at = now + (buttons ? hold_ticks + PANEL_SETTLE_TICKS : 0);
    action.done_at = 0;
    MEMORY_BARRIER();
    action.held = buttons;
    action.active = 1;
    return OK;
}

void panel_tick(void)
{
    if (!action.active) return;
    u32 now = REG32(TICKS);
    if (action.held && (s32)(now - action.release_at) >= 0) action.held = 0;
    if (action.held) return;
    if (action.steps && (s32)(now - action.next_step_at) >= 0) {
        s32 direction = action.steps > 0 ? 1 : -1;
        u8 position = BYTE(PANEL_ENCODER_POSITION);
        BYTE(PANEL_ENCODER_POSITION) = (u8)((position & ~PANEL_ENCODER_POSITION_MASK) |
            ((position + direction) & PANEL_ENCODER_POSITION_MASK));
        action.steps -= direction;
        action.next_step_at = now + PANEL_ENCODER_STEP_TICKS;
    }
    if (action.steps) return;
    if (!action.done_at) action.done_at = now + PANEL_SETTLE_TICKS;
    if ((s32)(now - action.done_at) < 0) return;
    action.active = 0;
    reply(CMD_PANEL, action.sequence, OK, 0, 0);
}
