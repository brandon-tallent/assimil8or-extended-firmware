#include "patch.h"

/* Sector cache for folder loads. A load lists the folder once per file and
 * opens each file by name, so the stock code reads the same directory and
 * FAT sectors again and again; while a load runs, one-sector reads are kept
 * here (direct-mapped by sector number). Multi-sector reads, the WAV data,
 * pass straight through. Writes drop what they replace. */
struct sector_tag { u32 sector, drive; }; /* drive + 1; 0 marks an empty slot */
#define TAGS  ((struct sector_tag *)SECTOR_CACHE_BASE)
#define SLOTS ((u32 *)(SECTOR_CACHE_BASE + SECTOR_CACHE_SLOTS * sizeof(struct sector_tag)))
#define SECTOR_WORDS (SECTOR_BYTES / 4u)

extern u32 folder_reload_presets_and_samples(const char *folder, u32 *loaded, void *progress);
static u32 caching;

static void copy_sector(void *to, const void *from)
{
    if (((u32)to | (u32)from) & 3u) { memcpy(to, from, SECTOR_BYTES); return; }
    u32 *d = to; const u32 *s = from;
    for (u32 n = 0; n < SECTOR_WORDS; ++n) d[n] = s[n];
}

u32 sector_cache_read(u32 drive, void *buffer, u32 sector, u32 count)
{
    u32 slot = sector & (SECTOR_CACHE_SLOTS - 1u);
    if (!caching || count != 1 || TAGS[slot].drive != drive + 1u || TAGS[slot].sector != sector) return 0;
    copy_sector(buffer, SLOTS + slot * SECTOR_WORDS);
    return 1;
}

void sector_cache_fill(u32 drive, const void *buffer, u32 sector, u32 count)
{
    u32 slot = sector & (SECTOR_CACHE_SLOTS - 1u);
    if (!caching || count != 1) return;
    copy_sector(SLOTS + slot * SECTOR_WORDS, buffer);
    TAGS[slot].sector = sector;
    TAGS[slot].drive = drive + 1u;
}

void sector_cache_forget(u32 drive, const void *buffer, u32 sector, u32 count)
{
    (void)buffer;
    if (!caching) return;
    for (u32 n = 0; n < count && n < SECTOR_CACHE_SLOTS; ++n) {
        struct sector_tag *tag = &TAGS[(sector + n) & (SECTOR_CACHE_SLOTS - 1u)];
        if (tag->drive == drive + 1u && tag->sector == sector + n) tag->drive = 0;
    }
}

u32 patch_folder_load(const char *folder, u32 *loaded, void *progress)
{
    memset(TAGS, 0, SECTOR_CACHE_SLOTS * sizeof(struct sector_tag));
    caching = 1;
    u32 result = folder_reload_presets_and_samples(folder, loaded, progress);
    caching = 0;
    return result;
}
