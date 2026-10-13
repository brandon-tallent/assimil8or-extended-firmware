#include "patch.h"

static u8 *stage;
static u8 block[BLOCK_BYTES];
static u32 block_offset, block_size, block_received, block_crc, block_open;
static u32 target_crc, touched_at;
/* The staged file: the application image, or an extension image. */
static u32 stage_bytes, stage_extension;
static volatile u32 job, job_sequence;
static u32 reset_pending, reset_requested_at;

int read_image(const char *path, u8 *image)
{
    FIL file; u32 done = 0;
    int status = fatfs_open(&file, path, FA_READ);
    if (status) return IO_ERROR;
    if (file.words[FIL_FILE_SIZE / 4] != IMAGE_FILE_BYTES) status = BAD_IMAGE;
    else if (resource_read_bytes(&file, image, IMAGE_FILE_BYTES, &done) ||
             done != IMAGE_FILE_BYTES) status = IO_ERROR;
    if (resource_close(&file) && !status) status = IO_ERROR;
    return status;
}

int image_valid(u8 *image)
{
    u32 marker_offset = IMAGE_HEADER_BYTES + PATCH_ADDRESS - IMAGE_LOAD_ADDRESS;
    u8 *marker = image + marker_offset;
    if (get_u32(image) != IMAGE_FILE_BYTES || get_u32(image + 4) != IMAGE_LOAD_ADDRESS ||
        get_u32(marker) != PATCH_MAGIC_0 || get_u32(marker + 4) != PATCH_MAGIC_1 ||
        get_u32(marker + 8) != PATCH_PROTOCOL_VERSION) return 0;
    u32 crc = get_u32(marker + 20);
    put_u32(marker + 20, 0);
    u32 actual = ~crc32_update(~0u, image + IMAGE_HEADER_BYTES, IMAGE_PAYLOAD_BYTES);
    put_u32(marker + 20, crc);
    return crc == actual;
}

int install_busy(void) { return stage != 0 || job || reset_pending || sample_transfer_busy(); }

static void discard_stage(void)
{
    u8 *old = stage;
    u32 irq = cpu_irq_save();
    stage = 0; block_open = 0;
    cpu_irq_restore(irq);
    free(old);
}

int install_command(u32 command, u32 sequence, const u8 *p, u32 bytes)
{
    u32 status = OK, offset = 0; u8 out[8]; u32 count = 0;
    if (command > CMD_INSTALL_DIRECT) return 0;
    if (job || reset_pending || sample_transfer_busy()) { reply(command, sequence, BUSY, 0, 0); return 1; }
    touched_at = REG32(TICKS);
    switch (command) {
    case CMD_INSTALL_BEGIN:
        /* u32 CRC for the application; u32 CRC, u32 size for an extension. */
        if (bytes != 4 && bytes != 8) { status = BAD_LENGTH; break; }
        if (stage || boot_mode == BOOT_TRIAL || stock_busy()) { status = BUSY; break; }
        stage_extension = bytes == 8;
        stage_bytes = stage_extension ? get_u32(p + 4) : IMAGE_FILE_BYTES;
        if (!stage_bytes || stage_bytes > EXTENSION_MAX_BYTES) { status = BAD_RANGE; break; }
        target_crc = get_u32(p);
        goto queue;
    case CMD_BLOCK_CRC:
        if (bytes != 4) { status = BAD_LENGTH; break; }
        if (stage) status_show("Receiving update");
        if (!stage || block_open) { status = BAD_STATE; break; }
        offset = get_u32(p);
        if (offset >= stage_bytes || offset % BLOCK_BYTES) { status = BAD_RANGE; break; }
        count = stage_bytes - offset;
        if (count > BLOCK_BYTES) count = BLOCK_BYTES;
        put_u32(out, offset);
        put_u32(out + 4, ~crc32_update(~0u, stage + offset, count));
        count = 8;
        break;
    case CMD_BLOCK_BEGIN:
        if (bytes != 10) { status = BAD_LENGTH; break; }
        if (!stage) { status = BAD_STATE; break; }
        offset = get_u32(p);
        count = get_u16(p + 4);
        if (offset >= stage_bytes || offset % BLOCK_BYTES || !count ||
            count != (stage_bytes - offset < BLOCK_BYTES ? stage_bytes - offset : BLOCK_BYTES)) {
            status = BAD_RANGE; count = 0; break;
        }
        block_offset = offset; block_size = count; block_received = 0;
        block_crc = get_u32(p + 6); block_open = 1; count = 0;
        status_show("Receiving update");
        break;
    case CMD_BLOCK_DATA:
        if (bytes < 5) { status = BAD_LENGTH; break; }
        if (!stage || !block_open) { status = BAD_STATE; break; }
        offset = get_u32(p);
        if (offset != block_offset + block_received || bytes - 4 > block_size - block_received) {
            status = BAD_RANGE; block_open = 0; break;
        }
        memcpy(block + block_received, p + 4, bytes - 4);
        block_received += bytes - 4;
        /* One acknowledgement per checked 4 KiB block. */
        return 1;
    case CMD_BLOCK_END:
        if (bytes != 4) { status = BAD_LENGTH; break; }
        if (!stage || !block_open) { status = BAD_STATE; break; }
        if (get_u32(p) != block_offset || block_received != block_size) { status = BAD_RANGE; break; }
        if (~crc32_update(~0u, block, block_size) != block_crc) { status = BAD_CRC; block_open = 0; break; }
        memcpy(stage + block_offset, block, block_size);
        put_u32(out, block_offset); count = 4; block_open = 0;
        status_show("Receiving update");
        break;
    case CMD_INSTALL_COMMIT:
        if (bytes) { status = BAD_LENGTH; break; }
        if (!stage || block_open) { status = BAD_STATE; break; }
        goto queue;
    case CMD_INSTALL_ABORT:
        if (bytes) { status = BAD_LENGTH; break; }
        goto queue;
    case CMD_INSTALL_DIRECT:
        /* Last resort when trial boot itself is broken: replaces app. */
        if (bytes != 4) { status = BAD_LENGTH; break; }
        if (get_u32(p) != CONFIRM_WORD) { status = BAD_RANGE; break; }
        if (!stage || block_open || boot_mode == BOOT_TRIAL || stage_extension) { status = BAD_STATE; break; }
        goto queue;
    case CMD_PROMOTE:
    case CMD_RESTORE:
    case CMD_REBOOT:
        if (bytes != 4) { status = BAD_LENGTH; break; }
        if (get_u32(p) != CONFIRM_WORD) { status = BAD_RANGE; break; }
        if (stage || stock_busy()) { status = BUSY; break; }
        if ((command == CMD_PROMOTE && boot_mode != BOOT_TRIAL) ||
            (command == CMD_RESTORE && boot_mode == BOOT_TRIAL)) { status = BAD_STATE; break; }
        goto queue;
    default: return 0;
    }
    reply(command, sequence, status, out, count);
    return 1;
queue:
    /* Silence and ignore channel MIDI from the start of any update. An
     * extension install replaces no running application code. */
    if ((command == CMD_INSTALL_BEGIN && !stage_extension) || command == CMD_RESTORE || command == CMD_REBOOT)
        update_enter();
    job_sequence = sequence; MEMORY_BARRIER(); job = command;
    return 1;
}

static int unlink_if_present(const char *path)
{
    int removed = fatfs_unlink(path);
    return removed && removed != FR_NO_FILE;
}

/* Write the checked stage to a temporary file and read it back. */
static int write_verified(const char *path)
{
    if (~crc32_update(~0u, stage, IMAGE_FILE_BYTES) != target_crc) return BAD_CRC;
    if (!image_valid(stage)) return BAD_IMAGE;
    FIL file; u32 done;
    if (fatfs_open(&file, path, FA_WRITE_CREATE_ALWAYS)) return IO_ERROR;
    int status = resource_write_bytes(&file, stage, IMAGE_FILE_BYTES, &done) || done != IMAGE_FILE_BYTES;
    if (!status) status = fatfs_sync_file(&file);
    if (resource_close(&file)) status = 1;
    if (!status) status = read_image(path, stage);
    if (!status && ~crc32_update(~0u, stage, IMAGE_FILE_BYTES) != target_crc) status = 1;
    if (status) { fatfs_unlink(path); return IO_ERROR; }
    return OK;
}

static int persist_trial(void)
{
    int status = write_verified("0:/apptrial.new");
    if (status) return status;
    if (unlink_if_present("0:/apptrial") || fatfs_rename("0:/apptrial.new", "0:/apptrial")) {
        fatfs_unlink("0:/apptrial.new");
        return IO_ERROR;
    }
    return OK;
}

static int persist_direct(void)
{
    int status = write_verified("0:/appnew");
    if (status) return status;
    /* A pending trial would otherwise run over the new app at next boot. */
    if (unlink_if_present("0:/apptrial") || unlink_if_present("0:/appold") ||
        fatfs_rename("0:/app", "0:/appold")) {
        fatfs_unlink("0:/appnew");
        return IO_ERROR;
    }
    if (fatfs_rename("0:/appnew", "0:/app")) {
        fatfs_rename("0:/appold", "0:/app");
        return IO_ERROR;
    }
    return OK;
}

static int promote(void)
{
    u8 *image = malloc(IMAGE_FILE_BYTES);
    if (!image) return NO_MEMORY;
    int status = read_image("0:/apptrial.run", image);
    if (!status && (!image_valid(image) || memcmp(image + 8 + PATCH_ADDRESS - IMAGE_LOAD_ADDRESS + 12,
                                               patch_identity.build_id, 8))) status = BAD_IMAGE;
    free(image);
    if (status) return status;
    if (unlink_if_present("0:/appold") || fatfs_rename("0:/app", "0:/appold")) return IO_ERROR;
    if (fatfs_rename("0:/apptrial.run", "0:/app")) {
        fatfs_rename("0:/appold", "0:/app");
        return IO_ERROR;
    }
    watchdog_stop(); boot_mode = BOOT_STABLE;
    return OK;
}

static int restore(void)
{
    u8 *image = malloc(IMAGE_FILE_BYTES);
    if (!image) return NO_MEMORY;
    int status = read_image("0:/appold", image);
    if (!status && !image_valid(image)) status = BAD_IMAGE;
    free(image);
    if (status) return status;
    if (unlink_if_present("0:/apptrial")) return IO_ERROR;
    /* Never remove an existing swap: it can be evidence of interrupted
     * recovery. FatFs rename refuses existing destinations. */
    if (fatfs_rename("0:/app", "0:/appswap")) return IO_ERROR;
    if (fatfs_rename("0:/appold", "0:/app")) {
        fatfs_rename("0:/appswap", "0:/app"); return IO_ERROR;
    }
    if (fatfs_rename("0:/appswap", "0:/appold")) {
        if (!fatfs_rename("0:/app", "0:/appold"))
            fatfs_rename("0:/appswap", "0:/app");
        return IO_ERROR;
    }
    return OK;
}

void install_foreground_poll(void)
{
    if (reset_pending) {
        /* Retry while the reply drains or stock work finishes; give up after
         * REBOOT_RETRY_TICKS so a stuck service cannot hold updates forever. */
        if (update_silent() && sysex_tx_idle() && !stock_busy() && storage_claim()) cold_reset();
        if ((u32)(REG32(TICKS) - reset_requested_at) > REBOOT_RETRY_TICKS) {
            reset_pending = 0; counters.last_job_result = BUSY; update_leave();
        }
        return;
    }
    u32 command = job;
    if (!command) {
        /* An abandoned update restores audio and MIDI. */
        if (update_active() && !stage && (u32)(REG32(TICKS) - touched_at) > STAGE_EXPIRY_TICKS)
            update_leave();
        /* An abandoned transfer must not retain a large heap allocation. */
        if (!stage) return;
        u32 irq = cpu_irq_save();
        int expired = !job && stage && (u32)(REG32(TICKS) - touched_at) > STAGE_EXPIRY_TICKS;
        if (expired) job = CMD_INSTALL_ABORT;
        cpu_irq_restore(irq);
        if (expired) { discard_stage(); job = 0; update_leave(); }
        return;
    }
    u32 sequence = job_sequence, status = OK;
    if (!storage_claim()) status = BUSY;
    else {
        switch (command) {
        case CMD_INSTALL_BEGIN: {
            u8 *image = malloc(stage_bytes);
            if (!image) { status = NO_MEMORY; break; }
            if (stage_extension) { extension_stage_seed(image, stage_bytes); stage = image; break; }
            status = read_image("0:/app", image);
            if (!status && !image_valid(image)) status = BAD_IMAGE;
            if (!status) { stage = image; status_show("Receiving update"); }
            else free(image);
            break;
        }
        case CMD_INSTALL_COMMIT:
            status_show("Saving update");
            status = stage_extension ? extension_persist(stage, stage_bytes, target_crc) : persist_trial();
            if (status) status_show("Update not saved");
            discard_stage();
            break;
        case CMD_INSTALL_DIRECT:
            status_show("Saving update");
            status = persist_direct();
            if (status) status_show("Update not saved");
            discard_stage();
            break;
        case CMD_INSTALL_ABORT: discard_stage(); update_leave(); break;
        case CMD_PROMOTE:
            status_show("Installing update");
            status = promote();
            status_show(status ? "Update not installed" : "Update installed");
            break;
        case CMD_RESTORE:
            status_show("Restoring previous version");
            status = restore();
            if (status) status_show("Previous version not restored");
            break;
        case CMD_REBOOT: status_show("Restarting"); reset_requested_at = REG32(TICKS); reset_pending = 1; break;
        }
        storage_release();
    }
    counters.last_job_result = status;
    if (status && command != CMD_PROMOTE) update_leave(); /* the update did not proceed */
    /* Keep the job occupied through reply publication: panel requests cannot
     * overtake its completion. */
    reply(command, sequence, status, 0, 0);
    MEMORY_BARRIER(); job = 0;
}
