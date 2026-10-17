/* Assimil8or patch: shared declarations.
 *
 * The patch is an overlay on the vendor v3.04 payload. Stock functions are
 * called at their fixed addresses (stock_abi.ld); the hardware and stock
 * data addresses below are that image's.
 */
#ifndef PATCH_H
#define PATCH_H

#include "trace_stages.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed int s32;
typedef signed short s16;
typedef unsigned int size_t;

#define REG32(address) (*(volatile u32 *)(address))

/* Vendor image layout. */
#define IMAGE_LOAD_ADDRESS        0x80000000u
#define IMAGE_PAYLOAD_BYTES       0x0009bac8u
#define IMAGE_FILE_BYTES          (IMAGE_PAYLOAD_BYTES + 8u)
#define IMAGE_HEADER_BYTES        8u

/* Identity record at the start of the patch region; tools/build_patch.py
 * fills build_id and image_crc. */
#define PATCH_MAGIC_0             0x38415153u /* "SQA8" */
#define PATCH_MAGIC_1             0x48435450u /* "PTCH" */
#define PATCH_PROTOCOL_VERSION    1u

struct patch_identity {
    u32 magic[2];
    u32 protocol;
    u8 build_id[8];
    u32 image_crc; /* CRC-32 of the payload with this field zeroed */
};
extern const struct patch_identity patch_identity;

/* Trial handoff and trampoline in DDR between the stacks above the heap
 * ceiling and the startup tables near 0x9bff0000. No stock code references
 * 0x9b000000..0x9b0fffff. The trampoline must be in DDR: the stock MMU maps
 * OCMC execute-never, and the jump happens before the MMU is disabled. */
#define HANDOFF_BASE              0x9b000000u
#define TRAMPOLINE_BASE           0x9b000100u
#define HANDOFF_TRIAL_MAGIC       0x4c415254u /* "TRAL" */
#define TRACE_BASE                (HANDOFF_BASE + 0x40u) /* one cache line */
#define TRACE_LINE_BYTES          64u
#define TRACE_LOG_STAGES          32u
#define TRACE_HEARTBEAT_MASK      4095u /* renderer calls between samples */

/* The patch's zeroed storage (patch.ld), between the trampoline and the
 * second code image. It is not part of the image: the reset entry clears
 * it before any other patch code runs. */
#define PATCH_STORAGE_BASE        0x9b001000u
extern u8 patch_storage_start[], patch_storage_end[];

/* Second code image (extension.c): linked at EXTENSION_BASE inside the
 * reserved 0x9b000000 range, which no stock code addresses; the stock DSP
 * default tables begin near 0x9bffef7e. */
#define EXTENSION_BASE            0x9b010000u
#define EXTENSION_MAX_BYTES       0x000f0000u /* image plus BSS, to 0x9b100000 */
#define EXTENSION_MAGIC           0x31545845u /* "EXT1" after "SQA8" */
#define EXTENSION_PATH_PREFIX     "0:/ext-"
#define EXTENSION_BOOT_VOLUME     '0'
#define EXTENSION_CARD_VOLUME     '1'         /* the front panel's card */
#define EXTENSION_PATH_BYTES      32u
#define CACHE_LINE_BYTES          64u         /* Cortex-A8 L1 */
enum { EXTENSION_UNTRIED, EXTENSION_LOADED, EXTENSION_MISSING, EXTENSION_INVALID, EXTENSION_IO_ERROR };
/* Grain mode (grain.S, ext/grains.c): the renderer records each channel's
 * pitch step every frame, a signed Q25 count of sample frames per output
 * frame, and while a hook is set it hands the hook the eight rendered Q31
 * samples before channel effects, to replace any of them. */
#define AUDIO_CHANNELS            8u
#define AUDIO_STEP_SHIFT          25u
extern void (*volatile patch_grain_hook)(s32 *mix, const s32 *steps);
extern s32 patch_grain_steps[AUDIO_CHANNELS], patch_grain_mix[AUDIO_CHANNELS];
/* Grain mode's settings: its fields in wire order and a CV route for eight
 * of them. They are a preset's (preset_extras.c) and live in the overlay, so
 * a preset keeps them whether or not the extension that plays them is
 * loaded; patch_grain_loaded counts the times other settings replaced them. */
enum { GRAIN_ON, GRAIN_SIZE, GRAIN_OVERLAP, GRAIN_SPRAY, GRAIN_DETUNE, GRAIN_OCTAVE, GRAIN_REVERSE,
       GRAIN_PAN, GRAIN_SHAPE, GRAIN_SCAN, GRAIN_SLIDE, GRAIN_SKEW, GRAIN_FIELDS };
enum { GRAIN_ROUTE_SIZE, GRAIN_ROUTE_OVERLAP, GRAIN_ROUTE_SPRAY, GRAIN_ROUTE_DETUNE, GRAIN_ROUTE_SLIDE,
       GRAIN_ROUTE_SCAN, GRAIN_ROUTE_SHAPE, GRAIN_ROUTE_PAN, GRAIN_ROUTES };
struct grain_settings {
    s16 value[GRAIN_FIELDS]; /* value[GRAIN_ON]: the channel that plays grains, from 1; 0 off */
    signed char source[GRAIN_ROUTES], amount[GRAIN_ROUTES]; /* as an EQ route's (eq.h) */
};
struct grain_range { s16 low, high, initial; };
extern const struct grain_range grain_ranges[GRAIN_FIELDS];
extern u32 patch_grain_loaded;
/* Resampling (eq.S, grain.S, record.c): each frame keeps the eight channels'
 * outputs, after level, envelope, Bits and EQ. While patch_resample_source
 * points at one, the stock 48 kHz capture stage records it and the word
 * after it (a Stereo/Right partner) in place of the Sample input. The
 * extension asks for the next Arm to record a channel (1-8) and names its
 * take. */
extern s32 patch_channel_out[AUDIO_CHANNELS + 1];
extern const s32 *volatile patch_resample_source;
extern u32 patch_take_source;
extern const char *patch_take_name;

struct extension_header {
    u32 magic[2];
    u8 build_id[8];   /* overlay this image links against */
    u32 file_bytes;
    u32 bss_bytes;    /* zeroed after the image */
    u32 crc;          /* CRC-32 of the file with this field zero */
    void (*init)(void);
    void (*command)(u32 command, u32 sequence, const u8 *payload, u32 bytes);
    void (*foreground)(void);
    int (*busy)(void);
};

struct handoff {
    u32 magic;
    u8 build_id[8];
};

/* Boot modes reported by Identify. */
#define BOOT_STABLE               0u
#define BOOT_TRIAL                1u

/* AM335x registers. */
#define UART0_BASE                0x44e09000u
#define UART_THR                  0x00u
#define UART_LSR                  0x14u
#define UART_LSR_TX_EMPTY         0x40u
#define UART_TX_FIFO_LVL          0x68u
#define UART_FIFO_DEPTH           64u

#define PRM_DEVICE_BASE           0x44e00f00u
#define PRM_RSTCTRL               0x00u
#define PRM_RSTCTRL_COLD_SW       0x02u
#define PRM_RSTST                 0x08u
#define PRM_RSTST_WDT1            0x10u

#define CM_WKUP_BASE              0x44e00400u
#define CM_WKUP_WDT1_CLKCTRL      0xd4u
#define CLKCTRL_MODULEMODE_ENABLE 0x2u
#define CLKCTRL_IDLEST_MASK       0x30000u

#define WDT1_BASE                 0x44e35000u
#define WDT_WCLR                  0x24u
#define WDT_WCRR                  0x28u
#define WDT_WLDR                  0x2cu
#define WDT_WTGR                  0x30u
#define WDT_WWPS                  0x34u
#define WDT_WSPR                  0x48u
#define WDT_WSPR_DISABLE_FIRST    0xaaaau
#define WDT_WSPR_DISABLE_SECOND   0x5555u
#define WDT_WSPR_ENABLE_FIRST     0xbbbbu
#define WDT_WSPR_ENABLE_SECOND    0x4444u
#define WDT_PENDING_WCLR          0x01u
#define WDT_PENDING_WCRR          0x02u
#define WDT_PENDING_WLDR          0x04u
#define WDT_PENDING_WTGR          0x08u
#define WDT_PENDING_WSPR          0x10u
#define WDT_CLOCK_HZ              32768u
#define WDT_TRIAL_SECONDS         20u /* boot to first foreground pass takes seconds */

/* Producers stopped before a chain-load (TRM SPRUH73Q register offsets). */
#define MCASP0_CFG_BASE           0x48038000u
#define MCASP1_CFG_BASE           0x4803c000u
#define MCASP_GBLCTL              0x44u
#define MCSPI0_BASE               0x48030000u
#define MCSPI1_BASE               0x481a0000u
#define MCSPI_CH0CTRL             0x134u
#define TSCADC_BASE               0x44e0d000u
#define TSCADC_CTRL               0x40u
#define TSCADC_CTRL_ENABLE        0x01u
#define EDMA_CC_BASE              0x49000000u
#define EDMA_SHADOW_REGION0       (EDMA_CC_BASE + 0x2000u)
#define EDMA_EECR                 0x28u
#define EDMA_EECRH                0x2cu
#define EDMA_QEECR                0x88u
#define EDMA_CCSTAT               0x640u
#define EDMA_CCSTAT_ACTIVE        0x10u
#define EDMA_TPTC0_BASE           0x49800000u
#define EDMA_TPTC1_BASE           0x49900000u
#define EDMA_TPTC2_BASE           0x49a00000u
#define EDMA_TPTC_TCSTAT          0x100u
#define EDMA_TCSTAT_BUSY_MASK     0x77u /* PROGBUSY, SRCACTV, WSACTV, DSTACTV */

/* OLED shadow image and display task. */
#define OLED_SHADOW_FRAMEBUFFER_BASE 0x40308000u
#define OLED_SIDE                 128u
#define OLED_FRAMEBUFFER_BYTES    (OLED_SIDE * OLED_SIDE * 2u)
#define SCREENSHOT_LITERAL_PIXELS 128u
#define SCREENSHOT_ENCODED_BYTES  (OLED_FRAMEBUFFER_BYTES + OLED_SIDE * OLED_SIDE / SCREENSHOT_LITERAL_PIXELS)
#define OLED_TRANSFER_KIND_RECT   1u
#define OLED_PIXEL_MASK           0xffffu
#define OLED_PIXEL_WHITE          0xffffu
#define OLED_PIXEL_BLACK          0x0000u
#define CDISPLAY_TASK_OBJECT      0x800b1e58u
#define UI_PAGE_RENDER_PREVIOUS_PAGE 0x38u
#define BYTE_MASK                 0xffu
#define RGB565_RED_SHIFT          11u
#define RGB565_GREEN_SHIFT        5u
#define RGB565_GREEN_MASK         0x3fu
#define RGB565_BLUE_MASK          0x1fu
#define LUMA_RED_WEIGHT           10u /* 0.299 per 1/31 */
#define LUMA_GREEN_WEIGHT         10u /* 0.587 per 1/63 */
#define LUMA_BLUE_WEIGHT          4u  /* 0.114 per 1/31 */
enum display_mode { DISPLAY_MODE_STOCK, DISPLAY_MODE_TEXT_WHITE, DISPLAY_MODE_ALL_WHITE };

/* Front panel: 28 button records. The encoder position is a four-bit count
 * the audio IRQ's quadrature decoder maintains and the panel dispatcher
 * diffs. */
#define FRONT_PANEL_STATE         0x80236f20u
#define FRONT_PANEL_RECORD_SIZE   0x50u
#define FRONT_PANEL_RECORD_COUNT  28u
#define PANEL_ENCODER_POSITION    0x80348004u
#define PANEL_ENCODER_POSITION_MASK 0x0fu
#define PANEL_ENCODER_STEP_TICKS  40u   /* beyond the 27-tick acceleration window */
#define PANEL_SETTLE_TICKS        60u   /* debounce (20) plus callback time */
#define PANEL_HOLD_MAX_TICKS      5000u
#define PANEL_STEPS_MAX           64

/* Utilities > Preferences. */
#define UI_SCREEN_CONTEXT         0x800db570u
#define UI_SCREEN_STATE_WORD      0u /* word indices into the screen record */
#define UI_SCREEN_CONFIRM_WORD    1u
#define UI_SCREEN_ENCODER_WORD    2u
#define UI_SCREEN_TEXT_LINE       0x10u
#define UI_SCREEN_CONTENT_Y_ORIGIN 0x14u
#define UI_SCREEN_CONTENT_ROW_PITCH 0x18u
#define UI_SCREEN_PENDING_CONTROL_VALUE 0x190u /* DATA 2 knob, float 0..1 */
#define UI_SCREEN_COMMITTED_CONTROL_VALUE 0x194u
#define UI_SCREEN_PREFERENCES_CURSOR 0x198u
#define FLOAT_ONE_HALF_BITS       0x3f000000u /* 0.5f; positive floats order as integers */
#define PREFERENCES_STATE_DRAW    0u
#define PREFERENCES_STATE_BROWSE  1u
#define PREFERENCES_STATE_EDIT_BASE 10u
#define PREFERENCES_STATE_SAVE    30u
#define PREFERENCES_ROW_BRIGHTNESS    0u
#define PREFERENCES_ROW_SCREEN_SAVER  1u
#define PREFERENCES_ROW_GATE_DELAY    2u
#define PREFERENCES_ROW_AUTO_AUDITION 3u
#define PREFERENCES_ROW_LOAD_WARNING  4u
#define PREFERENCES_ROW_HIGH_CONTRAST 5u
#define PREFERENCES_STATE_EDIT_HIGH_CONTRAST (PREFERENCES_STATE_EDIT_BASE + PREFERENCES_ROW_HIGH_CONTRAST)
#define PREFERENCES_HIGH_CONTRAST_LINE 6u /* content lines below the origin */
#define PREFERENCES_LINES_BELOW_HIGH_CONTRAST 2u /* its row and the exit prompt */
#define PREFERENCES_RUNTIME_STATE 0x80240e70u
#define PREFERENCES_RUNTIME_HIGH_CONTRAST 5u /* stock reserved byte */
#define PREFERENCES_VALUE_FRAME_COLOR 0x18c6u
#define PREFERENCES_VALUE_FRAME_Y_BACK 1u
#define PREFERENCES_ON_OFF_FRAME_X 102u /* PREFERENCES_AUTO_AUDITION_FRAME_X */
#define PREFERENCES_ON_OFF_FRAME_WIDTH 24u
#define UI_RENDER_DEFAULT_FONT_CONTEXT 0x800ac1f4u
#define UI_FONT_CONTEXT_BITMAP_ROWS 0x04u
#define PREFS_PATH                "0:/prefs.yml"
#define PREFS_FILE_MAX_BYTES      1024u

/* Stock status band (UI_OPERATION_STATUS_*). */
#define UI_OPERATION_STATUS_STATE 0x800ac350u
#define UI_OPERATION_STATUS_MODE  0x98u /* byte */
#define UI_OPERATION_STATUS_MODE_IDLE  0u
#define UI_OPERATION_STATUS_MODE_TIMED 2u
#define UI_STATUS_TEXT_COLOR      198u /* UI_OPERATION_STATUS_DEFAULT_TEXT_COLOR */
#define STATUS_MESSAGE_TICKS      10000 /* outlasts the gap between block refreshes */
#define STATUS_NOTICE_START_TICKS 3000u /* let the stock UI settle after boot */
#define STATUS_NOTICE_REFRESH_TICKS 5000u

/* DMTIMER2/3, left running by a chain-loading app. */
#define DMTIMER2_BASE             0x48040000u
#define DMTIMER3_BASE             0x48042000u
#define DMTIMER_IRQSTATUS         0x28u
#define DMTIMER_IRQENABLE_CLR     0x30u
#define DMTIMER_TCLR              0x38u
#define DMTIMER_TWPS              0x48u
#define DMTIMER_TWPS_TCLR         0x01u
#define DMTIMER_IRQ_ALL           0x07u /* match, overflow, capture */

/* Interrupt controller: 128 lines in four 32-line mask banks. */
#define INTC_BASE                 0x48200000u
#define INTC_SYSCONFIG            0x10u
#define INTC_SYSCONFIG_SOFTRESET  0x02u
#define INTC_SYSSTATUS            0x14u
#define INTC_SYSSTATUS_RESETDONE  0x01u
#define INTC_CONTROL              0x48u
#define INTC_CONTROL_NEW_AGREEMENT 0x03u /* NEWIRQAGR | NEWFIQAGR */
#define INTC_MIR_SET0             0x8cu
#define INTC_MIR_BANK_STRIDE      0x20u
#define INTC_MIR_BANKS            4u

/* Stock memory map. */
#define DDR_BASE                  0x80000000u
#define DDR_END                   0xa0000000u
#define OCMC_BASE                 0x40300000u
#define OCMC_END                  0x40310000u
#define STOCK_BSS_START           0x8009c000u
#define MMU_L1_TABLE_BASE         0x80344000u
#define HEAP_ALLOCATION_START     0x80349c90u
#define HEAP_ALLOCATION_END_LIMIT 0x9a949c90u

/* Stock state read by diagnostics and busy checks. */
#define HEAP_BREAK_POINTER        0x8009c038u
#define UI_ASYNC_OPERATION        0x800ac184u
#define UI_ASYNC_STATE            0x00u
#define UI_ASYNC_OWNER_LEVEL      0x04u
#define UI_ASYNC_PENDING_LEVEL    0x08u
#define UI_ASYNC_CLAIM_LEVEL_UI   2
#define SAMPLE_INPUT_RUNTIME_MODE_ADDRESS (0x802408c8u + 0x404u)
#define SAMPLE_INPUT_MODE_CAPTURE_ACTIVE 2u

/* Stock functions (stock.ld). */
int uart0_read_byte_nonblocking(void);
void uart0_module_clock_and_bootstrap(void);
void *malloc(size_t bytes);
void free(void *block);

/* FatFs R0.10 tiny: descriptor 0x24 bytes; file size at +0x0c. */
#define FIL_BYTES                 0x40u
#define FIL_FILE_SIZE             0x0cu
#define FA_READ                   0x01u
#define FA_WRITE_CREATE_ALWAYS    0x0au
#define FA_WRITE                  0x02u /* existing file */
typedef struct { u32 words[FIL_BYTES / 4]; } FIL;
int fatfs_open(FIL *file, const char *path, u32 mode);
int resource_read_bytes(FIL *file, void *buffer, u32 bytes, u32 *done);
int resource_write_bytes(FIL *file, const void *buffer, u32 bytes, u32 *done);
int fatfs_sync_file(FIL *file);
int resource_close(FIL *file);
int fatfs_unlink(const char *path);
int fatfs_rename(const char *from, const char *to);
int resource_seek_absolute(FIL *file, u32 offset);
#define FR_NO_FILE                4

/* CPU helpers (entry.S and boot.c). */
u32 cpu_irq_save(void);
void cpu_irq_restore(u32 state);
u32 cpu_cycles(void);
void cpu_cycles_enable(void);
void cpu_chain_load(const void *payload, u32 bytes);

/* Local freestanding runtime (runtime.c); the compiler may emit calls. */
void *memcpy(void *to, const void *from, size_t bytes);
void *memset(void *to, int value, size_t bytes);
int memcmp(const void *a, const void *b, size_t bytes);
u32 crc32_update(u32 crc, const void *data, u32 bytes);

/* Diagnostics counters, in wire order. */
#define RENDER_LOAD_BINS          8u
#define AUDIO_FRAME_BUDGET_CYCLES 20800u
struct counters {
    u32 uptime_ticks;
    u32 panel_polls;
    u32 foreground_polls;
    u32 rx_bytes;
    u32 rx_sysex_frames;
    u32 rx_sysex_rejects;
    u32 rx_sysex_overflows;
    u32 rx_stray_data;
    u32 tx_sysex_bytes;
    u32 tx_stock_bytes;
    u32 tx_frame_drops;
    u32 render_calls;
    u32 render_cycles_last;
    u32 render_cycles_min;
    u32 render_cycles_max;
    u32 render_interval_last;
    u32 render_interval_min;
    u32 render_interval_max;
    u32 render_over_budget; /* renderer >= 20,800 cycles, not a deadline-miss detector */
    u32 render_load[RENDER_LOAD_BINS];
    u32 wdt_kicks;
    u32 heap_break;
    u32 reset_status;
    u32 last_job_result;
};
extern struct counters counters;
_Static_assert(sizeof(struct patch_identity) == 24, "identity wire layout");
_Static_assert(sizeof(struct counters) == 124, "diagnostics wire layout");

#define PATCH_ADDRESS             0x8006e4f8u
#define TICKS                     0x80240ce8u /* SYSTEM_MONOTONIC_TICKS, ~1.04 ms */
#define TASK_EXEC_CONTEXT         0x80096c28u
#define TASK_EXEC_FOREGROUND      1u
#define TASK_EXEC_STARTED         0x80240ce0u

/* MIDI bytes and the stock receiver/manager layout. */
#define MIDI_STATUS_BIT           0x80u
#define MIDI_SYSTEM_STATUS        0xf0u
#define MIDI_SYSEX_START          0xf0u
#define MIDI_SYSEX_END            0xf7u
#define MIDI_REALTIME_FIRST       0xf8u
#define MIDI_CHANNEL_MASK         0x0fu
#define MIDI_STATUS_NIBBLE_SHIFT  4u
#define MIDI_STATUS_NIBBLE_FIRST  8u
#define MIDI_MANAGER              0x800db8b4u
#define MIDI_MANAGER_TX_RUNNING_STATUS 0x48u
#define MIDI_RX                   (MIDI_MANAGER + 4u)
#define MIDI_RX_ACTIVE_SETUP_POINTER 0x04u
#define MIDI_RX_MESSAGE_BYTES     0x08u
#define MIDI_RX_RECEIVED_COUNT    0x0cu
#define MIDI_RX_EXPECTED_COUNT    0x10u
#define MIDI_RX_STATUS_NIBBLE     0x14u
#define MIDI_RX_LENGTH_TABLE      0x18u
#define MIDI_RX_CHANNEL_MODE      0x38u
#define MIDI_RX_BASIC_CHANNEL     0x3cu
#define MIDI_RX_FILTERED_STATUS   0xfeu
#define MIDI_MODE_OMNI            0u
#define MIDI_MODE_UNI             1u

/* SysEx framing. */
#define SYSEX_MANUFACTURER        0x7du /* noncommercial */
#define SYSEX_DEVICE_FIRST        'A'
#define SYSEX_DEVICE_SECOND       '8'
#define SYSEX_HEADER_BYTES        7u /* manufacturer, device x2, version, command, sequence, length */
#define SYSEX_REPLY_FLAG          0x40u
#define EVENT_COUNTERS            0x30u /* sent as 70h */
#define EVENT_RANGE               0x31u /* sent as 71h */
#define CRC32_POLYNOMIAL          0xedb88320u

#define STAGE_EXPIRY_TICKS        30000u
#define WATCHDOG_KICK_TICKS       1000u
#define REBOOT_RETRY_TICKS        10000u
#define SAMPLE_SERVICE_STATE_ADDR (0x800dba88u + 0x134u)
#define POWERDOWN_LOW             0x80348e40u
#define BYTE(address)             (*(volatile u8 *)(address))
#define MEMORY_BARRIER()          __asm__ volatile("dmb" ::: "memory")

enum command {
    CMD_IDENTIFY = 1, CMD_DIAGNOSTICS, CMD_PEEK, CMD_POKE,
    CMD_STREAM_RANGE, CMD_STREAM, CMD_FOREGROUND, CMD_DISPLAY, CMD_PANEL, CMD_SCREENSHOT,
    CMD_INSTALL_BEGIN = 16, CMD_BLOCK_CRC, CMD_BLOCK_BEGIN, CMD_BLOCK_DATA,
    CMD_BLOCK_END, CMD_INSTALL_COMMIT, CMD_INSTALL_ABORT,
    CMD_PROMOTE, CMD_RESTORE, CMD_REBOOT, CMD_INSTALL_DIRECT,
    CMD_SAMPLE_BEGIN = 32, CMD_SAMPLE_DATA, CMD_SAMPLE_COMMIT, CMD_SAMPLE_ABORT,
    CMD_RECORD_ARM = 40, CMD_RECORD_START, CMD_RECORD_STOP, CMD_RECORD_CANCEL, CMD_RECORD_STATUS,
    CMD_EQ_AUTOMATE,
    /* 30h and 31h replies would collide with the stream events. */
    CMD_EXTENSION_FIRST = 0x32, CMD_EXTENSION_LAST = 0x3f,
};
enum result {
    OK, BAD_COMMAND, BAD_LENGTH, BAD_RANGE, BUSY, IO_ERROR, BAD_CRC,
    BAD_IMAGE, BAD_STATE, NO_MEMORY, FILE_EXISTS
};
#define REQUEST_BYTES 48u
#define REPLY_BYTES 127u
#define BLOCK_BYTES 4096u
#define CONFIRM_WORD 0x38415153u

void patch_startup(void);
void patch_rx_poll(void *rx);
void patch_foreground_pass(void);
void patch_storage_progress(u32 result);
extern u32 foreground_storage_reads;
void patch_render_stats(u32 start, u32 end);
void patch_cv_transmit(void *manager);
void patch_program_transmit(void *manager, u32 preset);
void patch_uart_put(u32 base, u32 byte);
void reply(u32 command, u32 sequence, u32 status, const void *data, u32 bytes);
void diagnostics_snapshot(struct counters *out);
void diagnostics_reset_timing(void);
int ram_range(u32 address, u32 bytes, int writing);
int storage_claim(void);
void storage_release(void);
int stock_busy(void);
int image_valid(u8 *image);
int read_image(const char *path, u8 *image);
int watchdog_start(void);
void watchdog_stop(void);
void cold_reset(void) __attribute__((noreturn));

/* Boot state. */
extern u32 boot_mode;
extern u32 boot_trial_failed;

/* sysex.c */
void sysex_panel_service(void);
int sysex_send(u32 command, u32 sequence, const u8 *payload, u32 bytes);
int sysex_tx_idle(void);

/* commands.c */
void commands_handle(u32 command, u32 sequence, const u8 *payload, u32 bytes);
void commands_panel_tick(void);
/* eq_model.c; eq.h describes the payload. */
void eq_automate(const u8 *payload, u32 bytes);

/* install.c */
void install_foreground_poll(void);
int install_command(u32 command, u32 sequence, const u8 *payload, u32 bytes);
int install_busy(void);
void watchdog_kick(void);

/* sample_transfer.c */
int sample_transfer_busy(void);
void sample_transfer_command(u32 command, u32 sequence, const u8 *payload, u32 bytes);
void sample_transfer_foreground_poll(void);

/* record.c */
int record_busy(void);
void record_command(u32 command, u32 sequence, const u8 *payload, u32 bytes);
void record_panel_tick(void);
void record_foreground_poll(void);

/* extension.c */
void extension_foreground_poll(void);
int extension_command(u32 command, u32 sequence, const u8 *payload, u32 bytes);
u32 extension_identity(u8 *out);
int extension_image_valid(u8 *image, u32 bytes);
int extension_persist(u8 *image, u32 bytes, u32 crc);
void extension_stage_seed(u8 *stage, u32 bytes);
void extension_reload(void);
int extension_missing(void);

/* update.c */
int update_active(void);
void update_enter(void);
void update_leave(void);
int update_silent(void);

/* boot.c */
void boot_check_trial(void);

void intc_reset(void);
int boot_chain_loaded(void);
void patch_reset_entry(void);

/* display.c */
void display_glyph_colors(u32 *colors);
void display_before_enqueue(u32 kind, u32 x, u32 y, u32 width, u32 height);
int display_set_mode(u32 mode);
void display_select_mode(u32 mode);

/* screenshot.c: snapshot after drawing; compress and allocate in foreground. */
void screenshot_command(u32 sequence, const u8 *payload, u32 bytes);
void screenshot_after_display(void);
void screenshot_foreground_poll(void);

/* panel.c */
u32 panel_start(u32 sequence, u32 buttons, u32 hold_ticks, s32 steps);
void panel_tick(void);

/* status.c */
void status_show(const char *text);
void status_panel_tick(void);

/* trace.c */
void trace_reset_entry(int chain_loaded);
void trace_stage(u32 stage);
void trace_heartbeat(u32 render_calls, u32 panel_polls, u32 foreground_polls);

/* Little-endian field helpers. */
static inline u32 get_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static inline u32 get_u16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static inline void put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

#endif
