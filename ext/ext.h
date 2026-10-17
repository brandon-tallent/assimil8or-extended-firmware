/* Assimil8or extension image: stock model layout and entry points. The
 * offsets match PRESET_FORMAT.md. */
#ifndef EXT_H
#define EXT_H

#include "patch.h"
#include "eq.h"
#include "panel_page.h"

typedef unsigned long long u64;
typedef signed long long s64;

/* Working preset: WORKING_PRESET_STATE holds the library index in word 0
 * and the mutable model at +8. */
#define WORKING_PRESET_STATE      0x80114a5cu
#define PRESET_MODEL              (WORKING_PRESET_STATE + 8u)
#define PRESET_NAME_BYTES         12u
#define PRESET_CHANNELS_OFFSET    0x20u
#define PRESET_CHANNEL_BYTES      0x2c8u
#define PRESET_CHANNELS           8u
#define PRESET_RECORD_BYTES       (PRESET_CHANNELS_OFFSET + PRESET_CHANNELS * PRESET_CHANNEL_BYTES)
#define PRESET_LIBRARY_FIRST      1u
#define PRESET_LIBRARY_LAST       199u
#define ZONES                     8u
#define ZONE_BYTES                0x4cu
#define ZONE_NAME_BYTES           0x30u
#define ZONE_SAMPLE_OBJECT        0x30u
#define ZONE_SIDE                 0x34u
#define ZONE_MIN_VOLTAGE          0x36u
#define ZONE_SAMPLE_START         0x38u
#define ZONE_SAMPLE_END           0x3cu
#define ZONE_LOOP_START           0x40u
#define ZONE_LOOP_LENGTH          0x44u
#define ZONE_PITCH_OFFSET         0x48u
#define ZONE_LEVEL_OFFSET         0x4au
#define ZONE_POSITION_UNBOUNDED   0x7fffffffu
#define ZONE_LOOP_LENGTH_SHIFT    5u    /* 1/32-frame units */
#define ZONE_LOOP_LENGTH_MIN      128u
#define ZONE_BALANCE_5V           0u
#define CHANNEL_MODE              0x264u
#define CHANNEL_MODE_MASTER       0u
#define CHANNEL_MODE_STEREO_RIGHT 2u
#define CHANNEL_MODE_CYCLE        3u
#define CHANNEL_LOOP_MODE         0x266u
#define CHANNEL_LOOP_ON           1u
#define CHANNEL_MIX_MOD           0x26cu
#define MOD_AMOUNT                2u    /* after the source in an assignment */
#define MOD_SOURCE_MIN            (-4)
#define MOD_SOURCE_MAX            23
#define MOD_AMOUNT_LIMIT          100

/* Runtime sample objects (SAMPLE_OBJECT_*): 32-bit frames per channel
 * buffer after a zeroed leading guard. */
#define SAMPLE_PATH               0x30u /* after the 48-byte short name */
#define SAMPLE_PATH_BYTES         128u
#define SAMPLE_FRAMES             0xb0u
#define SAMPLE_RATE               0xb4u
#define SAMPLE_CHANNELS           0xbcu
#define SAMPLE_LOAD_STATE         0xc0u
#define SAMPLE_LOAD_READY         2u
#define SAMPLE_BUFFER_0           0xc4u
#define SAMPLE_BUFFER_1           0xc8u /* buffers 2 and 3 follow: 0 and 1 reversed */
#define SAMPLE_GUARD_FRAMES       128u
/* The pool of sample objects; a free slot's name is empty. */
#define SAMPLE_POOL_FIRST_OBJECT  (0x800dee00u + 0xd8u)
#define SAMPLE_POOL_OBJECT_BYTES  0xd4u
#define SAMPLE_POOL_OBJECTS       1000u

#define ACTIVE_FOLDER_NAME        0x8022e6d4u
#define ACTIVE_FOLDER_BYTES       64u
#define SAMPLE_RUNTIME_OBJECT     0x80237e30u
#define SAMPLE_LOAD_PROGRESS      0x80096ae8u /* SAMPLE_LOAD_PROGRESS_CALLBACK_OBJECT */
/* A channel's voice: the software active bits and the engine's group masks. */
#define ACTIVE_PRESET_RUNTIME_STATE 0x800dea88u
#define RUNTIME_CHANNEL_ACTIVE_MASK 0x36cu
#define ENGINE_CHANNEL_GROUP_MASKS  0x9ffff7b0u

/* Front-panel Mute/Solo, one byte per channel in the runtime; not saved. */
#define CHANNEL_MIX_STATES        (SAMPLE_RUNTIME_OBJECT + 0x370u)
#define CHANNEL_MIX_STATE_SOLO    2u
#define SAMPLE_SERVICE_RESULT_ADDR (0x800dba88u + 0x135u)

/* The zone-editor context fields the stock publisher reads. */
#define ZONE_EDITOR_SELECTED_CHANNEL 0x5cu
#define ZONE_EDITOR_SELECTED_ZONE    0x60u
#define ZONE_EDITOR_CONTEXT_WORDS    (ZONE_EDITOR_SELECTED_ZONE / 4u + 1u)

/* Zone-selection sync (a double press on the Zones page): while its index is
 * not negative, Note On plays the editor's zone for the channel, a byte per
 * channel, instead of the note's own. */
#define SAMPLE_SERVICE_RECORD            0x800dba88u
#define ZONE_EDITOR_SELECTION_SYNC_INDEX (SAMPLE_SERVICE_RECORD + 0xbcu)
#define ZONE_EDITOR_CHANNEL_ZONES        (SAMPLE_SERVICE_RECORD + 0x18b9u)

/* Chop scratch (ZONE_CHOP_ANALYSIS_STATE). */
#define ZONE_CHOP_ANALYSIS_STATE  0x8023b588u
#define ZONE_CHOP_BOUNDARIES      0x300u
#define ZONE_CHOP_MAX_PIECES      8u
#define ZONE_CHOP_MODE_BUFFER_0   0u
#define ZONE_CHOP_MODE_STEREO     2u

/* FatFs directory scan (DIR and FILINFO with a caller long-name buffer). */
#define DIR_BYTES                 0x40u
#define FILINFO_BYTES             0x20u
#define FILINFO_ATTRIBUTE         0x08u
#define FILINFO_SHORT_NAME        0x09u
#define FILINFO_LONG_NAME         0x18u
#define FILINFO_LONG_NAME_BYTES   0x1cu
#define LONG_NAME_BYTES           256u
#define ATTRIBUTE_DIRECTORY       0x10u
#define ATTRIBUTE_HIDDEN          0x02u

void preset_publish_selected_channel_and_linked_stereo(void *context);
void sample_runtime_rebuild_from_model_and_publish_channels(void *runtime, void *model);
void preset_sync_stereo_right_channel(void *model, u32 channel, u32 zone);
void channel_mix_rebuild_masks_and_publish(void *runtime);
void engine_channel_voice_flag_clear_grouped_and_notify(u32 channel);
void preset_channel_balance_zone_voltages(void *zones, u32 mode);
void preset_zone_initialize_empty(void *zone);
void sample_object_release_and_clear_identity(void *object);
u32 preset_load_missing_sample_data(void *model, void *progress, u32 channel, u32 leds, const char *prefix);
void sample_name_store_48(char *name, const char *source);
void sample_object_rebase_to_active_folder(void *object);
void zone_chop_detect_transients(void *state, void *sample, u32 mode, u32 pieces);
u32 preset_save_library_index_schedule_yaml_write(void *holder, u32 index, void *model);
u32 preset_working_model_differs_from_library(void *holder);
int resource_directory_open_for_scan(void *dir, const char *path);
int resource_directory_read_next_file_info(void *dir, void *info);
int resource_seek_absolute(FIL *file, u32 offset);
u32 strlen(const char *text);
int strcmp(const char *a, const char *b);
int tolower(int character);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, u32 bytes);

static inline u8 *channel_record(u32 channel)
{
    return (u8 *)PRESET_MODEL + PRESET_CHANNELS_OFFSET + channel * PRESET_CHANNEL_BYTES;
}
static inline u8 *zone_record(u32 channel, u32 zone) { return channel_record(channel) + zone * ZONE_BYTES; }
/* The zone's sample object once its audio is decoded, else null. */
static inline const u8 *ready_sample(u32 channel, u32 zone)
{
    const u8 *sample = (const u8 *)REG32((u32)zone_record(channel, zone) + ZONE_SAMPLE_OBJECT);
    return sample && BYTE((u32)sample + SAMPLE_LOAD_STATE) == SAMPLE_LOAD_READY ? sample : 0;
}
static inline void put_u16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

/* folder.c: the browsed folder (the active folder until another is
 * opened), its listing, and Save's copy of samples chosen elsewhere. */
#define PATH_BYTES                (4u + ACTIVE_FOLDER_BYTES + ZONE_NAME_BYTES)
#define BROWSE_PATH_BYTES         61u /* the stock loader's folder prefix holds "1:/" and 60 characters */
#define BROWSE_PREFIX_BYTES       (3u + ACTIVE_FOLDER_BYTES)
#define BROWSE_REQUEST            0xffffu /* in List folder's first index: an Open folder request */
#define BROWSE_NAME_MAX           (REQUEST_BYTES - 3u)
enum { BROWSE_STAY, BROWSE_HOME, BROWSE_ROOT, BROWSE_ENTER, BROWSE_UP, BROWSE_FIND };
#define FOLDER_ENTRY_IS_FOLDER    0xffu /* an entry's channel count */
#define FILE_CHUNK_BYTES          4096u
#define FILE_COLUMN_BYTES         (16u * FILE_CHUNK_BYTES) /* the most a file-peaks column reads */
#define FILE_FRAME_BYTES_MAX      8u /* stereo, 32-bit */
#define COPY_MORE                 0x100u
extern u8 file_chunk[FILE_CHUNK_BYTES]; /* peaks.c: scratch for whoever holds storage */
struct wav_info { u32 frames, rate, data_offset; u8 channels, bytes_per_sample; };
int wav_read_info(FIL *file, struct wav_info *info);
void folder_init(void);
void folder_changed(void);
u32 folder_list(u32 first, u32 limit, u8 *out, u32 room);
u32 folder_browse(const u8 *p, u32 bytes, u8 *out, u32 *count);
u32 folder_path(char *path, const u8 *name, u32 bytes);
u32 browse_path(char *path, const u8 *name, u32 bytes);
u32 browse_source(const u8 *name, u32 bytes, char *prefix, char *path);
u32 externals_copy_step(void);

/* editor.c: one zone's sample object record (14 bytes). */
u32 sample_info(const u8 *p, u8 *out, u32 *count);
u32 active_zones(u32 channel);
u32 zone_assign(u32 channel, u32 zone, const char *name, int take, const char *prefix, const char *path, u32 *frames);
u32 zone_remove(u32 channel, u32 zone);
/* editor.c: channel/zone EQ (src/eq.h). Record r is channel r / EQ_SLOTS,
 * slot r % EQ_SLOTS: zones 0-7, then EQ_SLOT_CHANNEL for the channel. */
#define EQ_SLOTS                  (ZONES + 1u)
#define EQ_SLOT_CHANNEL           ZONES
#define EQ_RECORDS                (PRESET_CHANNELS * EQ_SLOTS)
#define EQ_RECORD_BYTES           33u
void eq_record(u32 index, u8 *out);

/* grains.c: grain mode for one channel or stereo pair, playing the
 * settings of patch.h. grain_request is the panel interrupt's way to set a
 * field, or past GRAIN_REQUEST_ROUTE a route's source (even) or amount (odd).
 * grain_set takes a Set parameter's number past the first: a field's own in
 * wire order up to Scan, or one of those below (Slide, the routes and Skew
 * follow the resample parameters). */
#define GRAIN_REQUEST_ROUTE       32u
#define GRAIN_SET_STATUS          10u
#define GRAIN_SET_VIEW            11u
#define GRAIN_SET_SLIDE           17u
#define GRAIN_SET_ROUTE           18u /* a source and an amount for each route */
#define GRAIN_SET_ROUTES          (GRAIN_SET_ROUTE + 2u * GRAIN_ROUTES)
#define GRAIN_SET_SKEW            (GRAIN_SET_ROUTES + 1u)
/* A route's field, and the span of it that five volts sweep at +1.00; a
 * zero span is five octaves. */
struct grain_route { u8 field; u16 span; };
extern const struct grain_route grain_routes[GRAIN_ROUTES];
u32 grain_set(u32 channel, u32 field, s32 value, u8 *out, u32 *count);
u32 grain_crc(void);
void grain_request(u32 channel, u32 field, s32 value);
s32 grain_value(u32 channel, u32 field);
s32 grain_low(u32 field);
s32 grain_high(u32 field);
u32 grain_revision(void);
u32 grain_ratio_q16(s32 cents);
/* The audio interrupt's hook, installed while grain mode or an audition is on. */
void audio_hook_update(void);
/* The stock voice: its playhead's frame within the sample it plays, and its
 * envelope. */
#define PLAYHEAD_FRAME_BASE    0x9ffff9c4u /* ENGINE_CHANNEL_PLAYHEAD_FRAME_BASE */
#define PLAYHEAD_FRAME_STRIDE  8u
#define LEVEL_WORD_BASE        0x9ffffce0u /* ENGINE_CHANNEL_LEVEL_WORD_BASE: full at 7fffffffh */
#define LEVEL_WORD_STRIDE      4u
#define LEVEL_SILENT           0x8000      /* MIDI_DYNAMIC_VOICE_LEVEL_LIMIT: the stock test for a free voice */
/* audition.c: one pass of a zone for the sample browser. */
void audition_init(void);
u32 audition_arm(u32 channel, u32 zone);
void audition_disarm(u32 channel); /* PRESET_CHANNELS: whichever is armed */
int audition_armed(void);
void audition_frame(s32 *mix, const s32 *steps);
void audition_foreground(void);
/* editor.c: end the channel's voice. */
void channel_stop(u32 channel);
/* grain_page.c: the module's Grain page. */
extern const struct panel_page grain_page;
void grains_foreground(void);
void grains_init(void);

/* resample.c: a channel's output recorded into one of its zones. */
u32 resample_arm(u32 channel, u32 zone, u32 value, u32 sequence, u8 *out, u32 *count);
u32 resample_set(u32 channel, u32 zone, u32 op, s32 value, u8 *out, u32 *count);
void resample_button(u32 channel);
void resample_saved(void);
void resample_foreground(void);
void resample_init(void);

/* stream.c */
#define EDIT_STREAM               0x3fu
void stream_request(u32 sequence, const u8 *p, u32 bytes);
void stream_foreground(void);
void stream_init(void);

/* peaks.c */
u32 zone_peaks(const u8 *p, u8 *out, u32 *count);
u32 file_peaks(const u8 *p, u32 bytes, u8 *out, u32 *count);

#endif
