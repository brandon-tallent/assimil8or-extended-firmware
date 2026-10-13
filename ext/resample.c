#include "ext.h"

/* Resampling: a channel's output, or a channel's and its Stereo/Right
 * partner's, recorded into a zone of that channel.
 *
 * The overlay's recorder does the take (src/record.c): told the source and
 * the take's name, the stock capture records the channel in place of the
 * Sample input, unassigned, and writes its WAV. When the recorder is done the
 * foreground here puts the take in its zone: the one asked for, or the next
 * free one. With no zone free the take stays on the card until Place names
 * the zone it replaces.
 *
 * Sample on the module's Grain page starts a take of the selected channel
 * that runs until Sample is pressed again or memory is full.
 *
 * A take is <preset>-<channel>-<NN>.wav, the lowest number neither on the
 * card nor loaded. A take that leaves its zone is retired: at the next save
 * of the same preset its file is deleted and its memory freed, unless a zone
 * uses it again. Until that save the preset on the card still names it. */

int sprintf(char *buffer, const char *format, ...);

enum { OP_ARM, OP_STATUS, OP_PLACE, OP_DELETE, OP_LENGTH };
enum { TAKE_NONE, TAKE_RECORDING, TAKE_PLACED, TAKE_ON_CARD, TAKE_FAILED };
#define ZONE_NEXT             0xffu
#define ARM_FRAMES_MASK       0x03ffffffu
#define ARM_LATENCY_SHIFT     26u
#define ARM_FRAMES_MAX        (48000u / 10u * (6000u - 20u)) /* the recorder's longest take */
#define RECORD_SIDE_STEREO    2u
#define RECORD_SIDE_SHIFT     5u
#define RECORD_LATENCY_SHIFT  2u
#define TAKE_NUMBERS          99u
#define TAKE_SUFFIX_BYTES     9u   /* "-C-NN.wav" */
#define RETIRED               16u
#define MESSAGE_BYTES         20u
/* The operator's Sampling Setup (SAMPLE_SETUP_EDIT_RECORD): the recorder
 * writes its take into it, and its monitor would add the Sample input to the
 * mix while a take runs. It is put back when the take ends. The stock
 * service saves the setup whenever it differs from the saved copy
 * (SAMPLE_SETUP_SAVED_RECORD), so the copy follows it through the take. */
#define SETUP                 0x8023b8c8u
#define SETUP_SAVED           0x8023b8b8u
#define SETUP_BYTES           14u
#define SETUP_MONITOR_L       0x0cu
#define SETUP_MONITOR_R       0x0du
#define SETUP_ARM_GATE        0x3cu
#define INPUT_MODE            (0x802408c8u + 0x404u) /* SAMPLE_INPUT_RUNTIME_STATE's mode byte */
#define INPUT_MODE_WINDOWS    3u
#define INPUT_MODE_CAPTURE    2u
#define SETUP_FRAME_LIMIT     0x38u

static struct {
    u32 state, channel, zone, frames;
    char name[ZONE_NAME_BYTES];
} take;
static char retired[RETIRED][ZONE_NAME_BYTES];
static u8 setup[SETUP_BYTES], setup_saved[SETUP_BYTES];
/* The panel's Sample press, for the foreground: a channel + 1 to record, and
 * whether its take starts as soon as it is armed. */
static volatile u32 button_channel, start_armed;
static u32 retired_preset, sweep;

static int digit(char c) { return c >= '0' && c <= '9'; }

/* A name this file made: <preset>-<channel>-<NN>.wav. */
static int is_take(const char *name)
{
    u32 length = strlen(name);
    if (length <= TAKE_SUFFIX_BYTES) return 0;
    const char *s = name + length - TAKE_SUFFIX_BYTES;
    return s[0] == '-' && digit(s[1]) && s[2] == '-' && digit(s[3]) && digit(s[4]) && !memcmp(s + 5, ".wav", 4);
}

static u8 *pool_object(const char *name)
{
    for (u32 i = 0; i < SAMPLE_POOL_OBJECTS; ++i) {
        u8 *object = (u8 *)SAMPLE_POOL_FIRST_OBJECT + i * SAMPLE_POOL_OBJECT_BYTES;
        if (object[0] && !strncasecmp((const char *)object, name, ZONE_NAME_BYTES)) return object;
    }
    return 0;
}

static int on_card(const char *name)
{
    char path[PATH_BYTES]; FIL file;
    if (!folder_path(path, (const u8 *)name, strlen(name)) || fatfs_open(&file, path, FA_READ)) return 0;
    resource_close(&file);
    return 1;
}

static int zone_uses(const char *name)
{
    for (u32 channel = 0; channel < PRESET_CHANNELS; ++channel)
        for (u32 zone = 0; zone < ZONES; ++zone)
            if (!strncasecmp((const char *)zone_record(channel, zone), name, ZONE_NAME_BYTES)) return 1;
    return 0;
}

/* The next take's name for a channel. */
static u32 name_take(u32 channel)
{
    const char *preset = (const char *)PRESET_MODEL;
    u32 length = 0;
    for (u32 i = 0; i < PRESET_NAME_BYTES && preset[i]; ++i) {
        char c = preset[i];
        int plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || digit(c) || c == ' ' || c == '_' || c == '-';
        take.name[length++] = plain ? c : '_';
    }
    while (length && take.name[length - 1] == ' ') --length;
    if (!length) { memcpy(take.name, "Take", 4); length = 4; }
    char *s = take.name + length;
    memcpy(s, "-0-00.wav", TAKE_SUFFIX_BYTES + 1);
    s[1] = (char)('1' + channel);
    for (u32 n = 1; n <= TAKE_NUMBERS; ++n) {
        s[3] = (char)('0' + n / 10); s[4] = (char)('0' + n % 10);
        if (!pool_object(take.name) && !on_card(take.name)) return OK;
    }
    return FILE_EXISTS;
}

static void retire(const char *name)
{
    if (!is_take(name)) return;
    u32 preset = REG32(WORKING_PRESET_STATE), free_slot = 0;
    if (preset != retired_preset) { memset(retired, 0, sizeof(retired)); retired_preset = preset; }
    for (u32 i = 0; i < RETIRED; ++i) {
        if (!strncasecmp(retired[i], name, ZONE_NAME_BYTES)) return;
        if (!retired[i][0]) free_slot = i;
    }
    sample_name_store_48(retired[free_slot], name);
}

/* After a save of the preset the retired takes left: delete each one no zone
 * uses, and free its memory. */
static void sweep_retired(void)
{
    for (u32 i = 0; i < RETIRED; ++i) {
        char *name = retired[i];
        if (!name[0]) continue;
        if (retired_preset == REG32(WORKING_PRESET_STATE) && !zone_uses(name)) {
            char path[PATH_BYTES];
            if (folder_path(path, (const u8 *)name, strlen(name))) { fatfs_unlink(path); folder_changed(); }
            u8 *object = pool_object(name);
            if (object) sample_object_release_and_clear_identity(object);
        }
        name[0] = 0;
    }
}

/* Put the take in a zone, retiring a take that was there. */
static u32 place(u32 zone)
{
    char old[ZONE_NAME_BYTES];
    sample_name_store_48(old, (const char *)zone_record(take.channel, zone));
    u32 status = zone_assign(take.channel, zone, take.name, 1, 0, 0, &take.frames);
    if (status != OK) return status;
    if (old[0] && zone < active_zones(take.channel) && strncasecmp(old, take.name, ZONE_NAME_BYTES)) retire(old);
    take.zone = zone; take.state = TAKE_PLACED;
    folder_changed();
    return OK;
}

/* Arm a take of the channel: value is the frame count (0: as long as memory
 * allows) and, above ARM_LATENCY_SHIFT, the host's MIDI latency in ms. The
 * recorder replies as for Record arm once it is primed. Replies the name. */
u32 resample_arm(u32 channel, u32 zone, u32 value, u32 sequence, u8 *out, u32 *count)
{
    if (channel >= PRESET_CHANNELS || (zone >= ZONES && zone != ZONE_NEXT)) return BAD_RANGE;
    if (channel_record(channel)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT || !active_zones(channel)) return BAD_STATE;
    if (record_busy() || take.state == TAKE_RECORDING) return BUSY;
    if (!storage_claim()) return BUSY;
    if (take.state == TAKE_ON_CARD) retire(take.name); /* never placed */
    u32 status = name_take(channel);
    storage_release();
    if (status != OK) return status;
    u32 pair = channel + 1 < PRESET_CHANNELS && channel_record(channel + 1)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT;
    u32 frames = value & ARM_FRAMES_MASK, latency = value >> ARM_LATENCY_SHIFT;
    if (!frames || frames > ARM_FRAMES_MAX) frames = ARM_FRAMES_MAX;
    u8 request[6] = {(u8)((pair ? RECORD_SIDE_STEREO : 0) << RECORD_SIDE_SHIFT),
                     (u8)((latency ? latency : 1) << RECORD_LATENCY_SHIFT)};
    put_u32(request + 2, frames);
    /* The recorder runs in the panel interrupt; arm it as one step there. */
    u32 irq = cpu_irq_save();
    memcpy(setup, (const void *)SETUP, SETUP_BYTES);
    memcpy(setup_saved, (const void *)SETUP_SAVED, SETUP_BYTES);
    patch_take_name = take.name; patch_take_source = channel + 1;
    record_command(CMD_RECORD_ARM, sequence, request, sizeof(request));
    int armed = record_busy();
    if (armed) {
        BYTE(SETUP + SETUP_MONITOR_L) = BYTE(SETUP + SETUP_MONITOR_R) = 0;
        memcpy((void *)SETUP_SAVED, (const void *)SETUP, SETUP_BYTES);
    } else memcpy((void *)SETUP, setup, SETUP_BYTES);
    cpu_irq_restore(irq);
    if (!armed) return BAD_STATE;
    take.channel = channel; take.zone = zone; take.frames = 0; take.state = TAKE_RECORDING;
    *count = strlen(take.name);
    memcpy(out, take.name, *count);
    return OK;
}

/* Status replies u8 state, channel, zone, retired takes; u32 frames; the
 * take's name. Place puts a take that is on the card in a zone. Delete
 * removes a zone that holds a take and retires the take. Length ends the
 * running take at a frame count: the stock capture stops itself there, so
 * the take's length does not depend on when a Stop would arrive. */
u32 resample_set(u32 channel, u32 zone, u32 op, s32 value, u8 *out, u32 *count)
{
    if (op == OP_STATUS) {
        u32 waiting = 0, length = strlen(take.name);
        for (u32 i = 0; i < RETIRED; ++i) waiting += retired[i][0] != 0;
        out[0] = (u8)take.state; out[1] = (u8)take.channel; out[2] = (u8)take.zone; out[3] = (u8)waiting;
        put_u32(out + 4, take.frames);
        memcpy(out + 8, take.name, length);
        *count = 8 + length;
        return OK;
    }
    if (op == OP_LENGTH) {
        if (take.state != TAKE_RECORDING || BYTE(INPUT_MODE) != INPUT_MODE_CAPTURE || value <= 0 ||
            (u32)value > REG32(SETUP + SETUP_FRAME_LIMIT)) return BAD_STATE;
        REG32(SETUP + SETUP_FRAME_LIMIT) = (u32)value;
        put_u32(out, (u32)value); *count = 4;
        return OK;
    }
    if (channel >= PRESET_CHANNELS || zone >= ZONES) return BAD_RANGE;
    if (op == OP_PLACE) {
        if (take.state != TAKE_ON_CARD || channel != take.channel) return BAD_STATE;
        u32 status = place(zone);
        if (status == OK) { put_u32(out, take.frames); *count = 4; }
        return status;
    }
    if (op == OP_DELETE) {
        char name[ZONE_NAME_BYTES];
        sample_name_store_48(name, (const char *)zone_record(channel, zone));
        if (zone >= active_zones(channel) || !is_take(name)) return BAD_STATE;
        u32 status = zone_remove(channel, zone);
        if (status == OK) retire(name);
        return status;
    }
    return BAD_RANGE;
}

/* Panel interrupt. A second press ends the take, or drops one not started. */
void resample_button(u32 channel)
{
    if (!record_busy()) { button_channel = channel + 1; return; }
    start_armed = 0;
    record_command(CMD_RECORD_STOP, 0, 0, 0);
    record_command(CMD_RECORD_CANCEL, 0, 0, 0);
}

static void button_foreground(void)
{
    u8 out[ZONE_NAME_BYTES]; u32 count, channel = button_channel;
    if (channel) {
        u32 status = resample_arm(channel - 1, ZONE_NEXT, 0, 0, out, &count);
        if (status == BUSY && !record_busy()) return; /* storage held: next pass */
        button_channel = 0;
        start_armed = status == OK;
        status_show(status == OK ? "Resampling" : "No take");
    }
    if (!start_armed) return;
    if (!record_busy()) { start_armed = 0; return; }
    if (BYTE(INPUT_MODE) != INPUT_MODE_WINDOWS || !BYTE(SETUP + SETUP_ARM_GATE)) return;
    start_armed = 0;
    u32 irq = cpu_irq_save();
    record_command(CMD_RECORD_START, 0, 0, 0);
    cpu_irq_restore(irq);
}

void resample_saved(void) { sweep = 1; }

void resample_foreground(void)
{
    if (sweep && storage_claim()) { sweep = 0; sweep_retired(); storage_release(); }
    button_foreground();
    if (take.state != TAKE_RECORDING || record_busy() || !storage_claim()) return;
    memcpy((void *)SETUP, setup, SETUP_BYTES);
    memcpy((void *)SETUP_SAVED, setup_saved, SETUP_BYTES);
    channel_mix_rebuild_masks_and_publish((void *)SAMPLE_RUNTIME_OBJECT);
    /* A cancelled take wrote no file. */
    if (!on_card(take.name)) take.state = TAKE_FAILED;
    else {
        u32 zone = take.zone == ZONE_NEXT ? active_zones(take.channel) : take.zone;
        if (zone >= ZONES) take.state = TAKE_ON_CARD;
        else if (place(zone) != OK) take.state = TAKE_FAILED;
    }
    storage_release();
    static char placed[MESSAGE_BYTES];
    sprintf(placed, "Take in zone %d", (int)take.zone + 1);
    status_show(take.state == TAKE_PLACED ? placed : take.state == TAKE_ON_CARD ? "Zones full: take on card" : "No take");
}

void resample_init(void)
{
    memset(&take, 0, sizeof(take));
    memset(retired, 0, sizeof(retired));
    sweep = 0; button_channel = start_armed = 0;
}
