#include "ext.h"

/* Streams for an editor's background mirror: one small request, then reply
 * frames under the request's sequence until the data is complete, sent as
 * the UART drains. The requester's MIDI line to the module carries notes;
 * its replies travel on the module's own MIDI OUT, so a whole preset or
 * waveform costs one request. A new stream replaces the running one;
 * ordinary commands still run beside it.
 *
 * Every frame: u8 status, u8 kind, u16 index, then data. The index is the
 * byte offset (model), column (peaks), channel (samples), first entry
 * (browsed folder) or first record (EQ, filter). A failed frame carries its status and ends the stream. */

enum { STREAM_MODEL, STREAM_ZONE_PEAKS, STREAM_FILE_PEAKS, STREAM_SAMPLES, STREAM_FOLDER, STREAM_EQ, STREAM_FILTER };
#define STREAM_HEADER        3u  /* kind, u16 index */
#define STREAM_DATA          (REPLY_BYTES - 1u - STREAM_HEADER)
#define STREAM_REQUEST_MAX   48u
#define STREAM_PEAK_COLUMNS  40u /* zone_peaks and file_peaks take at most 40 */
#define FILE_PEAKS_PASS_BYTES (8u * FILE_COLUMN_BYTES) /* the most one frame of file peaks reads, so a pass stays short */
#define PEAK_COLUMNS_LIMIT   702u /* the editor's waveform width in pixels */
#define SAMPLE_INFO_BYTES    14u
#define STREAM_EQ_RECORDS    (STREAM_DATA / EQ_RECORD_BYTES)
#define STREAM_FILTER_RECORDS (STREAM_DATA / FILTER_RECORD_BYTES)
#define FOLDER_LIST_HEADER   3u  /* u16 first index, u8 flags */
#define FOLDER_ENTRY_HEADER  10u
#define FOLDER_LIST_END      0x01u
#define ZONE_PEAKS_REQUEST   11u /* channel, zone, u32 first, u32 span, u8 columns */
#define FILE_PEAKS_HEADER    10u /* u32 first, u32 span, u8 columns, u8 reserved */

static struct {
    volatile u32 pending;
    u32 active, sequence, kind, index, end, bytes, pending_sequence, pending_bytes;
    u8 request[STREAM_REQUEST_MAX], pending_request[STREAM_REQUEST_MAX];
} stream;

/* Panel context: keep the newest request for the foreground. */
void stream_request(u32 sequence, const u8 *p, u32 bytes)
{
    if (!bytes || bytes > STREAM_REQUEST_MAX) { reply(EDIT_STREAM, sequence, BAD_LENGTH, 0, 0); return; }
    stream.pending = 0;
    MEMORY_BARRIER();
    memcpy(stream.pending_request, p, bytes);
    stream.pending_sequence = sequence; stream.pending_bytes = bytes;
    MEMORY_BARRIER();
    stream.pending = 1;
}

static u32 start(void)
{
    const u8 *p = stream.request;
    stream.index = 0;
    switch (stream.kind = p[0]) {
    case STREAM_MODEL: {
        if (stream.bytes != 5) return BAD_LENGTH;
        u32 offset = get_u16(p + 1), length = get_u16(p + 3);
        if (!length || offset + length > PRESET_RECORD_BYTES) return BAD_RANGE;
        stream.index = offset; stream.end = offset + length;
        return OK;
    }
    case STREAM_ZONE_PEAKS:
        if (stream.bytes != 13) return BAD_LENGTH;
        stream.end = get_u16(p + 11);
        return p[1] < PRESET_CHANNELS && p[2] < ZONES && get_u32(p + 7) && stream.end &&
               stream.end <= PEAK_COLUMNS_LIMIT ? OK : BAD_RANGE;
    case STREAM_FILE_PEAKS:
        if (stream.bytes <= 11) return BAD_LENGTH;
        stream.end = get_u16(p + 9);
        return get_u32(p + 5) && stream.end && stream.end <= PEAK_COLUMNS_LIMIT ? OK : BAD_RANGE;
    case STREAM_SAMPLES:
        stream.end = PRESET_CHANNELS;
        return stream.bytes == 1 ? OK : BAD_LENGTH;
    case STREAM_FOLDER:
        /* The whole listing, or u16 first, u16 entries of it. */
        if (stream.bytes != 1 && stream.bytes != 5) return BAD_LENGTH;
        stream.index = stream.bytes == 5 ? get_u16(p + 1) : 0;
        stream.end = stream.bytes == 5 ? stream.index + get_u16(p + 3) : ~0u;
        return stream.end > stream.index ? OK : BAD_RANGE;
    case STREAM_EQ:
        stream.end = EQ_RECORDS;
        return stream.bytes == 1 ? OK : BAD_LENGTH;
    case STREAM_FILTER:
        stream.end = EQ_RECORDS;
        return stream.bytes == 1 ? OK : BAD_LENGTH;
    }
    return BAD_RANGE;
}

static void send(u32 status, const u8 *data, u32 bytes)
{
    u8 frame[REPLY_BYTES - 1u];
    frame[0] = (u8)stream.kind; put_u16(frame + 1, stream.index);
    if (bytes) memcpy(frame + STREAM_HEADER, data, bytes);
    reply(EDIT_STREAM, stream.sequence, status, frame, STREAM_HEADER + bytes);
    if (status != OK) stream.active = 0;
}

/* One frame; 0 to try again on a later pass. */
static int next_frame(void)
{
    const u8 *p = stream.request;
    u8 data[STREAM_DATA];
    u32 count = 0, status = OK;
    switch (stream.kind) {
    case STREAM_MODEL:
        count = stream.end - stream.index < STREAM_DATA ? stream.end - stream.index : STREAM_DATA;
        memcpy(data, (const u8 *)PRESET_MODEL + stream.index, count);
        send(OK, data, count);
        stream.index += count;
        break;
    case STREAM_ZONE_PEAKS: case STREAM_FILE_PEAKS: {
        u32 columns = stream.end - stream.index < STREAM_PEAK_COLUMNS ? stream.end - stream.index : STREAM_PEAK_COLUMNS;
        u8 part[STREAM_REQUEST_MAX];
        if (stream.kind == STREAM_ZONE_PEAKS) {
            u32 span = get_u32(p + 7);
            part[0] = p[1]; part[1] = p[2];
            put_u32(part + 2, get_u32(p + 3) + stream.index * span); put_u32(part + 6, span);
            part[10] = (u8)columns;
            status = zone_peaks(part, data, &count);
        } else {
            if (!storage_claim()) return 0;
            u32 span = get_u32(p + 5), name = stream.bytes - 11;
            /* Fewer columns a frame when each reads much of the card. */
            u32 column = span < FILE_COLUMN_BYTES / FILE_FRAME_BYTES_MAX ? span * FILE_FRAME_BYTES_MAX : FILE_COLUMN_BYTES;
            if (columns > FILE_PEAKS_PASS_BYTES / column) columns = FILE_PEAKS_PASS_BYTES / column;
            put_u32(part, get_u32(p + 1) + stream.index * span); put_u32(part + 4, span);
            part[8] = (u8)columns; part[9] = 0;
            memcpy(part + FILE_PEAKS_HEADER, p + 11, name);
            status = file_peaks(part, FILE_PEAKS_HEADER + name, data, &count);
            storage_release();
        }
        send(status, data, status == OK ? count : 0);
        stream.index += columns;
        break;
    }
    case STREAM_SAMPLES:
        for (u32 zone = 0; zone < ZONES; ++zone) {
            u8 pair[2] = {(u8)stream.index, (u8)zone};
            u32 bytes;
            sample_info(pair, data + zone * SAMPLE_INFO_BYTES, &bytes);
        }
        send(OK, data, ZONES * SAMPLE_INFO_BYTES);
        stream.index++;
        break;
    case STREAM_EQ:
        count = stream.end - stream.index < STREAM_EQ_RECORDS ? stream.end - stream.index : STREAM_EQ_RECORDS;
        for (u32 r = 0; r < count; ++r) eq_record(stream.index + r, data + r * EQ_RECORD_BYTES);
        send(OK, data, count * EQ_RECORD_BYTES);
        stream.index += count;
        break;
    case STREAM_FILTER:
        count = stream.end - stream.index < STREAM_FILTER_RECORDS ? stream.end - stream.index : STREAM_FILTER_RECORDS;
        for (u32 r = 0; r < count; ++r) filter_record(stream.index + r, data + r * FILTER_RECORD_BYTES);
        send(OK, data, count * FILTER_RECORD_BYTES);
        stream.index += count;
        break;
    case STREAM_FOLDER: {
        if (!storage_claim()) return 0;
        count = folder_list(stream.index, stream.end - stream.index, data, sizeof(data));
        storage_release();
        if (!count) { send(IO_ERROR, 0, 0); break; }
        /* The page's own header repeats the first index; send its flags and entries. */
        u32 entries = 0;
        for (u32 at = FOLDER_LIST_HEADER; at < count; at += FOLDER_ENTRY_HEADER + data[at + 9]) ++entries;
        send(OK, data + 2, count - 2);
        if (data[2] & FOLDER_LIST_END) stream.active = 0;
        stream.index += entries;
        break;
    }
    }
    if (stream.index >= stream.end) stream.active = 0;
    return 1;
}

void stream_foreground(void)
{
    if (stream.pending) {
        stream.pending = 0;
        MEMORY_BARRIER();
        memcpy(stream.request, stream.pending_request, stream.pending_bytes);
        stream.sequence = stream.pending_sequence; stream.bytes = stream.pending_bytes;
        u32 status = start();
        stream.active = status == OK;
        if (status != OK) reply(EDIT_STREAM, stream.sequence, status, 0, 0);
    }
    /* One frame at a time, once the UART has sent the last. */
    if (stream.active && sysex_tx_idle()) next_frame();
}

void stream_init(void) { memset(&stream, 0, sizeof(stream)); }
