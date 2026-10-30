/* Grain mode's two places in the stock renderer; ext/grains.c renders the
 * grains. A third serves resampling.
 *
 * patch_grain_capture_steps replaces the first instruction of the stock
 * playhead stage. q4/q5 hold the eight channels' pitch steps for this frame,
 * after pitch CV, FM and Aliasing; r5 is dead until the stage's last
 * instruction sets it.
 *
 * patch_grain_mix_stage follows the convolution. q4/q5 hold the eight
 * rendered samples before channel effects; r4 and the flags are dead. The
 * hook is C built for general registers only, so NEON state needs no saving.
 *
 * patch_resample_frame replaces the first two instructions of the stock
 * 48 kHz capture stage, which form the Sample input's stereo frame in d0.
 * While a source is set, d0 is that channel's last output and the next
 * channel's. Before that, the Sample input's frame is kept in
 * patch_input_frame for the mix effects. r5 and the flags are dead until the stage's next instruction.
 */
.syntax unified
.cpu cortex-a8
.fpu neon
.arm
.text
.global patch_grain_capture_steps, patch_grain_mix_stage, patch_resample_frame

patch_grain_capture_steps:
    movw	r5, #:lower16:patch_grain_steps
    movt	r5, #:upper16:patch_grain_steps
    vst1.32	{d8-d11}, [r5]
    vld1.32	{d18-d21}, [r2 :128]!
    b	audio_advance_channel_playheads + 4

patch_resample_frame:
    vrhadd.s32	d0, d10, d12
    vqshl.s32	d0, d0, #4
    movw	r5, #:lower16:patch_input_frame
    movt	r5, #:upper16:patch_input_frame
    vst1.32	{d0}, [r5]
    movw	r5, #:lower16:patch_resample_source
    movt	r5, #:upper16:patch_resample_source
    ldr	r5, [r5]
    cmp	r5, #0
    beq	audio_capture_48khz_frame + 8
    vld1.32	{d0}, [r5]
    b	audio_capture_48khz_frame + 8

patch_grain_mix_stage:
    movw	r4, #:lower16:patch_grain_hook
    movt	r4, #:upper16:patch_grain_hook
    ldr	r4, [r4]
    cmp	r4, #0
    beq	audio_apply_channel_bit_depth
    push	{r0-r3, r12, lr}
    ldr	r0, =patch_grain_mix
    ldr	r1, =patch_grain_steps
    vst1.32	{d8-d11}, [r0]
    mov	r12, r4
    mov	r4, sp
    bic	sp, sp, #7
    blx	r12
    mov	sp, r4
    ldr	r0, =patch_grain_mix
    vld1.32	{d8-d11}, [r0]
    pop	{r0-r3, r12, lr}
    b	audio_apply_channel_bit_depth
.ltorg

/* patch_mix_bus replaces the first instruction of the stock stereo output
 * filter. d0 holds the two mix samples as the outputs take them and d1 the
 * two output trims. The hook is general-register C; r4 is dead here. */
.global patch_mix_bus
patch_mix_bus:
    push	{r0-r4, r12, lr}
    ldr	r4, =patch_mix_fx_hook
    ldr	r12, [r4]
    cmp	r12, #0
    beq	1f
    ldr	r0, =patch_mix_frame
    vst1.32	{d0-d1}, [r0]
    mov	r4, sp
    bic	sp, sp, #7
    blx	r12
    mov	sp, r4
    ldr	r0, =patch_mix_frame
    vld1.32	{d0}, [r0]
1:  pop	{r0-r4, r12, lr}
    vld1.32	{d1-d3}, [r2 :64]!
    b	audio_filter_stereo_mix_output + 4
.ltorg

/* patch_pan_capture replaces the first instruction of the stock pan stage.
 * q4/q5 hold the eight channels after their Mix Output Level, mute and
 * solo: the mix effects' post-fader sends read them. r0 is saved here. */
.global patch_pan_capture
patch_pan_capture:
    push	{r0}
    movw	r0, #:lower16:patch_fader_out
    movt	r0, #:upper16:patch_fader_out
    vst1.32	{d8-d11}, [r0]
    pop	{r0}
    vld1.16	{d22-d25}, [r2 :128]!
    b	audio_apply_stereo_pan + 4
