#include "ext.h"

/* The card's folders for the sample browser: one folder is browsed at a
 * time, the active folder until a requester opens another, and its folders
 * and WAV files are listed by index. A sample chosen outside the active
 * folder loads from where it is; Save copies its file into the active
 * folder, where the saved preset looks for it. Runs in the foreground with
 * the shared slot claimed; the stock directory scan and its long-name
 * scratch are not reentrant. */

#define WAV_CHUNKS_MAX        64u
#define WAV_FORMAT_PCM        1u
#define WAV_FORMAT_EXTENSIBLE 0xfffeu
#define FOLDER_ENTRY_HEADER   10u /* u32 frames, u32 rate, u8 channels, u8 name length */
#define FOLDER_LIST_HEADER    3u /* u16 first index, u8 flags */
#define FOLDER_LIST_END       0x01u
#define BROWSE_REPLY_HEADER   5u /* u16 entries, u16 found, u8 flags */
#define BROWSE_AT_ROOT        0x01u
#define BROWSE_AT_ACTIVE      0x02u
#define BROWSE_NOT_FOUND      0xffffu
#define DRIVE                 "1:"
#define DRIVE_BYTES           2u
#define COPY_CHUNKS_PER_PASS  16u
#define CATALOG_ENTRIES       2048u /* the stock browser's own limit */

/* The browsed folder under the card root, without slashes at its ends
 * (empty: the root); unused until a requester leaves the active folder. */
static char browse[BROWSE_PATH_BYTES];
static u8 browse_away;

int wav_read_info(FIL *file, struct wav_info *info)
{
    u8 head[24]; u32 done;
    if (resource_read_bytes(file, head, 12, &done) || done != 12 ||
        memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4)) return 0;
    u32 at = 12, format = 0;
    memset(info, 0, sizeof(*info));
    for (u32 n = 0; n < WAV_CHUNKS_MAX; ++n) {
        if (resource_seek_absolute(file, at) || resource_read_bytes(file, head, 8, &done) || done != 8) return 0;
        u32 size = get_u32(head + 4);
        if (!memcmp(head, "fmt ", 4)) {
            if (size < 16 || resource_read_bytes(file, head + 8, 16, &done) || done != 16) return 0;
            format = get_u16(head + 8);
            info->channels = (u8)get_u16(head + 10);
            info->rate = get_u32(head + 12);
            info->bytes_per_sample = (u8)(get_u16(head + 22) / 8);
        } else if (!memcmp(head, "data", 4)) {
            u32 frame = info->channels * info->bytes_per_sample;
            if ((format != WAV_FORMAT_PCM && format != WAV_FORMAT_EXTENSIBLE) || !frame ||
                info->bytes_per_sample < 2 || info->bytes_per_sample > 4) return 0;
            info->data_offset = at + 8;
            info->frames = size / frame;
            return 1;
        }
        at += 8 + size + (size & 1u);
    }
    return 0;
}

static u32 active_name(char *out)
{
    const char *folder = (const char *)ACTIVE_FOLDER_NAME;
    u32 length = 0;
    while (length < ACTIVE_FOLDER_BYTES - 1 && folder[length]) { out[length] = folder[length]; ++length; }
    out[length] = 0;
    return length;
}

static int name_valid(const u8 *name, u32 bytes)
{
    if (!bytes || bytes >= ZONE_NAME_BYTES || (name[0] == '.' && (bytes == 1 || (bytes == 2 && name[1] == '.')))) return 0;
    for (u32 i = 0; i < bytes; ++i)
        if (name[i] < ' ' || name[i] == '/' || name[i] == '\\' || name[i] == ':') return 0;
    return 1;
}

static u32 join(char *path, u32 length, const u8 *name, u32 bytes)
{
    path[length] = '/';
    memcpy(path + length + 1, name, bytes);
    path[length + 1 + bytes] = 0;
    return length + 1 + bytes;
}

/* A file of the active folder: where takes are written and the saved preset
 * looks for its samples. */
u32 folder_path(char *path, const u8 *name, u32 bytes)
{
    memcpy(path, DRIVE "/", DRIVE_BYTES + 1);
    u32 length = active_name(path + DRIVE_BYTES + 1);
    if (!length || !name_valid(name, bytes)) return 0;
    join(path, DRIVE_BYTES + 1 + length, name, bytes);
    return 1;
}

/* The browsed folder as the stock loader's folder prefix: "1:" alone is
 * the card root. Returns its length. */
u32 browse_prefix(char *out)
{
    memcpy(out, DRIVE, DRIVE_BYTES);
    out[DRIVE_BYTES] = '/';
    u32 length = browse_away ? strlen(browse) : active_name(out + DRIVE_BYTES + 1);
    if (browse_away) memcpy(out + DRIVE_BYTES + 1, browse, length);
    length = length ? DRIVE_BYTES + 1 + length : DRIVE_BYTES;
    out[length] = 0;
    return length;
}

static int browse_is_active(void)
{
    char active[ACTIVE_FOLDER_BYTES];
    active_name(active);
    return !browse_away || !strcasecmp(browse, active);
}

/* A file of the browsed folder. */
u32 browse_path(char *path, const u8 *name, u32 bytes)
{
    u32 length = browse_prefix(path);
    if (!name_valid(name, bytes) || length + 1 + bytes >= SAMPLE_PATH_BYTES) return 0;
    join(path, length, name, bytes);
    return 1;
}

static int wav_info_at(const char *path, struct wav_info *info)
{
    FIL file;
    if (fatfs_open(&file, path, FA_READ)) return 0;
    int valid = wav_read_info(&file, info);
    resource_close(&file);
    return valid;
}

/* Where a file chosen in the browsed folder loads from: its path, and the
 * folder prefix for the stock loader, empty when the active folder serves
 * it (the file is there, or the same audio is there under its name). The
 * saved preset names samples without their folder, so a different file of
 * that name in the active folder refuses the choice. */
u32 browse_source(const u8 *name, u32 bytes, char *prefix, char *path)
{
    char ours[PATH_BYTES];
    struct wav_info chosen, held;
    prefix[0] = 0;
    if (browse_is_active()) return folder_path(path, name, bytes) ? OK : BAD_RANGE;
    if (!browse_path(path, name, bytes)) return BAD_RANGE;
    if (!wav_info_at(path, &chosen)) return IO_ERROR;
    if (folder_path(ours, name, bytes) && wav_info_at(ours, &held)) {
        if (held.frames != chosen.frames || held.rate != chosen.rate || held.channels != chosen.channels ||
            held.bytes_per_sample != chosen.bytes_per_sample) return FILE_EXISTS;
        memcpy(path, ours, PATH_BYTES);
        return OK;
    }
    browse_prefix(prefix);
    return OK;
}

/* The browsed folder's entries: its folders, then its WAV files, each by
 * name with digit runs in number order; hidden and dot names and names too
 * long for a zone are left out. Read from the card when a folder is opened
 * and when the listing is asked for another folder's. */
struct scan { u32 dir[DIR_BYTES / 4], info[FILINFO_BYTES / 4]; char long_name[LONG_NAME_BYTES]; };

static struct {
    char folder[BROWSE_PREFIX_BYTES]; /* whose entries these are; empty when none */
    u32 count;
    u16 order[CATALOG_ENTRIES];
    u8 is_folder[CATALOG_ENTRIES];
    char name[CATALOG_ENTRIES][ZONE_NAME_BYTES];
} catalog;

static int compare(u32 a, u32 b)
{
    if (catalog.is_folder[a] != catalog.is_folder[b]) return catalog.is_folder[a] ? -1 : 1;
    const u8 *x = (const u8 *)catalog.name[a], *y = (const u8 *)catalog.name[b];
    while (*x && *y) {
        if (*x >= '0' && *x <= '9' && *y >= '0' && *y <= '9') {
            u32 m = 0, n = 0;
            while (*x >= '0' && *x <= '9') m = m * 10 + (*x++ - '0');
            while (*y >= '0' && *y <= '9') n = n * 10 + (*y++ - '0');
            if (m != n) return m < n ? -1 : 1;
            continue;
        }
        int c = tolower(*x) - tolower(*y);
        if (c) return c;
        ++x; ++y;
    }
    return *x ? 1 : *y ? -1 : 0;
}

static int catalog_read(void)
{
    struct scan s;
    char folder[BROWSE_PREFIX_BYTES + 1];
    u32 length = browse_prefix(folder);
    catalog.folder[0] = 0; catalog.count = 0;
    if (length == DRIVE_BYTES) { folder[length] = '/'; folder[length + 1] = 0; }
    if (resource_directory_open_for_scan(s.dir, folder)) return 0;
    while (catalog.count < CATALOG_ENTRIES) {
        memset(s.info, 0, sizeof(s.info));
        s.long_name[0] = 0;
        s.info[FILINFO_LONG_NAME / 4] = (u32)s.long_name;
        s.info[FILINFO_LONG_NAME_BYTES / 4] = LONG_NAME_BYTES;
        if (resource_directory_read_next_file_info(s.dir, s.info)) return 0;
        const char *short_name = (const char *)s.info + FILINFO_SHORT_NAME;
        if (!short_name[0]) break;
        u32 attribute = BYTE((u32)s.info + FILINFO_ATTRIBUTE), is_folder = (attribute & ATTRIBUTE_DIRECTORY) != 0;
        const char *name = s.long_name[0] ? s.long_name : short_name;
        u32 bytes = strlen(name);
        if (attribute & ATTRIBUTE_HIDDEN || name[0] == '.' || bytes >= ZONE_NAME_BYTES ||
            !(is_folder ? bytes <= BROWSE_NAME_MAX : bytes > 4 && !strncasecmp(name + bytes - 4, ".wav", 4))) continue;
        memcpy(catalog.name[catalog.count], name, bytes + 1);
        catalog.is_folder[catalog.count] = (u8)is_folder;
        catalog.order[catalog.count] = (u16)catalog.count;
        ++catalog.count;
    }
    /* Shell sort of the order. */
    for (u32 gap = catalog.count / 2; gap; gap /= 2)
        for (u32 i = gap; i < catalog.count; ++i) {
            u16 moved = catalog.order[i];
            u32 j = i;
            for (; j >= gap && compare(catalog.order[j - gap], moved) > 0; j -= gap) catalog.order[j] = catalog.order[j - gap];
            catalog.order[j] = moved;
        }
    folder[length] = 0;
    memcpy(catalog.folder, folder, length + 1);
    return 1;
}

static int catalog_current(void)
{
    char folder[BROWSE_PREFIX_BYTES];
    browse_prefix(folder);
    return (catalog.folder[0] && !strcmp(folder, catalog.folder)) || catalog_read();
}

/* Up to `limit` entries from index `first`, as many as fit. Returns reply
 * bytes, or 0 when the folder cannot be read. */
u32 folder_list(u32 first, u32 limit, u8 *out, u32 room)
{
    u32 used = FOLDER_LIST_HEADER, index = first;
    if (!catalog_current()) return 0;
    put_u16(out, first); out[2] = 0;
    for (; index < catalog.count && limit; ++index, --limit) {
        u32 entry = catalog.order[index];
        const char *name = catalog.name[entry];
        u32 bytes = strlen(name);
        if (used + FOLDER_ENTRY_HEADER + bytes > room) break;
        char path[SAMPLE_PATH_BYTES];
        struct wav_info wav;
        memset(&wav, 0, sizeof(wav));
        if (catalog.is_folder[entry]) wav.channels = FOLDER_ENTRY_IS_FOLDER;
        else if (!browse_path(path, (const u8 *)name, bytes) || !wav_info_at(path, &wav)) memset(&wav, 0, sizeof(wav));
        put_u32(out + used, wav.frames); put_u32(out + used + 4, wav.rate);
        out[used + 8] = wav.channels; out[used + 9] = (u8)bytes;
        memcpy(out + used + FOLDER_ENTRY_HEADER, name, bytes);
        used += FOLDER_ENTRY_HEADER + bytes;
    }
    if (index >= catalog.count) out[2] = FOLDER_LIST_END;
    return used;
}

/* Whether `text` holds `part`, letters of either case alike. */
static int contains(const char *text, const char *part)
{
    for (; *text; ++text) {
        u32 n = 0;
        while (part[n] && tolower((u8)text[n]) == tolower((u8)part[n])) ++n;
        if (!part[n]) return 1;
    }
    return 0;
}

static void browse_reply(u32 found, u8 *out, u32 *count)
{
    char prefix[BROWSE_PREFIX_BYTES];
    u32 length = browse_prefix(prefix);
    length = length == DRIVE_BYTES ? 0 : length - DRIVE_BYTES - 1;
    put_u16(out, catalog.count); put_u16(out + 2, found);
    out[4] = (u8)((length ? 0 : BROWSE_AT_ROOT) | (browse_is_active() ? BROWSE_AT_ACTIVE : 0));
    memcpy(out + BROWSE_REPLY_HEADER, prefix + DRIVE_BYTES + 1, length);
    *count = BROWSE_REPLY_HEADER + length;
}

/* Find in the browsed folder: u16 first index, then text. Replies as Open
 * folder does, with the first entry from that index whose name holds the
 * text. */
static u32 folder_find(const u8 *p, u32 bytes, u8 *out, u32 *count)
{
    char text[ZONE_NAME_BYTES];
    if (bytes < 3 || bytes - 2 >= ZONE_NAME_BYTES || !catalog_current()) return bytes < 3 ? BAD_LENGTH : IO_ERROR;
    memcpy(text, p + 2, bytes - 2); text[bytes - 2] = 0;
    u32 found = BROWSE_NOT_FOUND;
    for (u32 index = get_u16(p); index < catalog.count && found == BROWSE_NOT_FOUND; ++index)
        if (contains(catalog.name[catalog.order[index]], text)) found = index;
    browse_reply(found, out, count);
    return OK;
}

/* Open a folder to browse: u8 operation, then a name: the folder to enter,
 * or an entry to find where the browser arrives. Replies with the folder's
 * entry count, the found entry's index (the folder left, after Up), flags
 * and the path. A folder that cannot be read leaves the browser where it
 * was. */
u32 folder_browse(const u8 *p, u32 bytes, u8 *out, u32 *count)
{
    if (p[0] == BROWSE_FIND) return folder_find(p + 1, bytes - 1, out, count);
    char before[BROWSE_PATH_BYTES], path[ACTIVE_FOLDER_BYTES], find[ZONE_NAME_BYTES];
    u32 was_away = browse_away, n = bytes - 1;
    if (n >= ZONE_NAME_BYTES) return BAD_RANGE;
    memcpy(before, browse, sizeof(before));
    memcpy(find, p + 1, n); find[n] = 0;
    u32 length = browse_away ? strlen(browse) : active_name(path);
    if (browse_away) memcpy(path, browse, length + 1);
    switch (p[0]) {
    case BROWSE_STAY: break;
    case BROWSE_HOME: browse_away = 0; break;
    case BROWSE_ROOT: browse_away = 1; browse[0] = 0; break;
    case BROWSE_ENTER:
        if (!name_valid(p + 1, n) || length + (length ? 1 : 0) + n >= BROWSE_PATH_BYTES) return BAD_RANGE;
        if (length) join(path, length, p + 1, n);
        else memcpy(path, find, n + 1);
        memcpy(browse, path, BROWSE_PATH_BYTES);
        browse_away = 1; find[0] = 0;
        break;
    case BROWSE_UP: {
        if (!length) return BAD_STATE;
        u32 cut = length;
        while (cut && path[cut - 1] != '/') --cut;
        memcpy(find, path + cut, length - cut + 1);
        path[cut ? cut - 1 : 0] = 0;
        memcpy(browse, path, BROWSE_PATH_BYTES);
        browse_away = 1;
        break;
    }
    default: return BAD_RANGE;
    }
    if (!catalog_read()) {
        memcpy(browse, before, sizeof(before)); browse_away = (u8)was_away;
        return IO_ERROR;
    }
    u32 found = BROWSE_NOT_FOUND;
    for (u32 index = 0; find[0] && index < catalog.count && found == BROWSE_NOT_FOUND; ++index)
        if (!strcasecmp(catalog.name[catalog.order[index]], find)) found = index;
    browse_reply(found, out, count);
    return OK;
}

/* ---- Save: samples from other folders ---- */

static struct { u8 *object; FIL from, to; char path[SAMPLE_PATH_BYTES]; } copy;

static void copy_close(int keep)
{
    resource_close(&copy.from);
    resource_close(&copy.to);
    if (!keep) fatfs_unlink(copy.path);
    copy.object = 0;
}

/* The next active zone whose sample's file is not in the active folder,
 * with the path it belongs at. */
static u8 *external_sample(char *path)
{
    for (u32 channel = 0; channel < PRESET_CHANNELS; ++channel)
        for (u32 zone = 0; zone < active_zones(channel); ++zone) {
            u8 *object = (u8 *)REG32((u32)zone_record(channel, zone) + ZONE_SAMPLE_OBJECT);
            if (!object || !object[SAMPLE_PATH] ||
                !folder_path(path, object, strlen((const char *)object))) continue;
            if (strcasecmp((const char *)object + SAMPLE_PATH, path)) return object;
        }
    return 0;
}

/* One pass of copying those files in, unchanged: OK when none is left,
 * COPY_MORE to be called again, or the failure, the unfinished copy removed.
 * A sample is rebased once its file is in place, as the stock copy does. */
u32 externals_copy_step(void)
{
    if (!copy.object) {
        u8 *object = external_sample(copy.path);
        if (!object) return OK;
        /* Its name already there: the choice checked it holds the same audio. */
        if (!fatfs_open(&copy.to, copy.path, FA_READ)) {
            resource_close(&copy.to);
            sample_object_rebase_to_active_folder(object);
            return COPY_MORE;
        }
        if (fatfs_open(&copy.from, (const char *)object + SAMPLE_PATH, FA_READ)) return IO_ERROR;
        if (fatfs_open(&copy.to, copy.path, FA_WRITE_CREATE_ALWAYS)) { resource_close(&copy.from); return IO_ERROR; }
        copy.object = object;
        return COPY_MORE;
    }
    for (u32 n = 0; n < COPY_CHUNKS_PER_PASS; ++n) {
        u32 read, written;
        if (resource_read_bytes(&copy.from, file_chunk, FILE_CHUNK_BYTES, &read) ||
            (read && (resource_write_bytes(&copy.to, file_chunk, read, &written) || written != read))) {
            copy_close(0);
            return IO_ERROR;
        }
        if (read == FILE_CHUNK_BYTES) continue;
        u8 *object = copy.object;
        if (fatfs_sync_file(&copy.to)) { copy_close(0); return IO_ERROR; }
        copy_close(1);
        sample_object_rebase_to_active_folder(object);
        folder_changed();
        break;
    }
    return COPY_MORE;
}

/* A file was added to the active folder or removed from it: the next
 * listing reads the card again. */
void folder_changed(void) { catalog.folder[0] = 0; }

void folder_init(void) { browse_away = 0; browse[0] = 0; copy.object = 0; folder_changed(); }
