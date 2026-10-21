#include "patch.h"
#include "eq.h"

void *memcpy(void *to, const void *from, size_t bytes)
{
    u8 *d = to; const u8 *s = from;
    while (bytes--) *d++ = *s++;
    return to;
}
void *memset(void *to, int value, size_t bytes)
{
    u8 *d = to;
    while (bytes--) *d++ = (u8)value;
    return to;
}
int memcmp(const void *a, const void *b, size_t bytes)
{
    const u8 *x = a, *y = b;
    while (bytes--) { if (*x != *y) return *x - *y; ++x; ++y; }
    return 0;
}
u32 crc32_update(u32 crc, const void *data, u32 bytes)
{
    const u8 *p = data;
    while (bytes--) {
        crc ^= *p++;
        for (u32 bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (CRC32_POLYNOMIAL & (0u - (crc & 1u)));
    }
    return crc;
}

const struct patch_identity patch_identity
    __attribute__((section(".identity"), used)) = {
        {PATCH_MAGIC_0, PATCH_MAGIC_1}, PATCH_PROTOCOL_VERSION, {0}, 0
    };
#define VERSION_STRING(x) #x
#define VERSION_NUMBER(x) VERSION_STRING(x)
const u8 patch_version[PATCH_VERSION_BYTES] = {
    PATCH_VERSION_MAJOR, PATCH_VERSION_MINOR, PATCH_VERSION_PATCH, PATCH_VERSION_DEV ? PATCH_VERSION_FLAG_DEV : 0u
};
/* The About screen's version line (prefs.c). The development flag is only
 * in Identify. */
const char patch_version_text[] = "Tallent Edition Rev v" VERSION_NUMBER(PATCH_VERSION_MAJOR) "." VERSION_NUMBER(PATCH_VERSION_MINOR)
    "." VERSION_NUMBER(PATCH_VERSION_PATCH);
struct counters counters;
u32 boot_mode, boot_trial_failed;
u32 foreground_storage_reads;
static u32 last_render_start, render_seen;

void patch_startup(void)
{
    /* Runs after stock constructors, before UART can be polled. Patch BSS is
     * file-backed zero storage in the reclaimed region, not stock BSS. */
    trace_stage(TRACE_STAGE_PATCH_STARTUP);
    u32 irq = cpu_irq_save();
    memset(&counters, 0, sizeof(counters));
    render_seen = 0;
    /* A system status drops data bytes that precede the first real status. */
    BYTE(MIDI_RX + MIDI_RX_MESSAGE_BYTES) = MIDI_SYSTEM_STATUS;
    counters.reset_status = REG32(PRM_DEVICE_BASE + PRM_RSTST);
    counters.render_cycles_min = counters.render_interval_min = ~0u;
    cpu_cycles_enable();
    cpu_irq_restore(irq);
    uart0_module_clock_and_bootstrap();
    boot_check_trial();
    if (boot_mode == BOOT_TRIAL) trace_stage(TRACE_STAGE_TRIAL_CONFIRMED);
}

void patch_render_stats(u32 start, u32 end)
{
    u32 cycles = end - start;
    counters.render_calls++;
    counters.render_cycles_last = cycles;
    if (cycles < counters.render_cycles_min) counters.render_cycles_min = cycles;
    if (cycles > counters.render_cycles_max) counters.render_cycles_max = cycles;
    if (render_seen) {
        u32 interval = start - last_render_start;
        counters.render_interval_last = interval;
        if (interval < counters.render_interval_min) counters.render_interval_min = interval;
        if (interval > counters.render_interval_max) counters.render_interval_max = interval;
    }
    if (cycles >= AUDIO_FRAME_BUDGET_CYCLES) counters.render_over_budget++;
    if (!(counters.render_calls & TRACE_HEARTBEAT_MASK))
        trace_heartbeat(counters.render_calls, counters.panel_polls, counters.foreground_polls);
    u32 bin = cycles >> 11;
    counters.render_load[bin < RENDER_LOAD_BINS ? bin : RENDER_LOAD_BINS - 1]++;
    last_render_start = start;
    render_seen = 1;
}

void diagnostics_reset_timing(void)
{
    u32 irq = cpu_irq_save();
    counters.render_cycles_last = counters.render_cycles_max = 0;
    counters.render_interval_last = counters.render_interval_max = 0;
    counters.render_cycles_min = counters.render_interval_min = ~0u;
    counters.render_over_budget = 0;
    memset(counters.render_load, 0, sizeof(counters.render_load));
    last_render_start = render_seen = 0;
    cpu_irq_restore(irq);
}

void diagnostics_snapshot(struct counters *out)
{
    u32 irq = cpu_irq_save();
    counters.uptime_ticks = REG32(TICKS);
    counters.heap_break = REG32(HEAP_BREAK_POINTER);
    memcpy(out, &counters, sizeof(*out));
    cpu_irq_restore(irq);
}

int stock_busy(void)
{
    u8 mode = BYTE(SAMPLE_INPUT_RUNTIME_MODE_ADDRESS);
    return BYTE(POWERDOWN_LOW) || BYTE(SAMPLE_SERVICE_STATE_ADDR) ||
        mode == SAMPLE_INPUT_MODE_CAPTURE_ACTIVE || mode == 4; /* finalizing */
}
int storage_claim(void)
{
    u32 irq = cpu_irq_save();
    int available = !stock_busy() && !BYTE(UI_ASYNC_OPERATION) &&
        (s32)REG32(UI_ASYNC_OPERATION + UI_ASYNC_PENDING_LEVEL) <= 1;
    if (available) {
        REG32(UI_ASYNC_OPERATION + UI_ASYNC_PENDING_LEVEL) = 0;
        REG32(UI_ASYNC_OPERATION + UI_ASYNC_OWNER_LEVEL) = 1;
        BYTE(UI_ASYNC_OPERATION) = 1;
    }
    cpu_irq_restore(irq);
    return available;
}
void storage_release(void)
{
    u32 irq = cpu_irq_save();
    REG32(UI_ASYNC_OPERATION + UI_ASYNC_OWNER_LEVEL) = 0;
    BYTE(UI_ASYNC_OPERATION) = 0;
    cpu_irq_restore(irq);
}

void patch_foreground_pass(void)
{
    if (!counters.foreground_polls) trace_stage(TRACE_STAGE_FIRST_FOREGROUND_PASS);
    install_foreground_poll();
    sample_transfer_foreground_poll();
    record_foreground_poll();
    screenshot_foreground_poll();
    preset_extras_foreground();
    eq_foreground_poll();
    filter_foreground_poll();
    extension_foreground_poll();
    counters.foreground_polls++;
    if (boot_mode == BOOT_TRIAL) watchdog_kick();
}

void patch_storage_progress(u32 result)
{
    /* Never feed from panel/display interrupts, failed reads, or early boot.
     * A read that blocks does not get here and still times out in 20 seconds. */
    if (result || !BYTE(TASK_EXEC_STARTED) ||
        REG32(TASK_EXEC_CONTEXT) != TASK_EXEC_FOREGROUND) return;
    foreground_storage_reads++;
    if (boot_mode == BOOT_TRIAL) watchdog_kick();
}

int ram_range(u32 address, u32 bytes, int writing)
{
    /* Only mapped DDR/OCMC, with no peripheral aliases or wrap. Poke excludes
     * the application, patch, stacks, handoff and MMU tables. */
    if (!bytes || address + bytes < address) return 0;
    if (!writing)
        return (address >= DDR_BASE && address + bytes <= DDR_END) ||
            (address >= OCMC_BASE && address + bytes <= OCMC_END);
    return (address >= STOCK_BSS_START && address + bytes <= MMU_L1_TABLE_BASE) ||
        (address >= HEAP_ALLOCATION_START && address + bytes <= HEAP_ALLOCATION_END_LIMIT);
}
