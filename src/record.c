#include "patch.h"

/* Remote timed recording through the stock Sampling Setup capture.
 *
 * The host sends the take's frame count with Arm: the stock Once limit ends
 * the capture at exactly that count, so a loop's length never depends on
 * MIDI arrival jitter. Stop ends a take early.
 *
 * Arm writes the live Sampling Setup record (Once mode, Threshold Off), has
 * the foreground allocate the capture object, sets the pre-trigger to the
 * host's measured MIDI latency, and primes the rolling input windows through
 * the stock SSetup hold callback. Start and Stop run the stock Sample-button
 * transaction in panel context, as a button press does. The pre-trigger keeps
 * the latency's worth of audio before Start arrives. The stock poller stops
 * the Once capture at the requested frame limit; an early Stop removes the
 * latency from the end, so it too ends where the host sent it.
 * The stock commit binds the zone and schedules the WAV write; the foreground
 * then checks the written file's header against the take.
 *
 * A resample records a channel's output in place of the Sample input. The
 * extension sets patch_take_source and patch_take_name, then arms: the take
 * is as long as asked or as memory allows, and carries that name. */

void sample_setup_toggle_transient_arm(void);
void sample_setup_capture_state_transaction(void);
u32 sample_setup_prepare_capture_object(void *record);
void sample_object_release_and_clear_identity(void *object);
void channel_mix_rebuild_masks_and_publish(void *runtime);
void sample_name_store_48(char *name, const char *source);

/* Sampling Setup edit/capture record and input runtime. */
#define SETUP                     0x8023b8c8u /* SAMPLE_SETUP_EDIT_RECORD */
#define SETUP_DESTINATION         0x00u
#define SETUP_MODE                0x01u
#define SETUP_TIME                0x02u /* s16 tenths of a second */
#define SETUP_RATE                0x04u
#define SETUP_THRESHOLD           0x06u
#define SETUP_SIDE                0x07u
#define SETUP_ZONE_DESTINATION    0x0bu
#define SETUP_CAPTURE_OBJECT      0x14u
#define SETUP_ENGINE_0            0x1cu /* six bank register addresses */
#define SETUP_FRAME_LIMIT         0x38u
#define SETUP_ARM_GATE            0x3cu
#define SETUP_SESSION_OBJECT      0x50u
#define SETUP_DESTINATION_MAX     9u
#define SETUP_ZONE_DESTINATION_MAX 1u
#define SETUP_MODE_ONCE           0u
#define SETUP_THRESHOLD_OFF       0u
#define SETUP_RATE_MAX            2u
#define SETUP_SIDE_MAX            3u
#define SETUP_TIME_MAX            6000u
#define ENGINE_SLOTS              6u
#define ENGINE_SLOT_COUNTER       3u /* 48 kHz ticks, including the pre-trigger */
#define ENGINE_IDLE_POINTER       0x9ffff2c0u
#define ENGINE_CONTROL_BIASED_BASE 0x9fffefffu /* SAMPLE_CAPTURE_CONTROL_BIASED_BASE */
#define ENGINE_CONTROL_0_BACK     0x11fu
#define ENGINE_CONTROL_FIRST      (ENGINE_CONTROL_BIASED_BASE - ENGINE_CONTROL_0_BACK)
#define ENGINE_CONTROLS           6u
#define INPUT_RUNTIME             0x802408c8u /* SAMPLE_INPUT_RUNTIME_STATE */
#define INPUT_PRETRIGGER_MS       0x400u
#define INPUT_MODE                0x404u
#define INPUT_MODE_IDLE           0u
#define INPUT_MODE_CAPTURE        2u
#define INPUT_MODE_WINDOWS        3u
#define INPUT_MODE_FINALIZING     4u
#define CAPTURE_WINDOW_WORDS      1920u
#define TICKS_PER_MS              48u
#define CHANNEL_RUNTIME           0x80237e30u /* SAMPLE_RUNTIME_OBJECT */
#define PREPARE_NO_FREE           1u
#define PREPARE_MEMORY_FULL       2u
#define OBJECT_FRAME_COUNT        0xb0u
#define OBJECT_SAMPLE_RATE        0xb4u
#define OBJECT_NAME_BYTES         0x30u
#define ACTIVE_FOLDER_NAME        0x8022e6d4u
#define FOLDER_NAME_MAX           60u
#define SERVICE_IDLE_SETTLE_TICKS 2000u /* a write that never showed as busy */
#define WAV_HEADER_READ_BYTES     256u

#define ARM_DESTINATION_MASK      0x0fu
#define ARM_ZONE_SHIFT            4u
#define ARM_SIDE_SHIFT            5u
#define ARM_RATE_MASK             0x03u
#define ARM_LATENCY_SHIFT         2u
#define CAPTURE_48KHZ_HZ          48000u
#define TENTHS_PER_SECOND         10u
#define TIME_MARGIN_TENTHS        20u

#define SETUP_BYTE(offset)        BYTE(SETUP + (offset))
#define SETUP_WORD(offset)        REG32(SETUP + (offset))

enum record_state {
    REC_IDLE, REC_PREPARING, REC_PRIMING, REC_ARMED, REC_CAPTURING, REC_WRITING, REC_DONE
};
/* Status flags. */
#define TAKE_STOPPED_BY_MODULE    0x01u /* frame count or front panel, not Stop */
#define TAKE_CANCELLED            0x02u
#define TAKE_DISARMED_BY_PANEL    0x04u

static volatile u32 state, job, job_sequence;
static u32 cancel_requested, request_frames, latency_ms, service_seen, writing_since, take_flags, take_result;
static u32 take_frames, take_rate;
static void *release_object;
static char take_name[OBJECT_NAME_BYTES];
static char take_path[4 + FOLDER_NAME_MAX + OBJECT_NAME_BYTES];
s32 patch_channel_out[AUDIO_CHANNELS + 1];
const s32 *volatile patch_resample_source;
u32 patch_take_source;
const char *patch_take_name;
static u32 source;
enum { JOB_NONE, JOB_PREPARE, JOB_RELEASE, JOB_VERIFY };

int record_busy(void) { return state != REC_IDLE && state != REC_DONE; }

static u32 rate_multiplier(void) { return 1u << SETUP_BYTE(SETUP_RATE); }
static volatile u32 *engine_slot(u32 slot) { return (volatile u32 *)SETUP_WORD(SETUP_ENGINE_0 + slot * 4u); }

/* The finalize path's engine reset, without the commit: slots 2..5 zero,
 * the buffer slots parked on the idle pointer. */
static void park_engine(int controls)
{
    if (!SETUP_WORD(SETUP_ENGINE_0)) return;
    for (u32 slot = 0; slot < ENGINE_SLOTS; ++slot)
        *engine_slot(slot) = slot < 2 ? ENGINE_IDLE_POINTER : 0;
    if (controls)
        for (u32 i = 0; i < ENGINE_CONTROLS; ++i) REG32(ENGINE_CONTROL_FIRST + i * 4u) = 0;
}

static void take_object(void)
{
    const u8 *object = (const u8 *)SETUP_WORD(SETUP_SESSION_OBJECT);
    take_frames = object ? REG32((u32)object + OBJECT_FRAME_COUNT) : 0;
    take_rate = object ? REG32((u32)object + OBJECT_SAMPLE_RATE) : 0;
    memset(take_name, 0, sizeof(take_name));
    if (object) memcpy(take_name, object, sizeof(take_name) - 1);
    service_seen = 0; writing_since = REG32(TICKS);
    state = REC_WRITING;
}

static u32 take_reply(u8 *out)
{
    put_u32(out, take_frames); put_u32(out + 4, take_rate);
    u32 length = 0;
    while (length < sizeof(take_name) - 1 && take_name[length]) ++length;
    memcpy(out + 8, take_name, length);
    return 8 + length;
}

/* Disown the capture object; the foreground frees it. */
static void drop_object(void)
{
    release_object = (void *)SETUP_WORD(SETUP_CAPTURE_OBJECT);
    SETUP_WORD(SETUP_CAPTURE_OBJECT) = 0;
    if (release_object) { MEMORY_BARRIER(); job = JOB_RELEASE; }
}

static u32 arm(u32 sequence, const u8 *p, u32 bytes)
{
    /* u8 destination | zone << 4 | side << 5, u8 rate | latency << 2, u32
     * frames: the frame fits one 16-byte message, which a sequencer can
     * place on a tick. */
    source = patch_take_source; patch_take_source = 0;
    if (bytes != 6) return BAD_LENGTH;
    u32 destination = p[0] & ARM_DESTINATION_MASK, zone = p[0] >> ARM_ZONE_SHIFT & 1u;
    u32 side = p[0] >> ARM_SIDE_SHIFT, rate = p[1] & ARM_RATE_MASK;
    u32 latency = p[1] >> ARM_LATENCY_SHIFT, frames = get_u32(p + 2);
    u32 rate_hz = CAPTURE_48KHZ_HZ << rate;
    if (destination > SETUP_DESTINATION_MAX || rate > SETUP_RATE_MAX || !frames ||
        frames > rate_hz / TENTHS_PER_SECOND * (SETUP_TIME_MAX - TIME_MARGIN_TENTHS) ||
        !latency || latency * TICKS_PER_MS << rate > CAPTURE_WINDOW_WORDS) return BAD_RANGE;
    /* Time only sizes the allocation; the take ends at exactly `frames`. */
    u32 time = frames / (rate_hz / TENTHS_PER_SECOND) + 1u + TIME_MARGIN_TENTHS;
    if (record_busy() || job) return BUSY;
    if (stock_busy() || BYTE(UI_ASYNC_OPERATION) || install_busy() || sample_transfer_busy())
        return BUSY;
    if (SETUP_BYTE(SETUP_ARM_GATE)) return BAD_STATE; /* armed from the front panel */
    SETUP_BYTE(SETUP_DESTINATION) = (u8)destination;
    SETUP_BYTE(SETUP_ZONE_DESTINATION) = (u8)zone;
    SETUP_BYTE(SETUP_RATE) = (u8)rate;
    SETUP_BYTE(SETUP_SIDE) = (u8)side;
    SETUP_BYTE(SETUP_MODE) = SETUP_MODE_ONCE;
    SETUP_BYTE(SETUP_THRESHOLD) = SETUP_THRESHOLD_OFF;
    *(volatile u16 *)(SETUP + SETUP_TIME) = (u16)time;
    latency_ms = latency; request_frames = frames; take_flags = take_result = cancel_requested = 0;
    take_frames = take_rate = 0; take_name[0] = 0;
    job_sequence = sequence; state = REC_PREPARING;
    MEMORY_BARRIER(); job = JOB_PREPARE;
    return OK;
}

/* Panel context, after the foreground allocated the object. */
static void prime(void)
{
    u32 status = OK;
    if (cancel_requested) {
        take_flags |= TAKE_CANCELLED; status = BAD_STATE; drop_object();
    } else if (BYTE(INPUT_RUNTIME + INPUT_MODE) == INPUT_MODE_CAPTURE || SETUP_BYTE(SETUP_ARM_GATE)) {
        status = BUSY; drop_object();
    } else {
        /* Restart the windows at this rate and length; a bank left by an
         * earlier arm at another rate would keep writing them. */
        park_engine(0);
        REG32(INPUT_RUNTIME + INPUT_PRETRIGGER_MS) = latency_ms;
        BYTE(INPUT_RUNTIME + INPUT_MODE) = INPUT_MODE_IDLE;
        sample_setup_toggle_transient_arm();
        if (!SETUP_BYTE(SETUP_ARM_GATE) || BYTE(INPUT_RUNTIME + INPUT_MODE) != INPUT_MODE_WINDOWS) {
            status = BAD_STATE; drop_object();
        }
    }
    state = status ? REC_IDLE : REC_ARMED;
    u8 out[4]; put_u32(out, SETUP_WORD(SETUP_FRAME_LIMIT));
    reply(CMD_RECORD_ARM, job_sequence, status, out, status ? 0 : 4);
}

static u32 start(void)
{
    if (state != REC_ARMED) return BAD_STATE;
    if (BYTE(INPUT_RUNTIME + INPUT_MODE) != INPUT_MODE_WINDOWS || !SETUP_BYTE(SETUP_ARM_GATE))
        return BAD_STATE;
    sample_setup_capture_state_transaction();
    if (BYTE(INPUT_RUNTIME + INPUT_MODE) != INPUT_MODE_CAPTURE) return BAD_STATE;
    state = REC_CAPTURING;
    return OK;
}

static u32 stop(u8 *out, u32 *count)
{
    if (state != REC_CAPTURING || BYTE(INPUT_RUNTIME + INPUT_MODE) != INPUT_MODE_CAPTURE)
        return BAD_STATE;
    /* Stop arrived one latency after it was sent: end the take there. */
    u32 trim = latency_ms * TICKS_PER_MS, irq = cpu_irq_save();
    volatile u32 *counter = engine_slot(ENGINE_SLOT_COUNTER);
    if (*counter > 2 * trim) *counter -= trim;
    cpu_irq_restore(irq);
    sample_setup_capture_state_transaction();
    take_object();
    *count = take_reply(out);
    return OK;
}

static u32 cancel(void)
{
    switch (state) {
    case REC_ARMED:
        if (SETUP_BYTE(SETUP_ARM_GATE)) sample_setup_toggle_transient_arm(); /* disarm */
        break;
    case REC_CAPTURING: {
        u32 irq = cpu_irq_save();
        park_engine(1);
        BYTE(INPUT_RUNTIME + INPUT_MODE) = INPUT_MODE_IDLE;
        cpu_irq_restore(irq);
        channel_mix_rebuild_masks_and_publish((void *)CHANNEL_RUNTIME);
        break;
    }
    case REC_PREPARING: case REC_PRIMING:
        cancel_requested = 1; /* the arm completes as refused */
        return OK;
    case REC_IDLE: case REC_DONE: return OK;
    default: return BUSY;
    }
    drop_object();
    take_flags |= TAKE_CANCELLED;
    state = REC_IDLE;
    return OK;
}

void record_command(u32 command, u32 sequence, const u8 *p, u32 bytes)
{
    u8 out[REPLY_BYTES - 1]; u32 status, count = 0;
    if (command != CMD_RECORD_ARM && bytes) status = BAD_LENGTH;
    else switch (command) {
    case CMD_RECORD_ARM:
        status = arm(sequence, p, bytes);
        if (status == OK) return; /* replies once primed */
        break;
    case CMD_RECORD_START: status = start(); break;
    case CMD_RECORD_STOP: status = stop(out, &count); break;
    case CMD_RECORD_CANCEL: status = cancel(); break;
    case CMD_RECORD_STATUS: {
        u32 mode = BYTE(INPUT_RUNTIME + INPUT_MODE), elapsed = 0;
        if (mode == INPUT_MODE_CAPTURE && SETUP_WORD(SETUP_ENGINE_0))
            elapsed = *engine_slot(ENGINE_SLOT_COUNTER) * rate_multiplier();
        out[0] = (u8)state; out[1] = (u8)mode; out[2] = SETUP_BYTE(SETUP_ARM_GATE);
        out[3] = BYTE(SAMPLE_SERVICE_STATE_ADDR);
        out[4] = (u8)take_flags; out[5] = (u8)take_result; out[6] = (u8)latency_ms; out[7] = 0;
        put_u32(out + 8, elapsed);
        put_u32(out + 12, SETUP_WORD(SETUP_FRAME_LIMIT));
        count = 16 + take_reply(out + 16);
        status = OK;
        break;
    }
    default: status = BAD_COMMAND; break;
    }
    reply(command, sequence, status, out, count);
}

void record_panel_tick(void)
{
    u32 mode = BYTE(INPUT_RUNTIME + INPUT_MODE);
    patch_resample_source = source && (state == REC_ARMED || state == REC_CAPTURING) ?
        &patch_channel_out[source - 1] : 0;
    switch (state) {
    case REC_PRIMING: prime(); break;
    case REC_ARMED:
        /* The front panel may start (Sample) or disarm (SSetup hold). */
        if (mode == INPUT_MODE_CAPTURE) state = REC_CAPTURING;
        else if (!SETUP_BYTE(SETUP_ARM_GATE)) { take_flags |= TAKE_DISARMED_BY_PANEL; state = REC_IDLE; }
        break;
    case REC_CAPTURING:
        if (mode != INPUT_MODE_CAPTURE && mode != INPUT_MODE_FINALIZING) {
            take_flags |= TAKE_STOPPED_BY_MODULE; take_object();
        }
        break;
    case REC_WRITING:
        if (BYTE(SAMPLE_SERVICE_STATE_ADDR)) { service_seen = 1; break; }
        if ((service_seen || (u32)(REG32(TICKS) - writing_since) > SERVICE_IDLE_SETTLE_TICKS) && !job) {
            MEMORY_BARRIER(); job = JOB_VERIFY;
        }
        break;
    }
}

static int build_path(void)
{
    const char *folder = (const char *)ACTIVE_FOLDER_NAME;
    u32 length = 0, name = 0;
    while (length < FOLDER_NAME_MAX && folder[length]) ++length;
    while (name < sizeof(take_name) && take_name[name]) ++name;
    if (!length || length == FOLDER_NAME_MAX || !name) return 0;
    memcpy(take_path, "1:/", 3); memcpy(take_path + 3, folder, length);
    take_path[3 + length] = '/'; memcpy(take_path + 4 + length, take_name, name);
    take_path[4 + length + name] = 0;
    return 1;
}

/* The WAV on the card holds the take's frame count. */
static u32 verify_take(void)
{
    u8 header[WAV_HEADER_READ_BYTES]; FIL file; u32 done = 0;
    if (!build_path()) return BAD_STATE;
    if (fatfs_open(&file, take_path, FA_READ)) return IO_ERROR;
    u32 size = file.words[FIL_FILE_SIZE / 4];
    int failed = resource_read_bytes(&file, header, sizeof(header), &done);
    resource_close(&file);
    if (failed || done < 12 || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4))
        return BAD_IMAGE;
    u32 align = 0;
    for (u32 at = 12; at + 8 <= done;) {
        u32 chunk = get_u32(header + at + 4);
        if (!memcmp(header + at, "fmt ", 4) && at + 22 <= done) align = get_u16(header + at + 20);
        if (!memcmp(header + at, "data", 4)) {
            if (!align || chunk % align || chunk / align != take_frames ||
                at + 8 + chunk > size) return BAD_IMAGE;
            return OK;
        }
        if (chunk > size) break;
        at += 8 + chunk + (chunk & 1u);
    }
    return BAD_IMAGE;
}

/* Each job is cleared before its state is published: the panel may queue
 * the next job as soon as it sees that state. */
void record_foreground_poll(void)
{
    u32 current = job, next;
    switch (current) {
    case JOB_PREPARE: {
        u32 result = sample_setup_prepare_capture_object((void *)SETUP);
        int failed = result || !SETUP_WORD(SETUP_CAPTURE_OBJECT);
        if (!failed && source) {
            if (SETUP_WORD(SETUP_FRAME_LIMIT) < request_frames) request_frames = SETUP_WORD(SETUP_FRAME_LIMIT);
            sample_name_store_48((char *)SETUP_WORD(SETUP_CAPTURE_OBJECT), patch_take_name);
        }
        if (!failed && SETUP_WORD(SETUP_FRAME_LIMIT) < request_frames) {
            /* Capacity fell short of the requested take. */
            void *object = (void *)SETUP_WORD(SETUP_CAPTURE_OBJECT);
            SETUP_WORD(SETUP_CAPTURE_OBJECT) = 0;
            sample_object_release_and_clear_identity(object);
            result = PREPARE_MEMORY_FULL; failed = 1;
        }
        /* The stock Once poll stops the capture at this frame count and the
         * commit keeps exactly it, independent of when Stop arrives. */
        if (!failed) SETUP_WORD(SETUP_FRAME_LIMIT) = request_frames;
        next = failed ? REC_IDLE : REC_PRIMING;
        job = JOB_NONE; MEMORY_BARRIER(); state = next;
        if (failed)
            reply(CMD_RECORD_ARM, job_sequence, result == PREPARE_NO_FREE ? BUSY : NO_MEMORY, 0, 0);
        return;
    }
    case JOB_RELEASE: {
        void *object = release_object; release_object = 0;
        if (object) sample_object_release_and_clear_identity(object);
        break;
    }
    case JOB_VERIFY:
        take_result = verify_take();
        counters.last_job_result = take_result;
        status_show(take_result ? "Take not saved" : "Take saved");
        job = JOB_NONE; MEMORY_BARRIER(); state = REC_DONE;
        return;
    default: return;
    }
    MEMORY_BARRIER(); job = JOB_NONE;
}
