#include "patch.h"

/* Update progress and outcome messages in the stock status band.
 *
 * The OLED cannot be drawn directly during boot: stock init reconfigures the
 * GPMC display bus early and writes stall until its own OLED bring-up, so all
 * feedback waits for the stock UI and goes through its status record. */

void ui_operation_status_record_set_timed(void *record, const char *text, s32 ticks, u32 color);

/* Operator-initiated steps: always shown. */
void status_show(const char *text)
{
    ui_operation_status_record_set_timed((void *)UI_OPERATION_STATUS_STATE, text,
                                         STATUS_MESSAGE_TICKS, UI_STATUS_TEXT_COLOR);
}

/* Background notices: shown only over an idle band or a timed message, never
 * over stock progress or persistent text such as folder loading. */
static int status_notice_for(const char *text, s32 ticks)
{
    u8 mode = BYTE(UI_OPERATION_STATUS_STATE + UI_OPERATION_STATUS_MODE);
    if (mode != UI_OPERATION_STATUS_MODE_IDLE && mode != UI_OPERATION_STATUS_MODE_TIMED) return 0;
    ui_operation_status_record_set_timed((void *)UI_OPERATION_STATUS_STATE, text, ticks, UI_STATUS_TEXT_COLOR);
    return 1;
}
static int status_notice(const char *text) { return status_notice_for(text, STATUS_MESSAGE_TICKS); }

/* A short notice about the field just selected. */
void status_hint(const char *text) { status_notice_for(text, STATUS_HINT_TICKS); }

/* Panel tick: a running trial says so until promotion; a stable build that
 * returned from a failed trial says so once, as does one that found no
 * extension image on the boot volume or the front card. */
void status_panel_tick(void)
{
    static u32 next_notice = STATUS_NOTICE_START_TICKS, failure_shown, missing_shown;
    u32 now = REG32(TICKS);
    if ((s32)(now - next_notice) < 0) return;
    next_notice = now + STATUS_NOTICE_REFRESH_TICKS;
    if (boot_mode == BOOT_TRIAL) status_notice("Checking update");
    else if (boot_trial_failed && !failure_shown) failure_shown = status_notice("Update not installed");
    else if (extension_missing() && !missing_shown) missing_shown = status_notice("Extension not found");
}
