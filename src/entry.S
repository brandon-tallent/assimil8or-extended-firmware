#include "trace_stages.h"
.syntax unified
.cpu cortex-a8
.arm
.text
.equ PMCR_ENABLE_AND_RESET_CYCLES, 0x5        @ PMCR.E | PMCR.C
.equ PMCNTEN_CYCLE_COUNTER, 0x80000000        @ PMCNTENSET.C
.equ SCTLR_MMU_ENABLE, 0x1                    @ SCTLR.M
.equ IMAGE_LOAD_ADDRESS, 0x80000000           @ reset vector of the loaded image
.global cpu_irq_save, cpu_irq_restore, cpu_cycles, cpu_cycles_enable
cpu_irq_save:
    mrs r0, cpsr
    cpsid i
    bx lr
cpu_irq_restore:
    msr cpsr_c, r0
    bx lr
cpu_cycles:
    isb
    mrc p15, 0, r0, c9, c13, 0
    bx lr
cpu_cycles_enable:
    mov r0, #PMCR_ENABLE_AND_RESET_CYCLES
    mcr p15, 0, r0, c9, c12, 0
    mov r0, #PMCNTEN_CYCLE_COUNTER
    mcr p15, 0, r0, c9, c12, 1
    isb
    bx lr

/* C hooks use integer registers only. Stock renderer also preserves r12 and
 * all NEON registers; retain that stronger contract at this IRQ call site. */
.global patch_render
patch_render:
    push {r4, r5, r12, lr}
    isb
    mrc p15, 0, r4, c9, c13, 0
    bl process_mcspi_ring_frame_simd
    isb
    mrc p15, 0, r5, c9, c13, 0
    mov r0, r4
    mov r1, r5
    bl patch_render_stats
    pop {r4, r5, r12, pc}

/* Only the taken branch after a complete foreground pass is redirected.
 * The held/spinning path does not feed the watchdog. Preserve flags and
 * volatile registers that the inlined executive keeps across that branch. */
.global patch_foreground_hook
patch_foreground_hook:
    push {r0-r3, r12, lr}
    mrs r0, cpsr
    push {r0, r1}
    bl patch_foreground_pass
    pop {r0, r1}
    msr cpsr_f, r0
    pop {r0-r3, r12, lr}
    b exec_foreground_check_hold

/* A folder/sample load can stay inside one foreground pass for much longer
 * than the watchdog period. Successful disk reads are real work checkpoints.
 * Preserve the stock return value and its stronger r3 preservation. */
.global patch_disk_read
patch_disk_read:
    push {r3-r5, lr}
    bl stock_disk_read
    mov r4, r0
    bl patch_storage_progress
    mov r0, r4
    pop {r3-r5, pc}
stock_disk_read:
    add r0, r0, r0, lsl #1
    b fatfs_disk_read + 4

/* Replays each hooked function's first stock instruction, then resumes it. */
.equ MIDI_MANAGER_TRANSMIT_READY, 0x104
.global stock_cv_transmit, stock_program_transmit
stock_cv_transmit:
    push {r4-r11, lr}
    b midi_transmit_cv_changes + 4
stock_program_transmit:
    ldrb r3, [r0, #MIDI_MANAGER_TRANSMIT_READY]
    b midi_transmit_preset_program_change + 4

/* Preferences hooks. The screen hook is entered by a branch at the stock
 * entry; the replay stubs run each replaced first instruction. */
.equ FRONT_PANEL_RUNTIME_STATE, 0x80236ef8
.global preferences_ui_stock, preferences_load_hook, preferences_write_hook
preferences_ui_stock:
    ldr r1, [r0, #4]
    b preferences_ui + 4
preferences_load_stock:
    push {r4, r5, r6, r7, r8, r9, lr}
    b preferences_yaml_load + 4
preferences_write_stock:
    movw r2, #:lower16:FRONT_PANEL_RUNTIME_STATE
    b preferences_yaml_write + 4
preferences_load_hook:
    push {r4, lr}
    bl preferences_load_stock
    mov r4, r0
    bl patch_preferences_after_load
    mov r0, r4
    pop {r4, pc}
preferences_write_hook:
    push {r4, lr}
    bl preferences_write_stock
    mov r4, r0
    bl patch_preferences_after_write
    mov r0, r4
    pop {r4, pc}

/* Display mode hooks: each replaces its stock function's first
 * instruction, adjusts colours or pixels, then replays that instruction. */
.global display_enqueue_hook, display_glyph_hook
display_enqueue_hook:
    push {r0-r3, r12, lr}
    ldrb r12, [sp, #24]              @ height: the caller's stack argument
    sub sp, sp, #8
    str r12, [sp]
    bl display_before_enqueue
    add sp, sp, #8
    pop {r0-r3, r12, lr}
    push {r4-r10, lr}
    b oled_enqueue_transfer + 4
display_glyph_hook:
    push {r0-r3, r12, lr}
    add r0, sp, #24                  @ foreground, background stack arguments
    bl display_glyph_colors
    pop {r0-r3, r12, lr}
    push {r4-r11, lr}
    b ui_draw_proportional_glyph + 4

/* Capture only after the stock compositor has completed a display pass. */
.global screenshot_display_hook
screenshot_display_hook:
    push {r4, lr}
    bl screenshot_display_stock
    bl screenshot_after_display
    pop {r4, pc}
screenshot_display_stock:
    push {r4-r11, lr}
    b cdisplay_task_service + 4

/* Replaces the panel dispatcher's button-pin read; the record being
 * scanned is in r4 there. */
.global panel_pin_read_hook
panel_pin_read_hook:
    mov r2, r4
    b panel_pin_read

/* The reset handler's final bx r10 enters the C runtime with lr set to its
 * idle loop; run the patch reset entry first, then take that branch
 * unchanged. */
.global patch_reset_hook
patch_reset_hook:
    push {r0-r5, r10, lr}
    bl patch_reset_entry
    pop {r0-r5, r10, lr}
    bx r10

/* Each stub replaces one bl in the stock boot path: log the stage, then
 * tail-call the stock target with the caller's arguments and lr intact. */
.macro TRACE_CALL stage, target
.global trace_call_\stage
trace_call_\stage:
    push {r0-r5, r12, lr}
    mov r0, #\stage
    bl trace_stage
    pop {r0-r5, r12, lr}
    b \target
.endm
TRACE_CALL TRACE_STAGE_MAIN, main
TRACE_CALL TRACE_STAGE_MMU_PREPARE, mmu_table_clear_and_cp15_prep
TRACE_CALL TRACE_STAGE_MMU_ENABLE, mmu_enable_after_table_build
TRACE_CALL TRACE_STAGE_CACHE_ENABLE, arm_cache_enable_mask
TRACE_CALL TRACE_STAGE_ENGINE_MEMORY, audio_engine_shared_memory_bootstrap
TRACE_CALL TRACE_STAGE_TIMERS, dmtimer2_3_initialize_and_register_irqs
TRACE_CALL TRACE_STAGE_BOARD_GPIO, am335x_board_gpio_clock_reset_pinmux_init
TRACE_CALL TRACE_STAGE_GPMC, gpmc_cs0_init_and_pinmux
TRACE_CALL TRACE_STAGE_MCSPI_BOOT, mcspi_bootstrap_and_dma_setup
TRACE_CALL TRACE_STAGE_MCASP0_SETUP, mcasp0_audio_stream_setup
TRACE_CALL TRACE_STAGE_MCASP1_BRINGUP, mcasp1_audio_bringup
TRACE_CALL TRACE_STAGE_MCSPI_ENABLE, mcspi_enable_ch0_both
TRACE_CALL TRACE_STAGE_MCASP0_START, mcasp0_tx_start
TRACE_CALL TRACE_STAGE_MCASP1_RELEASE, mcasp1_release_state_machines_and_frames
TRACE_CALL TRACE_STAGE_IRQ_ENABLE, arm_enable_irq
TRACE_CALL TRACE_STAGE_TSCADC, tscadc_fifo0_edma_mailbox_setup
TRACE_CALL TRACE_STAGE_MMCHS, mmchs_pair_driver_init
TRACE_CALL TRACE_STAGE_MOUNT_BOOT, filesystem_register_and_mount_volume
TRACE_CALL TRACE_STAGE_MOUNT_PANEL, filesystem_register_and_mount_volume
TRACE_CALL TRACE_STAGE_OLED, ssd1351_oled_bootstrap
TRACE_CALL TRACE_STAGE_OLED_DMA, oled_edma_init
TRACE_CALL TRACE_STAGE_LATE_STARTUP, application_late_startup_initialize
TRACE_CALL TRACE_STAGE_FOREGROUND_LOOP, exec_foreground_main_loop
TRACE_CALL TRACE_STAGE_TIMER_CLOCKS, am335x_prcm_enable_dmtimer2_3
TRACE_CALL TRACE_STAGE_TIMER_IRQ_ENABLE, arm_enable_irq
TRACE_CALL TRACE_STAGE_TIMER_INTC_RESET, intc_initialize_controller_and_handler_table
TRACE_CALL TRACE_STAGE_TIMER2_HANDLER, irq_handler_table_set
TRACE_CALL TRACE_STAGE_TIMER2_ROUTE, intc_configure_irq_priority_route
TRACE_CALL TRACE_STAGE_TIMER2_UNMASK, intc_unmask_irq
TRACE_CALL TRACE_STAGE_TIMER3_HANDLER, irq_handler_table_set
TRACE_CALL TRACE_STAGE_TIMER3_ROUTE, intc_configure_irq_priority_route
TRACE_CALL TRACE_STAGE_TIMER3_UNMASK, intc_unmask_irq

/* Copied to TRAMPOLINE_BASE in DDR and run after all files close and DMA is
 * quiescent. No stack, literals or calls after the payload starts
 * overwriting the application. Enter the image in Supervisor mode, as the
 * boot loader does. */
.equ ARM_MODE_SUPERVISOR, 0x13
.global trampoline_start, trampoline_end
trampoline_start:
    cpsid if, #ARM_MODE_SUPERVISOR
    mrc p15, 0, r3, c1, c0, 0
    bic r3, r3, #SCTLR_MMU_ENABLE
    mcr p15, 0, r3, c1, c0, 0
    isb
    mov r2, #IMAGE_LOAD_ADDRESS
1:  ldr r3, [r0], #4
    str r3, [r2], #4
    subs r1, r1, #4
    bne 1b
    dsb
    mov r3, #0
    mcr p15, 0, r3, c7, c5, 0
    mcr p15, 0, r3, c7, c5, 6
    mcr p15, 0, r3, c8, c7, 0
    dsb
    isb
    mov r0, #0
    mov r1, #0
    mov r2, #0
    mov pc, #IMAGE_LOAD_ADDRESS
trampoline_end:

.fpu neon
/* EQ panel entry/replay stubs. Scalar C uses the soft PCS; the stock
 * normalized knob callback instead receives its float in s0. */
.global eq_bits_hook, eq_bits_stock, eq_render_stock, eq_render_hook, eq_knob_hook
.global eq_channel_select_hook
.global eq_channel_select_stock
.global eq_channel_button_hook
.global eq_value_bar
 /* Soft-PCS C passes the bar value's float bits in r3; the stock bar takes s0. */
 eq_value_bar:
    vmov s0, r3
    b ui_draw_bipolar_value_bar
 eq_channel_button_hook:
    push {r0-r3, r12, lr}
    bl eq_panel_channel_gesture
    pop {r0-r3, r12, lr}
    cmp r1, #7
    b front_panel_channel_button_press + 4
 eq_bits_hook:
    push {r4, lr}
    mov r4, r0
    bl eq_bits_stock
    mov r0, r4
    bl eq_panel_after_bits
    pop {r4, pc}
 eq_bits_stock:
    push {r4-r9, lr}
    b zone_editor_bit_control_primary_callback + 4
 eq_render_hook:
    b eq_panel_render
 eq_render_stock:
    push {r4-r11, lr}
    b ui_render_channel_bits_aliasing_page + 4
/* DATA 2 on a page of ours sets our field; otherwise replay the stock
 * setter's first instruction and resume it. */
.equ KNOB_SAVED_NEON_BYTES, 64                @ d0-d7, below the saved r0
.equ ZONE_EDITOR_BIT_PARAMETER_SELECTOR, 0xf8
.equ ZONE_EDITOR_PHASE_MODULATION_PARAMETER_SELECTOR, 0x100
.macro KNOB_HOOK name, target, replay:vararg
\name:
    push {r0-r3, r12, lr}
    vpush {d0-d7}
    bl eq_panel_active
    cmp r0, #0
    beq 1f
    ldr r0, [sp, #KNOB_SAVED_NEON_BYTES]
    vmov r1, s0
    bl eq_panel_knob
    vpop {d0-d7}
    pop {r0-r3, r12, pc}
1:  vpop {d0-d7}
    pop {r0-r3, r12, lr}
    \replay
    b \target + 4
.endm
.global phase_knob_hook, phase_entry_stock, phase_render_stock
KNOB_HOOK eq_knob_hook, zone_editor_bit_parameter_set_normalized, ldr r3, [r0, #ZONE_EDITOR_BIT_PARAMETER_SELECTOR]
KNOB_HOOK phase_knob_hook, zone_editor_phase_modulation_parameter_set_normalized, ldr r2, [r0, #ZONE_EDITOR_PHASE_MODULATION_PARAMETER_SELECTOR]
.equ EDITOR_SELECTOR_PAGE_STATE, 0x58
 phase_entry_stock:
    ldr r3, [r0, #EDITOR_SELECTOR_PAGE_STATE]
    b zone_operation_panel_primary_callback + 4
 phase_render_stock:
    push {r4-r10, lr}
    b ui_render_channel_phase_modulation_page + 4
 eq_channel_select_hook:
    push {r4, lr}
    bl eq_channel_select_stock
    mov r4, r0
    bl eq_panel_selection_changed
    mov r0, r4
    pop {r4, pc}
 eq_channel_select_stock:
    ldr r12, [r0, #0x58]
    b zone_editor_select_channel_and_refresh_value + 4

/* Preset extras (preset_extras.c). Its C routines stand in for the stock
 * ones and call them through these stubs, which replay each first
 * instruction. The load and folder hooks keep the stock arguments and
 * results in place around their C call. */
.equ PRESET_LIBRARY_FIRST_INDEX, 1
.global extras_parse_file_stock, extras_parse_field_stock, extras_save_stock, extras_erase_stock
.global extras_write_stock, extras_differs_stock, extras_load_hook, extras_folder_hook
 extras_parse_file_stock:
    push {r4-r11, lr}
    b preset_yaml_parse_open_file + 4
 extras_parse_field_stock:
    push {r4-r10, lr}
    b preset_yaml_parse_field + 4
 extras_save_stock:
    sub r3, r1, #PRESET_LIBRARY_FIRST_INDEX
    b preset_save_library_index_schedule_yaml_write + 4
 extras_erase_stock:
    sub r3, r1, #PRESET_LIBRARY_FIRST_INDEX
    b preset_erase_library_index_schedule_yaml_delete + 4
 extras_write_stock:
    push {r4, r5, r6, lr}
    b preset_yaml_write_file + 4
 extras_differs_stock:
    push {r4-r11, lr}
    b preset_working_model_differs_from_library + 4
 extras_load_hook:
    push {r4, lr}
    mov r4, r1
    bl extras_load_stock
    push {r0, r1}
    mov r0, r4
    bl preset_extras_loaded
    pop {r0, r1}
    pop {r4, pc}
 extras_load_stock:
    sub r3, r1, #PRESET_LIBRARY_FIRST_INDEX
    b preset_load_library_index_into_working_model + 4
 extras_folder_hook:
    push {r0-r3, r4, lr}
    sub sp, sp, #8
    bl preset_extras_folder
    add sp, sp, #8
    pop {r0-r3, r4, lr}
    push {r4-r11, lr}
    b preset_library_load_yaml_folder + 4
