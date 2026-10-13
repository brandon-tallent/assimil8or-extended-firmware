/* Trial boot trace stages, shared by C and entry.S. A stage is logged just
 * before the step it names runs, so the last stage of a hung trial is the
 * step that did not return. */
#ifndef TRACE_STAGES_H
#define TRACE_STAGES_H

#define TRACE_STAGE_RESET                 1  /* reset handler, before C runtime */
#define TRACE_STAGE_MAIN                  2
#define TRACE_STAGE_MMU_PREPARE           3
#define TRACE_STAGE_MMU_ENABLE            4
#define TRACE_STAGE_CACHE_ENABLE          5
#define TRACE_STAGE_ENGINE_MEMORY         6
#define TRACE_STAGE_TIMERS                7
#define TRACE_STAGE_BOARD_GPIO            8
#define TRACE_STAGE_GPMC                  9
#define TRACE_STAGE_MCSPI_BOOT            10
#define TRACE_STAGE_MCASP0_SETUP          11
#define TRACE_STAGE_MCASP1_BRINGUP        12
#define TRACE_STAGE_MCSPI_ENABLE          13
#define TRACE_STAGE_MCASP0_START          14
#define TRACE_STAGE_MCASP1_RELEASE        15
#define TRACE_STAGE_IRQ_ENABLE            16
#define TRACE_STAGE_TSCADC                17
#define TRACE_STAGE_MMCHS                 18
#define TRACE_STAGE_MOUNT_BOOT            19
#define TRACE_STAGE_MOUNT_PANEL           20
#define TRACE_STAGE_PATCH_STARTUP         21
#define TRACE_STAGE_TRIAL_CONFIRMED       22
#define TRACE_STAGE_OLED                  23
#define TRACE_STAGE_OLED_DMA              24
#define TRACE_STAGE_LATE_STARTUP          25
#define TRACE_STAGE_FOREGROUND_LOOP       26
#define TRACE_STAGE_FIRST_FOREGROUND_PASS 27
#define TRACE_STAGE_FIRST_PANEL_SERVICE   28
/* Calls inside dmtimer2_3_initialize_and_register_irqs. */
#define TRACE_STAGE_TIMER_CLOCKS          29
#define TRACE_STAGE_TIMER_IRQ_ENABLE      30
#define TRACE_STAGE_TIMER_INTC_RESET      31
#define TRACE_STAGE_TIMER2_HANDLER        32
#define TRACE_STAGE_TIMER2_ROUTE          33
#define TRACE_STAGE_TIMER2_UNMASK         34
#define TRACE_STAGE_TIMER3_HANDLER        35
#define TRACE_STAGE_TIMER3_ROUTE          36
#define TRACE_STAGE_TIMER3_UNMASK         37

#endif
