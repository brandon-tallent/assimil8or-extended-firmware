#include "patch.h"
#include "panel_page.h"

/* Second code image: larger features live in 0:/ext-<build ID>.bin, loaded
 * at startup into reserved DDR beside the trial handoff. The image must name
 * the running overlay's build ID (it calls overlay and stock code at fixed
 * addresses) and pass its CRC. Its commands arrive through commands_handle;
 * its foreground poll runs every pass. Installing an image for the running
 * build reloads it in place.
 *
 * LOAD SOFTWARE copies only `app` from the front card. A build that finds no
 * image of its own on the boot volume takes the file of the same name from
 * the front card's root and keeps a copy on the boot volume. */

static u32 state, reload;
void (*volatile patch_grain_hook)(s32 *mix, const s32 *steps);
s32 patch_grain_steps[AUDIO_CHANNELS], patch_grain_mix[AUDIO_CHANNELS];
const struct panel_page *volatile patch_panel_page;
const struct panel_page *volatile patch_envelope_page;
const struct panel_page *volatile patch_pitch_page;
const struct panel_page *volatile patch_mix_page;
void (*volatile patch_channel_fx_hook)(s32 *channels);
void (*volatile patch_mix_fx_hook)(s32 *mix);
s32 patch_mix_frame[4];
s32 patch_input_frame[2];
s32 patch_fader_out[AUDIO_CHANNELS];
void (*volatile patch_record_copied)(void *to, const void *from, u32 bytes);
static struct extension_header *loaded;

int extension_persist(u8 *image, u32 bytes, u32 crc);
static void path_for(char *path, const u8 *build_id, const char *suffix)
{
    static const char hex[] = "0123456789abcdef";
    memcpy(path, EXTENSION_PATH_PREFIX, sizeof(EXTENSION_PATH_PREFIX) - 1);
    char *at = path + sizeof(EXTENSION_PATH_PREFIX) - 1;
    for (u32 i = 0; i < sizeof(patch_identity.build_id); ++i) {
        *at++ = hex[build_id[i] >> 4]; *at++ = hex[build_id[i] & 15u];
    }
    while ((*at++ = *suffix++)) {}
}

int extension_image_valid(u8 *image, u32 bytes)
{
    struct extension_header *h = (struct extension_header *)image;
    if (bytes < sizeof(*h) || bytes > EXTENSION_MAX_BYTES || h->magic[0] != PATCH_MAGIC_0 ||
        h->magic[1] != EXTENSION_MAGIC || h->file_bytes != bytes ||
        h->bss_bytes > EXTENSION_MAX_BYTES - bytes) return 0;
    u32 crc = h->crc;
    h->crc = 0;
    u32 actual = ~crc32_update(~0u, image, bytes);
    h->crc = crc;
    return crc == actual;
}

/* Make freshly written code visible to instruction fetch: clean L1 data
 * lines to the point of unification (L2), then invalidate the instruction
 * cache and branch predictor (Cortex-A8 TRM, CP15 c7). */
static void code_sync(u32 address, u32 bytes)
{
    for (u32 line = address & ~(CACHE_LINE_BYTES - 1u); line < address + bytes; line += CACHE_LINE_BYTES)
        __asm__ volatile("mcr p15, 0, %0, c7, c11, 1" :: "r"(line) : "memory");
    __asm__ volatile("dsb\n"
                     "mcr p15, 0, %0, c7, c5, 0\n"
                     "mcr p15, 0, %0, c7, c5, 6\n"
                     "dsb\nisb" :: "r"(0) : "memory");
}

/* Read this build's image from a volume into place and check it. */
static u32 read_from(char volume, u32 *size)
{
    char path[EXTENSION_PATH_BYTES];
    path_for(path, patch_identity.build_id, ".bin");
    path[0] = volume;
    FIL file; u32 done = 0;
    if (fatfs_open(&file, path, FA_READ)) return EXTENSION_MISSING;
    u32 bytes = file.words[FIL_FILE_SIZE / 4], status = EXTENSION_LOADED;
    u8 *image = (u8 *)EXTENSION_BASE;
    if (bytes < sizeof(struct extension_header) || bytes > EXTENSION_MAX_BYTES) status = EXTENSION_INVALID;
    else if (resource_read_bytes(&file, image, bytes, &done) || done != bytes) status = EXTENSION_IO_ERROR;
    if (resource_close(&file) && status == EXTENSION_LOADED) status = EXTENSION_IO_ERROR;
    if (status != EXTENSION_LOADED) return status;
    if (!extension_image_valid(image, bytes) ||
        memcmp(((struct extension_header *)image)->build_id, patch_identity.build_id,
               sizeof(patch_identity.build_id))) return EXTENSION_INVALID;
    *size = bytes;
    return EXTENSION_LOADED;
}

static u32 load(void)
{
    u8 *image = (u8 *)EXTENSION_BASE;
    u32 bytes = 0, status = read_from(EXTENSION_BOOT_VOLUME, &bytes);
    if (status == EXTENSION_MISSING) {
        status = read_from(EXTENSION_CARD_VOLUME, &bytes);
        if (status != EXTENSION_LOADED) return status;
        /* The copy is read back into place; without it the card's serves
         * this boot and the next tries again. */
        if (!extension_persist(image, bytes, ~crc32_update(~0u, image, bytes))) status_show("Extension installed");
        else if (read_from(EXTENSION_CARD_VOLUME, &bytes) != EXTENSION_LOADED) return EXTENSION_IO_ERROR;
        reload = 0; /* this load is the one a saved image asks for */
    }
    if (status != EXTENSION_LOADED) return status;
    struct extension_header *h = (struct extension_header *)image;
    memset(image + bytes, 0, h->bss_bytes);
    code_sync(EXTENSION_BASE, bytes + h->bss_bytes);
    h->init();
    MEMORY_BARRIER();
    loaded = h;
    return EXTENSION_LOADED;
}

void extension_foreground_poll(void)
{
    if (state && !reload) {
        if (loaded) loaded->foreground();
        return;
    }
    if (!BYTE(TASK_EXEC_STARTED)) return;
    /* A request queued before the reload was asked for is finished first. */
    if (loaded && loaded->busy()) { loaded->foreground(); return; }
    if (!storage_claim()) return;
    u32 irq = cpu_irq_save();
    loaded = 0; /* panel commands see no extension while it is replaced */
    patch_grain_hook = 0; /* no audio or panel IRQ is inside it while IRQs are masked */
    patch_panel_page = 0;
    patch_envelope_page = 0;
    patch_pitch_page = 0;
    patch_mix_page = 0;
    patch_channel_fx_hook = 0;
    patch_mix_fx_hook = 0;
    patch_record_copied = 0;
    cpu_irq_restore(irq);
    reload = 0;
    state = load();
    storage_release();
}

void extension_reload(void) { reload = 1; }
int extension_missing(void) { return state == EXTENSION_MISSING; }

int extension_command(u32 command, u32 sequence, const u8 *payload, u32 bytes)
{
    if (command < CMD_EXTENSION_FIRST || command > CMD_EXTENSION_LAST) return 0;
    if (loaded) loaded->command(command, sequence, payload, bytes);
    else reply(command, sequence, reload ? BUSY : BAD_COMMAND, 0, 0);
    return 1;
}

u32 extension_identity(u8 *out)
{
    put_u32(out, loaded ? loaded->crc : 0);
    out[4] = (u8)state;
    return 5;
}

/* Install target: write a checked image for any build, replacing that
 * build's file; reload when it belongs to the running build. */
int extension_persist(u8 *image, u32 bytes, u32 crc)
{
    if (~crc32_update(~0u, image, bytes) != crc) return BAD_CRC;
    if (!extension_image_valid(image, bytes)) return BAD_IMAGE;
    const u8 *build_id = ((struct extension_header *)image)->build_id;
    char path[EXTENSION_PATH_BYTES], temporary[EXTENSION_PATH_BYTES];
    path_for(path, build_id, ".bin");
    path_for(temporary, build_id, ".new");
    FIL file; u32 done;
    if (fatfs_open(&file, temporary, FA_WRITE_CREATE_ALWAYS)) return IO_ERROR;
    int failed = resource_write_bytes(&file, image, bytes, &done) || done != bytes;
    if (!failed) failed = fatfs_sync_file(&file);
    if (resource_close(&file)) failed = 1;
    if (!failed) {
        failed = fatfs_open(&file, temporary, FA_READ);
        if (!failed) {
            failed = file.words[FIL_FILE_SIZE / 4] != bytes ||
                resource_read_bytes(&file, image, bytes, &done) || done != bytes;
            if (resource_close(&file)) failed = 1;
            if (!failed) failed = ~crc32_update(~0u, image, bytes) != crc;
        }
    }
    if (!failed) {
        int removed = fatfs_unlink(path);
        failed = (removed && removed != FR_NO_FILE) || fatfs_rename(temporary, path);
    }
    if (failed) { fatfs_unlink(temporary); return IO_ERROR; }
    if (!memcmp(build_id, patch_identity.build_id, sizeof(patch_identity.build_id))) extension_reload();
    return OK;
}

/* A new stage starts from the running image, so an unchanged build
 * transfers only the blocks that differ. */
void extension_stage_seed(u8 *stage, u32 bytes)
{
    memset(stage, 0, bytes);
    if (loaded) memcpy(stage, loaded, loaded->file_bytes < bytes ? loaded->file_bytes : bytes);
}
