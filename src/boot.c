#include "patch.h"

/* AM335x TRM SPRUH73Q: WDT posted writes, EDMA CCSTAT/TCSTAT.
 * Clock and watchdog waits are bounded; a failed trial never re-arms itself. */
static int wait_clear(u32 address, u32 mask)
{
    for (u32 left = 1000000; left; --left) if (!(REG32(address) & mask)) return 1;
    return 0;
}
static int wdt_write(u32 offset, u32 mask, u32 value)
{
    if (!wait_clear(WDT1_BASE + WDT_WWPS, mask)) return 0;
    REG32(WDT1_BASE + offset) = value;
    return wait_clear(WDT1_BASE + WDT_WWPS, mask);
}
int watchdog_start(void)
{
    REG32(CM_WKUP_BASE + CM_WKUP_WDT1_CLKCTRL) = CLKCTRL_MODULEMODE_ENABLE;
    if (!wait_clear(CM_WKUP_BASE + CM_WKUP_WDT1_CLKCTRL, CLKCTRL_IDLEST_MASK)) return 0;
    return wdt_write(WDT_WSPR, WDT_PENDING_WSPR, WDT_WSPR_DISABLE_FIRST) &&
        wdt_write(WDT_WSPR, WDT_PENDING_WSPR, WDT_WSPR_DISABLE_SECOND) &&
        wdt_write(WDT_WCLR, WDT_PENDING_WCLR, 0) &&
        wdt_write(WDT_WLDR, WDT_PENDING_WLDR, 0u - WDT_CLOCK_HZ * WDT_TRIAL_SECONDS) &&
        wdt_write(WDT_WCRR, WDT_PENDING_WCRR, 0u - WDT_CLOCK_HZ * WDT_TRIAL_SECONDS) &&
        wdt_write(WDT_WSPR, WDT_PENDING_WSPR, WDT_WSPR_ENABLE_FIRST) &&
        wdt_write(WDT_WSPR, WDT_PENDING_WSPR, WDT_WSPR_ENABLE_SECOND);
}
void watchdog_stop(void)
{
    if (!wdt_write(WDT_WSPR, WDT_PENDING_WSPR, WDT_WSPR_DISABLE_FIRST) ||
        !wdt_write(WDT_WSPR, WDT_PENDING_WSPR, WDT_WSPR_DISABLE_SECOND))
        cold_reset(); /* app is already promoted */
}
void watchdog_kick(void)
{
    static u32 last_kick;
    u32 now = REG32(TICKS);
    if ((u32)(now - last_kick) < WATCHDOG_KICK_TICKS) return;
    if (wdt_write(WDT_WTGR, WDT_PENDING_WTGR, REG32(WDT1_BASE + WDT_WTGR) ^ 1u)) {
        counters.wdt_kicks++; last_kick = now;
    }
}
void cold_reset(void)
{
    __asm__ volatile("dsb" ::: "memory");
    REG32(PRM_DEVICE_BASE + PRM_RSTCTRL) = PRM_RSTCTRL_COLD_SW;
    for (;;) __asm__ volatile("wfi");
}

/* Stock timer setup clears CPSR.I before it soft-resets the INTC, relying on
 * the boot loader leaving the INTC quiet. A chain-loaded image inherits the
 * previous app's unmasked lines and possibly an IRQ the INTC has already
 * sorted and keeps asserting until acknowledged, even after masking; either
 * would dispatch through the still-empty handler table. Mask, acknowledge,
 * then soft-reset, as the stock init does later. The INTC is always
 * clocked, so this is safe on every boot. */
void intc_reset(void)
{
    for (u32 bank = 0; bank < INTC_MIR_BANKS; ++bank)
        REG32(INTC_BASE + INTC_MIR_SET0 + bank * INTC_MIR_BANK_STRIDE) = ~0u;
    REG32(INTC_BASE + INTC_CONTROL) = INTC_CONTROL_NEW_AGREEMENT;
    REG32(INTC_BASE + INTC_SYSCONFIG) = INTC_SYSCONFIG_SOFTRESET;
    __asm__ volatile("dsb" ::: "memory");
    for (u32 left = 1000000; left; --left)
        if (REG32(INTC_BASE + INTC_SYSSTATUS) & INTC_SYSSTATUS_RESETDONE) break;
}

/* Stock timer setup unmasks the timer lines before it reprograms the timers,
 * which is safe only while they are stopped, as they are after the boot
 * loader. Only a chain-load leaves them running with clocks enabled; on a
 * cold boot their module clocks may be off, so this must not run then. */
static void timers_stop(void)
{
    static const u32 bases[] = {DMTIMER2_BASE, DMTIMER3_BASE};
    for (u32 i = 0; i < sizeof(bases) / sizeof(bases[0]); ++i) {
        REG32(bases[i] + DMTIMER_IRQENABLE_CLR) = DMTIMER_IRQ_ALL;
        wait_clear(bases[i] + DMTIMER_TWPS, DMTIMER_TWPS_TCLR);
        REG32(bases[i] + DMTIMER_TCLR) = 0;
        wait_clear(bases[i] + DMTIMER_TWPS, DMTIMER_TWPS_TCLR);
        REG32(bases[i] + DMTIMER_IRQSTATUS) = DMTIMER_IRQ_ALL;
    }
    __asm__ volatile("dsb" ::: "memory");
}

/* A software chain-load is the only boot with the trial marker set and no
 * latched hardware reset cause: the stable build clears PRM_RSTST on boot,
 * and any real reset latches a cause. The marker alone is not enough; it
 * survives a reset in DDR when a trial dies before consuming it. */
int boot_chain_loaded(void)
{
    return !REG32(PRM_DEVICE_BASE + PRM_RSTST) &&
        ((volatile struct handoff *)HANDOFF_BASE)->magic == HANDOFF_TRIAL_MAGIC;
}

/* First patch code of every boot, before the C runtime; MMU is off. */
void patch_reset_entry(void)
{
    memset(patch_storage_start, 0, (u32)(patch_storage_end - patch_storage_start));
    int chain_loaded = boot_chain_loaded();
    intc_reset();
    if (chain_loaded) timers_stop();
    else ((volatile struct handoff *)HANDOFF_BASE)->magic = 0; /* stale */
    trace_reset_entry(chain_loaded);
}

void arm_cache_disable_mask(u32 mask);
extern u8 trampoline_start[], trampoline_end[];
void cpu_chain_load(const void *payload, u32 bytes)
{
    cpu_irq_save();
    intc_reset();
    timers_stop();
    /* Both audio streams and McSPI/ADC have already started in stock main.
     * Stop producers, mask DMA events, and wait for all three transfer
     * controllers before invalidating the old MMU or any DMA descriptors. */
    REG32(MCASP0_CFG_BASE + MCASP_GBLCTL) = 0;
    REG32(MCASP1_CFG_BASE + MCASP_GBLCTL) = 0;
    REG32(MCSPI0_BASE + MCSPI_CH0CTRL) = 0;
    REG32(MCSPI1_BASE + MCSPI_CH0CTRL) = 0;
    REG32(TSCADC_BASE + TSCADC_CTRL) &= ~TSCADC_CTRL_ENABLE;
    REG32(EDMA_SHADOW_REGION0 + EDMA_EECR) = ~0u;
    REG32(EDMA_SHADOW_REGION0 + EDMA_EECRH) = ~0u;
    REG32(EDMA_SHADOW_REGION0 + EDMA_QEECR) = ~0u;
    __asm__ volatile("dsb" ::: "memory");
    if (!wait_clear(EDMA_CC_BASE + EDMA_CCSTAT, EDMA_CCSTAT_ACTIVE) ||
        !wait_clear(EDMA_TPTC0_BASE + EDMA_TPTC_TCSTAT, EDMA_TCSTAT_BUSY_MASK) ||
        !wait_clear(EDMA_TPTC1_BASE + EDMA_TPTC_TCSTAT, EDMA_TCSTAT_BUSY_MASK) ||
        !wait_clear(EDMA_TPTC2_BASE + EDMA_TPTC_TCSTAT, EDMA_TCSTAT_BUSY_MASK)) cold_reset();
    memcpy((void *)TRAMPOLINE_BASE, trampoline_start, (u32)(trampoline_end - trampoline_start));
    /* The stock StarterWare routine disables and cleans both data levels,
     * invalidates instruction cache, and clears Cortex-A8 L2EN. That also
     * writes the trampoline copy back to DDR. The MMU stays on until the
     * trampoline turns it off, so the trampoline must be in executable DDR. */
    arm_cache_disable_mask(3);
    __asm__ volatile("dsb\n\tisb" ::: "memory");
    ((void (*)(const void *, u32))TRAMPOLINE_BASE)(payload, bytes);
    for (;;) {}
}

void boot_check_trial(void)
{
    volatile struct handoff *handoff = (volatile struct handoff *)HANDOFF_BASE;
    /* Software chain-load keeps this cookie; every actual reset clears its
     * authority through PRM_RSTST. A stale DDR cookie cannot become a trial. */
    if (!counters.reset_status && handoff->magic == HANDOFF_TRIAL_MAGIC &&
        !memcmp((const void *)handoff->build_id, patch_identity.build_id, 8)) {
        handoff->magic = 0;
        boot_mode = BOOT_TRIAL;
        return;
    }
    handoff->magic = 0;
    boot_trial_failed = !!(counters.reset_status & PRM_RSTST_WDT1);
    /* Latched causes persist until power-on; clear them once recorded so a
     * watchdog reset is reported only for the boot that follows it. */
    REG32(PRM_DEVICE_BASE + PRM_RSTST) = counters.reset_status;
    u8 *image = malloc(IMAGE_FILE_BYTES);
    if (!image) return;
    int status = read_image("0:/apptrial", image);
    if (!status && image_valid(image)) {
        int removed = fatfs_unlink("0:/apptrial.run");
        if ((!removed || removed == FR_NO_FILE) && !fatfs_rename("0:/apptrial", "0:/apptrial.run")) {
            if (watchdog_start()) {
                memcpy((void *)handoff->build_id,
                       image + 8 + PATCH_ADDRESS - IMAGE_LOAD_ADDRESS + 12, 8);
                handoff->magic = HANDOFF_TRIAL_MAGIC;
                MEMORY_BARRIER();
                cpu_chain_load(image + IMAGE_HEADER_BYTES, IMAGE_PAYLOAD_BYTES);
            } else cold_reset();
        }
    }
    free(image);
}

void *patch_demangle_disabled(const char *name, char *buffer, size_t *length, int *status)
{
    (void)name; (void)buffer; (void)length;
    if (status) *status = -2;
    return 0;
}
