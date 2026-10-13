#include "patch.h"

/* Small PCM loops only. All allocation and SD work belongs to foreground;
 * the panel service assembles bounded, sequential RAM chunks. */
#define SAMPLE_MAX_BYTES 16384u
#define SAMPLE_NAME_MAX 31u
#define ACTIVE_FOLDER_NAME 0x8022e6d4u
#define SAMPLE_PATH_BYTES 128u
#define FA_WRITE_CREATE_NEW 0x06u

static u8 *stage;
static u32 size, received, expected_crc, touched_at;
static char path[SAMPLE_PATH_BYTES], temporary[SAMPLE_PATH_BYTES];
static volatile u32 job, job_sequence;

int sample_transfer_busy(void) { return stage != 0 || job; }

static int sample_path(const u8 *name, u32 bytes)
{
    if (bytes < 5 || bytes > SAMPLE_NAME_MAX || memcmp(name + bytes - 4, ".wav", 4)) return 0;
    for (u32 i = 0; i < bytes - 4; ++i) {
        u8 c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    const char *folder = (const char *)ACTIVE_FOLDER_NAME;
    u32 length = 0;
    while (length < 60 && folder[length]) {
        if (folder[length] == '/' || folder[length] == '\\' || folder[length] == ':') return 0;
        ++length;
    }
    if (!length || length == 60 || (length == 1 && folder[0] == '.') ||
        (length == 2 && folder[0] == '.' && folder[1] == '.')) return 0;
    memcpy(path, "1:/", 3); memcpy(path + 3, folder, length);
    path[3 + length] = '/'; memcpy(path + 4 + length, name, bytes);
    length += 4 + bytes; path[length] = 0;
    memcpy(temporary, path, length); memcpy(temporary + length, ".tmp", 5);
    return 1;
}

static void discard(void)
{
    u8 *old = stage;
    u32 irq = cpu_irq_save(); stage = 0; received = 0; cpu_irq_restore(irq);
    free(old);
}

void sample_transfer_command(u32 command, u32 sequence, const u8 *p, u32 bytes)
{
    u32 status = OK;
    if (job) { reply(command, sequence, BUSY, 0, 0); return; }
    switch (command) {
    case CMD_SAMPLE_BEGIN:
        if (bytes < 13) { status = BAD_LENGTH; break; }
        if (install_busy() || stock_busy() || boot_mode == BOOT_TRIAL) { status = BUSY; break; }
        if (get_u32(p) < 48 || get_u32(p) > SAMPLE_MAX_BYTES || !sample_path(p + 8, bytes - 8)) {
            status = BAD_RANGE; break;
        }
        size = get_u32(p); expected_crc = get_u32(p + 4); received = 0;
        goto queue;
    case CMD_SAMPLE_DATA: {
        if (bytes < 5) { status = BAD_LENGTH; break; }
        if (!stage) { status = BAD_STATE; break; }
        u32 offset = get_u32(p), count = bytes - 4;
        if (offset > size || count > size - offset) { status = BAD_RANGE; break; }
        if (offset == received) {
            memcpy(stage + offset, p + 4, count); received += count;
        } else if (offset + count != received || memcmp(stage + offset, p + 4, count)) {
            status = BAD_RANGE; break;
        }
        /* Exact replay of the last chunk is safe after a lost reply. */
        touched_at = REG32(TICKS);
        u8 out[4]; put_u32(out, received); reply(command, sequence, OK, out, 4); return;
    }
    case CMD_SAMPLE_COMMIT:
        if (bytes) { status = BAD_LENGTH; break; }
        if (!stage || received != size) { status = BAD_STATE; break; }
        goto queue;
    case CMD_SAMPLE_ABORT:
        if (bytes) { status = BAD_LENGTH; break; }
        goto queue;
    default: status = BAD_COMMAND; break;
    }
    reply(command, sequence, status, 0, 0); return;
queue:
    touched_at = REG32(TICKS); job_sequence = sequence; MEMORY_BARRIER(); job = command;
}

static int wav_valid(void)
{
    if (memcmp(stage, "RIFF", 4) || get_u32(stage + 4) != size - 8 ||
        memcmp(stage + 8, "WAVEfmt ", 8) || get_u32(stage + 16) != 16 ||
        get_u16(stage + 20) != 1 || memcmp(stage + 36, "data", 4) ||
        get_u32(stage + 40) != size - 44) return 0;
    u32 channels = get_u16(stage + 22), bits = get_u16(stage + 34);
    u32 align = channels * (bits >> 3);
    return (channels == 1 || channels == 2) && (bits == 16 || bits == 24 || bits == 32) &&
        get_u32(stage + 24) == 48000 && get_u16(stage + 32) == align &&
        get_u32(stage + 28) == 48000 * align && (size - 44) % align == 0;
}

static int save_sample(void)
{
    if (~crc32_update(~0u, stage, size) != expected_crc) return BAD_CRC;
    if (!wav_valid()) return BAD_IMAGE;
    FIL file; u32 done;
    int opened = fatfs_open(&file, temporary, FA_WRITE_CREATE_NEW);
    if (opened) return opened == 8 ? FILE_EXISTS : IO_ERROR; /* FR_EXIST */
    int failed = resource_write_bytes(&file, stage, size, &done) || done != size;
    if (!failed) failed = fatfs_sync_file(&file);
    if (resource_close(&file)) failed = 1;
    if (!failed) {
        failed = fatfs_open(&file, temporary, FA_READ);
        if (!failed) {
            failed = file.words[FIL_FILE_SIZE / 4] != size ||
                resource_read_bytes(&file, stage, size, &done) || done != size;
            if (resource_close(&file)) failed = 1;
            if (!failed) failed = ~crc32_update(~0u, stage, size) != expected_crc;
        }
    }
    int status = failed ? IO_ERROR : fatfs_rename(temporary, path);
    if (status) fatfs_unlink(temporary); /* only the file we exclusively created */
    return failed ? IO_ERROR : status == 8 ? FILE_EXISTS : status ? IO_ERROR : OK;
}

void sample_transfer_foreground_poll(void)
{
    u32 command = job;
    if (!command) {
        u32 irq = cpu_irq_save();
        if (stage && (u32)(REG32(TICKS) - touched_at) > STAGE_EXPIRY_TICKS)
            command = job = CMD_SAMPLE_ABORT;
        cpu_irq_restore(irq);
        if (!command) return;
        discard(); job = 0; return;
    }
    u32 status = OK, sequence = job_sequence;
    if (command == CMD_SAMPLE_ABORT) discard();
    else if (!storage_claim()) status = BUSY;
    else {
        if (command == CMD_SAMPLE_BEGIN) {
            stage = malloc(size);
            if (!stage) status = NO_MEMORY;
            else status_show("Receiving sample");
        } else {
            status = save_sample(); discard();
            status_show(status ? "Sample not saved" : "Sample saved");
        }
        storage_release();
    }
    counters.last_job_result = status;
    u32 length = 0;
    if (command == CMD_SAMPLE_COMMIT && !status) while (path[length]) ++length;
    reply(command, sequence, status, path, length);
    MEMORY_BARRIER(); job = 0;
}
