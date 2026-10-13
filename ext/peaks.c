#include "ext.h"

/* Waveform overviews: per column, the minimum and maximum across all sides
 * as signed 8-bit fractions of full scale and the RMS as unsigned 8-bit.
 * Zone peaks read the loaded sample in RAM; file peaks read a WAV in the
 * browsed folder for previews. A column longer than FILE_COLUMN_BYTES is
 * read as that much in pieces spread evenly over it, so a long file's
 * preview costs no more than a short one's. */

#define PEAK_COLUMNS_MAX 40u

struct column { s32 low, high; u64 energy; u32 count; };

static void add(struct column *c, s32 v)
{
    s32 h = v >> 16;
    if (v < c->low) c->low = v;
    if (v > c->high) c->high = v;
    c->energy += (u64)(h * h);
    c->count++;
}

static void store(u8 *out, const struct column *c)
{
    u32 rms = 0;
    if (c->count) {
        u32 mean = (u32)(c->energy / c->count), root = 0;
        for (u32 bit = 1u << 30; bit; bit >>= 2) {
            if (mean >= root + bit) { mean -= root + bit; root = (root >> 1) + bit; }
            else root >>= 1;
        }
        rms = root >> 7; /* 15-bit magnitude to 8 bits */
        if (rms > 255) rms = 255;
    }
    out[0] = (u8)(c->low >> 24); out[1] = (u8)(c->high >> 24); out[2] = (u8)rms;
}

static void bounds(u32 first, u32 span, u32 index, u32 frames, u32 *a, u32 *b)
{
    *a = first + index * span; *b = *a + span;
    if (*a > frames) *a = frames;
    if (*b > frames) *b = frames;
}

u32 zone_peaks(const u8 *p, u8 *out, u32 *count)
{
    u32 channel = p[0], zone = p[1], first = get_u32(p + 2), span = get_u32(p + 6), columns = p[10];
    if (channel >= PRESET_CHANNELS || zone >= ZONES || !span || !columns || columns > PEAK_COLUMNS_MAX)
        return BAD_RANGE;
    const u8 *sample = ready_sample(channel, zone);
    if (!sample) return BAD_STATE;
    u32 frames = REG32((u32)sample + SAMPLE_FRAMES), sides = REG32((u32)sample + SAMPLE_CHANNELS);
    const s32 *side[2] = {
        (const s32 *)REG32((u32)sample + SAMPLE_BUFFER_0) + SAMPLE_GUARD_FRAMES,
        (const s32 *)REG32((u32)sample + SAMPLE_BUFFER_1) + SAMPLE_GUARD_FRAMES,
    };
    if (sides != 2 || !REG32((u32)sample + SAMPLE_BUFFER_1)) sides = 1;
    for (u32 c = 0; c < columns; ++c) {
        struct column column = {0, 0, 0, 0};
        u32 a, b;
        bounds(first, span, c, frames, &a, &b);
        for (u32 s = 0; s < sides; ++s)
            for (u32 f = a; f < b; ++f) add(&column, side[s][f]);
        store(out + c * 3, &column);
    }
    *count = columns * 3;
    return OK;
}

static s32 pcm(const u8 *at, u32 bytes)
{
    if (bytes == 2) return (s32)((u32)at[0] << 16 | (u32)at[1] << 24);
    if (bytes == 3) return (s32)((u32)at[0] << 8 | (u32)at[1] << 16 | (u32)at[2] << 24);
    return (s32)get_u32(at);
}

u8 file_chunk[FILE_CHUNK_BYTES];

/* u32 first frame, u32 frames per column, u8 columns, u8 reserved, name. */
u32 file_peaks(const u8 *p, u32 bytes, u8 *out, u32 *count)
{
    u32 first = get_u32(p), span = get_u32(p + 4), columns = p[8];
    char path[SAMPLE_PATH_BYTES];
    if (!span || !columns || columns > PEAK_COLUMNS_MAX || !browse_path(path, p + 10, bytes - 10)) return BAD_RANGE;
    FIL file; struct wav_info wav;
    if (fatfs_open(&file, path, FA_READ)) return IO_ERROR;
    u32 status = wav_read_info(&file, &wav) ? OK : BAD_IMAGE;
    u32 frame = wav.channels * wav.bytes_per_sample, per_chunk = frame ? FILE_CHUNK_BYTES / frame : 0;
    for (u32 c = 0; c < columns && status == OK; ++c) {
        struct column column = {0, 0, 0, 0};
        u32 a, b;
        bounds(first, span, c, wav.frames, &a, &b);
        /* Whole, one chunk after another; or a chunk at each step. */
        u32 sparse = (b - a) * frame > FILE_COLUMN_BYTES;
        u32 step = sparse ? (b - a) / (FILE_COLUMN_BYTES / FILE_CHUNK_BYTES) : per_chunk;
        if (a < b && !sparse && resource_seek_absolute(&file, wav.data_offset + a * frame)) status = IO_ERROR;
        for (u32 f = a; f < b && status == OK; f += step) {
            u32 take = b - f < per_chunk ? b - f : per_chunk, done;
            if ((sparse && resource_seek_absolute(&file, wav.data_offset + f * frame)) ||
                resource_read_bytes(&file, file_chunk, take * frame, &done) || done != take * frame) { status = IO_ERROR; break; }
            for (u32 i = 0; i < take * wav.channels; ++i)
                add(&column, pcm(file_chunk + i * wav.bytes_per_sample, wav.bytes_per_sample));
        }
        store(out + c * 3, &column);
    }
    resource_close(&file);
    *count = columns * 3;
    return status;
}
