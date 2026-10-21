#include "patch.h"

static struct { u32 address, bytes; } ranges[8];
static u32 stream_period, stream_due;

void commands_handle(u32 command, u32 sequence, const u8 *p, u32 bytes)
{
    u8 out[126];
    u32 status = OK, count = 0;
    if (command >= CMD_SAMPLE_BEGIN && command <= CMD_SAMPLE_ABORT) {
        sample_transfer_command(command, sequence, p, bytes);
        return;
    }
    if (command >= CMD_RECORD_ARM && command <= CMD_RECORD_STATUS) {
        record_command(command, sequence, p, bytes);
        return;
    }
    if (command == CMD_EQ_AUTOMATE) { eq_automate(p, bytes); return; }
    if (extension_command(command, sequence, p, bytes)) return;
    if (command >= CMD_INSTALL_BEGIN) {
        if (!install_command(command, sequence, p, bytes))
            reply(command, sequence, BAD_COMMAND, 0, 0);
        return;
    }
    switch (command) {
    case CMD_IDENTIFY:
        if (bytes) { status = BAD_LENGTH; break; }
        memcpy(out, &patch_identity, sizeof(patch_identity));
        put_u32(out + 24, boot_mode);
        put_u32(out + 28, boot_trial_failed);
        count = 32 + extension_identity(out + 32);
        memcpy(out + count, patch_version, PATCH_VERSION_BYTES);
        count += PATCH_VERSION_BYTES;
        break;
    case CMD_DIAGNOSTICS: {
        if (bytes > 1) { status = BAD_LENGTH; break; }
        if (bytes && p[0] != 1) { status = BAD_RANGE; break; }
        if (bytes) diagnostics_reset_timing();
        struct counters snapshot;
        diagnostics_snapshot(&snapshot);
        count = sizeof(snapshot); memcpy(out, &snapshot, count);
        break;
    }
    case CMD_FOREGROUND:
        if (bytes) { status = BAD_LENGTH; break; }
        put_u32(out, foreground_storage_reads);
        put_u32(out + 4, BYTE(SAMPLE_SERVICE_STATE_ADDR));
        count = 8;
        break;
    case CMD_PEEK:
        if (bytes != 5) { status = BAD_LENGTH; break; }
        count = p[4];
        if (count > 96 || !ram_range(get_u32(p), count, 0)) {
            status = BAD_RANGE; count = 0; break;
        }
        memcpy(out, (const void *)get_u32(p), count);
        break;
    case CMD_POKE:
        if (bytes < 5) { status = BAD_LENGTH; break; }
        if (!ram_range(get_u32(p), bytes - 4, 1)) { status = BAD_RANGE; break; }
        if (install_busy()) { status = BUSY; break; }
        memcpy((void *)get_u32(p), p + 4, bytes - 4);
        MEMORY_BARRIER();
        break;
    case CMD_STREAM_RANGE:
        if (bytes != 6) { status = BAD_LENGTH; break; }
        if (p[0] >= 8 || p[5] > 96 || (p[5] && !ram_range(get_u32(p + 1), p[5], 0))) {
            status = BAD_RANGE; break;
        }
        if (stream_period) { status = BUSY; break; }
        ranges[p[0]].address = get_u32(p + 1); ranges[p[0]].bytes = p[5];
        break;
    case CMD_STREAM: {
        if (bytes != 2) { status = BAD_LENGTH; break; }
        u32 period = get_u16(p), wire_bytes = 160;
        for (u32 i = 0; i < 8; ++i)
            if (ranges[i].bytes) wire_bytes += 12 + ranges[i].bytes * 8 / 7;
        /* Leave at least half the DIN bandwidth for stock traffic/requests. */
        if (period && period < wire_bytes * 2 / 3 + 1) { status = BAD_RANGE; break; }
        stream_period = period; stream_due = REG32(TICKS) + period;
        break;
    }
    case CMD_DISPLAY:
        if (bytes != 1) { status = BAD_LENGTH; break; }
        if (!display_set_mode(p[0])) status = BAD_RANGE;
        break;
    case CMD_PANEL:
        /* u32 button mask, u16 hold ticks, s8 encoder steps; replies when done. */
        if (bytes != 7) { status = BAD_LENGTH; break; }
        status = panel_start(sequence, get_u32(p), get_u16(p + 4), (s32)(signed char)p[6]);
        if (status == OK) return;
        break;
    case CMD_SCREENSHOT:
        screenshot_command(sequence, p, bytes);
        return;
    default: status = BAD_COMMAND; break;
    }
    /* Command errors are reported in the reply; rx_sysex_rejects counts only
     * frames that fail framing checks. */
    reply(command, sequence, status, out, count);
}

void commands_panel_tick(void)
{
    if (!stream_period || (s32)(REG32(TICKS) - stream_due) < 0 || install_busy()) return;
    stream_due = REG32(TICKS) + stream_period;
    if (!sysex_tx_idle()) return;
    struct counters snapshot;
    diagnostics_snapshot(&snapshot);
    reply(EVENT_COUNTERS, 0, OK, &snapshot, sizeof(snapshot));
    for (u32 i = 0; i < 8; ++i) if (ranges[i].bytes) {
        u8 out[97]; out[0] = (u8)i;
        memcpy(out + 1, (const void *)ranges[i].address, ranges[i].bytes);
        reply(EVENT_RANGE, 0, OK, out, ranges[i].bytes + 1);
    }
}
