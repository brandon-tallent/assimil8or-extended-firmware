#include "patch.h"

/* Boot trace for trial images. The reset entry arms it only when the trial
 * handoff is present, so stable boots leave the last trial's record intact.
 * Each update is cleaned to DDR, which survives a watchdog reset, so the
 * next stable boot can peek how far a failed trial got. */
struct trace {
    u32 armed;
    u32 count;          /* stages logged, including any beyond the log */
    u32 last;           /* most recent stage */
    u32 render_calls;   /* heartbeats, sampled */
    u32 panel_polls;
    u32 foreground_polls;
    u8 build_id[8];     /* the trial that wrote this record */
    u8 log[TRACE_LOG_STAGES];
};
_Static_assert(sizeof(struct trace) == TRACE_LINE_BYTES, "trace is one cache line");
#define TRACE ((volatile struct trace *)TRACE_BASE)

static void trace_publish(void)
{
    /* Clean the line to the point of coherency; harmless with caches off. */
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 1\n\tdsb" :: "r"(TRACE_BASE) : "memory");
}

void trace_reset_entry(int chain_loaded)
{
    volatile struct trace *trace = TRACE;
    if (!chain_loaded) {
        trace->armed = 0;
        trace_publish();
        return;
    }
    trace->armed = 1;
    trace->count = 0;
    trace->last = 0;
    trace->render_calls = trace->panel_polls = trace->foreground_polls = 0;
    for (u32 i = 0; i < TRACE_LOG_STAGES; ++i) trace->log[i] = 0;
    for (u32 i = 0; i < sizeof(patch_identity.build_id); ++i)
        trace->build_id[i] = patch_identity.build_id[i];
    trace_stage(TRACE_STAGE_RESET);
}

void trace_stage(u32 stage)
{
    volatile struct trace *trace = TRACE;
    if (!trace->armed) return;
    if (trace->count < TRACE_LOG_STAGES) trace->log[trace->count] = (u8)stage;
    trace->count++;
    trace->last = stage;
    trace_publish();
}

void trace_heartbeat(u32 render_calls, u32 panel_polls, u32 foreground_polls)
{
    volatile struct trace *trace = TRACE;
    if (!trace->armed) return;
    trace->render_calls = render_calls;
    trace->panel_polls = panel_polls;
    trace->foreground_polls = foreground_polls;
    trace_publish();
}
