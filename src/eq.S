/* Five direct-form-I sections for eight channels, in two four-lane groups.
 * Coefficients and histories are Q4.28; input/output are stock signed Q31.
 * Ordinary (not doubling) 64-bit accumulation keeps cancellation exact.
 * The coefficient builder bounds the L1 sum below sixteen, so even full-range
 * histories plus the saved fractional error cannot wrap the accumulator.
 * No allocation, coefficient calculation or extra audio buffer runs here.
 */
.syntax unified
.cpu cortex-a8
.fpu neon
.arm
.text
.global patch_channel_eq

.macro GROUP input, lo, hi
    mov r6, r3
    vld1.32 {d4-d5}, [r3]!
    vorr d2, d4, d5
    vmov r1, r2, d2
    orr r2, r1, r2
    cmp r9, #0
    beq .Lmask_done\@
    vld1.32 {d0-d1}, [r7]!
    cmp r9, #1
    bne .Lmask_union\@
    vorr q2, q0, q0
    b .Lmask_store\@
.Lmask_union\@:
    vorr q2, q2, q0
.Lmask_store\@:
    vst1.32 {d4-d5}, [r6]
    b .Lmask_done2\@
.Lmask_done\@:
    add r7, r7, #16
.Lmask_done2\@:
    vorr d0, d4, d5
    vmov r4, r5, d0
    orrs r4, r4, r5
    bne .Lwet\@
    cmp r2, #0
    beq .Ldry\@
.Lwet\@:
    vpush {\lo-\hi}
    vshr.s32 \input, \input, #3
    mov r5, #5
.Lsection\@:
    vld1.32 {d20-d23}, [r3]!
    vld1.32 {d24-d27}, [r3]!
    vld1.32 {d28-d29}, [r3]!
    cmp r9, #0
    beq .Lsteady\@
    sub r4, r3, #80
    mov r0, #5
    /* q10..q14 hold the current coefficients. Update one vector at a
     * time through the data table so q4/q5 and histories remain untouched. */
    sub r1, r3, #80
.Lglide\@:
    vld1.32 {d0-d1}, [r4]
    cmp r9, #512
    bne .Lstep_ready\@
    vld1.32 {d2-d3}, [r7]
    vsub.i32 q1, q1, q0
    vshr.s32 q1, q1, #9
    vst1.32 {d2-d3}, [r8]
.Lstep_ready\@:
    cmp r9, #1
    vld1.32 {d2-d3}, [r8]!
    vadd.i32 q0, q0, q1
    bne .Lnot_final\@
    vld1.32 {d0-d1}, [r7]
.Lnot_final\@:
    add r7, r7, #16
    vst1.32 {d0-d1}, [r4]!
    subs r0, r0, #1
    bne .Lglide\@
    vld1.32 {d20-d23}, [r1]!
    vld1.32 {d24-d27}, [r1]!
    vld1.32 {d28-d29}, [r1]!
    b .Lcoeff_done\@
.Lsteady\@:
    add r7, r7, #80
    add r8, r8, #80
.Lcoeff_done\@:
    vld1.32 {d12-d15}, [r3]!
    vld1.32 {d16-d19}, [r3]!
    vld1.32 {d30-d31}, [r3]!
    vmovl.s32 q0, d30
    vmovl.s32 q1, d31
    vmlal.s32 q0, \lo, d20
    vmlal.s32 q1, \hi, d21
    vmlal.s32 q0, d12, d22
    vmlal.s32 q1, d13, d23
    vmlal.s32 q0, d14, d24
    vmlal.s32 q1, d15, d25
    vmlal.s32 q0, d16, d26
    vmlal.s32 q1, d17, d27
    vmlal.s32 q0, d18, d28
    vmlal.s32 q1, d19, d29
    vqrshrn.s64 d6, q0, #28
    vqrshrn.s64 d7, q1, #28
    /* Away from clipping, signed low 28 bits equal
     * acc - rounded(acc/2^28)*2^28. Save only that bounded fractional part
     * (also on clipping) so rounding noise has a zero at DC without windup. */
    vmovn.i64 d30, q0
    vmovn.i64 d31, q1
    vshl.i32 q15, q15, #4
    vshr.s32 q15, q15, #4
    sub r4, r3, #80
    vst1.32 {\lo-\hi}, [r4]!
    vst1.32 {d12-d13}, [r4]!
    vst1.32 {d6-d7}, [r4]!
    vst1.32 {d16-d17}, [r4]!
    vst1.32 {d30-d31}, [r4]!
    vorr \input, q3, q3
    subs r5, r5, #1
    bne .Lsection\@
    vqshl.s32 \input, \input, #3
    vpop {d0-d1}
    vld1.32 {d4-d5}, [r6]
    vbsl q2, \input, q0
    vorr \input, q2, q2
    b .Ldone\@
.Ldry\@:
    add r3, r3, #800
    add r7, r7, #400
    add r8, r8, #400
.Ldone\@:
.endm

patch_channel_eq:
    push {r0-r12, lr}
    mrs r10, cpsr
    vmrs r11, fpscr
    vpush {d0-d7}
    vpush {d12-d15}
    vpush {d16-d31}
    bl patch_filter
    ldr r12, =eq_mailbox
    ldr r7, [r12, #4]
    cmp r7, #0
    beq .Lno_publish
    str r7, [r12]
    mov r9, #512
    str r9, [r12, #8]
    dmb sy
    mov r0, #0
    str r0, [r12, #4]
.Lno_publish:
    ldr r7, [r12]
    ldr r9, [r12, #8]
    ldr r8, =eq_steps
    ldr r3, =eq_fixed_data
    GROUP q4, d8, d9
    GROUP q5, d10, d11
    cmp r9, #0
    subne r9, r9, #1
    strne r9, [r12, #8]
    /* Update mode fade (update.c): scale all eight channels by the current
     * gain and move it one step toward its target. */
    ldr r3, =patch_output_gain
    ldmia r3, {r4, r5, r6}
    cmp r4, r5
    beq .Lgain_steady
.Lgain_apply:
    vdup.32 q0, r4
    vqrdmulh.s32 q4, q4, q0
    vqrdmulh.s32 q5, q5, q0
    cmp r4, r5
    bgt .Lgain_down
    sub r7, r5, r4
    cmp r7, r6
    addhi r4, r4, r6
    movls r4, r5
    str r4, [r3]
    b .Lgain_done
.Lgain_down:
    sub r7, r4, r5
    cmp r7, r6
    subhi r4, r4, r6
    movls r4, r5
    str r4, [r3]
    b .Lgain_done
.Lgain_steady:
    mvn r7, #0x80000000
    cmp r4, r7
    bne .Lgain_apply
.Lgain_done:
    ldr r3, =patch_channel_out
    vst1.32 {d8-d11}, [r3]
    ldr r12, =patch_channel_fx_hook
    ldr r12, [r12]
    cmp r12, #0
    beq .Lfx_done
    mov r0, r3
    mov r4, sp
    bic sp, sp, #7
    blx r12
    mov sp, r4
    ldr r3, =patch_channel_out
    vld1.32 {d8-d11}, [r3]
.Lfx_done:
    vpop {d16-d31}
    vpop {d12-d15}
    vpop {d0-d7}
    vmsr fpscr, r11
    msr cpsr_f, r10
    pop {r0-r12, lr}
    vld1.8 {d12}, [lr :64]!
    b audio_per_channel_eq_insertion_point + 4
.ltorg
