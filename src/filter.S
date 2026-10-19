/* The channel filter: two state-variable sections for eight channels, in two
 * four-lane groups, run on the channel samples before the EQ.
 *
 * A section is the trapezoidal-integrator form of tools/svf_model.py in Q31:
 *
 *   v3 = x - ic2
 *   v1 = a1*ic1 + a2*v3                  bandpass
 *   v2 = ic2 + a2*ic1 + a3*v3            lowpass
 *   ic1 = 2*v1 - ic1,  ic2 = 2*v2 - ic2
 *   hp = x - k*v1 - v2                   highpass   (kq = k/4, so k reaches 4)
 *
 * Products are vqrdmulh.s32 and every add saturates, so a loud resonance clips
 * the states instead of wrapping. The samples keep five guard bits.
 *
 * A filter type is data: section 1's input and its lowpass, bandpass and
 * highpass taps mix into section 2's input (mix 1), and section 2's input and
 * taps, and section 1's lowpass, mix into the output (mix 2), so section 2
 * runs after section 1 or beside it. The weights are Q30, up to 2. Every type runs both sections, so the
 * cost does not depend on the type. The cutoff may move by a CV input, read
 * each block as stock reads its CV; a group runs the moving cutoff only when
 * a lane asks.
 *
 * A lane fades between its dry sample and the filter over 4 ms (WET_STEP) as
 * its mask turns on or off, so switching the filter on or off does not click;
 * the foreground keeps an Off lane's last coefficients for the fade out. A
 * lane that is off and faded passes its sample through exactly and keeps its
 * states at zero, and an idle group copies the published coefficients and
 * mixes to the current ones, so the next lane on starts from them.
 *
 * The coefficients and tap mixes the sections use close a thirty-second of
 * the gap to the published ones each frame, so a step in cutoff or
 * resonance does not zipper and a change of type crossfades over about
 * 3 ms. No allocation or coefficient calculation runs here; the foreground
 * fills the bank (eq_model.c, eq_math.c).
 *
 * Called from patch_channel_eq before it sets up its own registers: the
 * eight channels arrive in q4 and q5 and leave there. Uses r0-r8, r12 and
 * q0-q3, q6-q13, all saved by the caller.
 */
.syntax unified
.cpu cortex-a8
.fpu neon
.arm
.text
.global patch_filter

/* A group's published vectors, in bytes (FILTER_VECTOR_* in eq.h). */
.equ GROUP_BYTES, 416
/* A group's current values: the coefficients and tap mixes as they glide to
 * the published ones, then the moving cutoff's bases. */
.equ CURRENT_BYTES, 336
.equ CURRENT_MIX_1_OFFSET, 128
.equ CURRENT_MIX_2_OFFSET, 192
.equ CURRENT_LP1_OFFSET, 256
.equ CURRENT_G1_OFFSET, 272          /* then K1, G2 and K2: the moving cutoff's bases, float */
.equ STATE_BYTES, 96
.equ SECTION_CURRENT_BYTES, 64
.equ SECTION_STATE_BYTES, 32
.equ WET_OFFSET, 64
.equ LP1_OFFSET, 80               /* section 1's lowpass, weighted, for the output; not a state */
.equ COEFFICIENTS_OFFSET, 16
.equ DYNAMIC_MASK_OFFSET, 272
.equ G1_OFFSET, 288
.equ K1_OFFSET, 304
.equ CV_INPUT_OFFSET, 320            /* byte offsets into CV_BASE */
.equ CV_SCALE_OFFSET, 336            /* octaves per count, float */
.equ G2_OFFSET, 352
.equ K2_OFFSET, 368
.equ MIX_2_LP1_OFFSET, 384
.equ FLAG_DYNAMIC_OFFSET, 400
.equ SMOOTH_PAIRS, 8                 /* the coefficients and both tap mixes, two vectors a pair */
.equ SMOOTH_SHIFT, 5
.equ GUARD_BITS, 5
.equ G_MIN_BITS, 0x3951b717      /* 0.0002f */
.equ G_MAX_BITS, 0x41800000      /* 16.0f */
.equ EXP_C1_BITS, 0x3f281062     /* 0.6565f */
.equ EXP_C2_BITS, 0x3eafdf3b     /* 0.3435f */
.equ CV_BASE, 0x9ffffe50         /* ENGINE_CALIBRATION_CV_SAMPLE_BASE: 24 halfwords, this block's */
.equ EXPONENT_BIAS_INTEGER, 103  /* 127 - 24, the offset added before the convert */
.equ WET_STEP, 0x00aaaaaa        /* full scale over 192 frames, 4 ms */
.equ SMOOTH_FRACTION_BITS, 0x3d000000 /* 1/32f, the float bases' step toward the published */

/* One section. Input x in q1; the section's current coefficients are at
 * \coef (a1 a2 a3 kq) and its states at \state (ic1 ic2). Leaves lowpass in
 * q12, bandpass in q13 and highpass in q7; q1 is unchanged. The lane mask is
 * in q0 and zeroes the states of idle lanes. */
.macro SECTION coef, state
    vld1.32 {d4-d5}, [\state:128]
    add r3, \state, #16
    vld1.32 {d6-d7}, [r3:128]
    vqsub.s32 q6, q1, q3
    vld1.32 {d16-d17}, [\coef:128]
    add r3, \coef, #16
    vld1.32 {d18-d19}, [r3:128]
    add r3, \coef, #32
    vld1.32 {d20-d21}, [r3:128]
    vqrdmulh.s32 q12, q8, q2
    vqrdmulh.s32 q13, q9, q6
    vqadd.s32 q13, q12, q13
    vqrdmulh.s32 q12, q9, q2
    vqrdmulh.s32 q11, q10, q6
    vqadd.s32 q12, q12, q3
    vqadd.s32 q12, q12, q11
    vqshl.s32 q11, q13, #1
    vqsub.s32 q2, q11, q2
    vqshl.s32 q11, q12, #1
    vqsub.s32 q3, q11, q3
    vand q2, q2, q0
    vand q3, q3, q0
    vst1.32 {d4-d5}, [\state:128]
    add r3, \state, #16
    vst1.32 {d6-d7}, [r3:128]
    add r3, \coef, #48
    vld1.32 {d16-d17}, [r3:128]
    vqrdmulh.s32 q11, q8, q13
    vqshl.s32 q11, q11, #2
    vqsub.s32 q7, q1, q11
    vqsub.s32 q7, q7, q12
.endm

/* The coefficients of both sections for the lanes whose cutoff moves: each
 * section's base g follows 2^(the cutoff CV's octaves). The trapezoidal
 * coefficients are worked out in float, with a Newton-refined reciprocal, and
 * the current coefficients at r1 take them for the lanes in the mask. */
.macro DYNAMIC_SECTION g_reg, k_offset, coefficients
    add r3, r1, #\k_offset
    vld1.32 {d20-d21}, [r3:128]
    vadd.f32 q8, \g_reg, q10
    vmul.f32 q8, q8, \g_reg
    vadd.f32 q8, q8, q13
    vrecpe.f32 q10, q8
    vrecps.f32 q12, q8, q10
    vmul.f32 q10, q10, q12
    vrecps.f32 q12, q8, q10
    vmul.f32 q10, q10, q12
    vmul.f32 q8, \g_reg, q10
    vmul.f32 q12, \g_reg, q8
    vcvt.s32.f32 q10, q10, #31
    vcvt.s32.f32 q8, q8, #31
    vcvt.s32.f32 q12, q12, #31
    add r3, r0, #DYNAMIC_MASK_OFFSET
    vld1.32 {d4-d5}, [r3:128]
    add r3, r1, #\coefficients
    vld1.32 {d12-d13}, [r3:128]
    vbsl q2, q10, q6
    vst1.32 {d4-d5}, [r3:128]
    add r3, r0, #DYNAMIC_MASK_OFFSET
    vld1.32 {d4-d5}, [r3:128]
    add r3, r1, #\coefficients+16
    vld1.32 {d12-d13}, [r3:128]
    vbsl q2, q8, q6
    vst1.32 {d4-d5}, [r3:128]
    add r3, r0, #DYNAMIC_MASK_OFFSET
    vld1.32 {d4-d5}, [r3:128]
    add r3, r1, #\coefficients+32
    vld1.32 {d12-d13}, [r3:128]
    vbsl q2, q12, q6
    vst1.32 {d4-d5}, [r3:128]
.endm
/* The moving cutoff's bases (G1 K1 G2 K2) close a thirty-second of the gap
 * to the published ones each frame, as the static coefficients do. */
.macro SMOOTH_BASE published, slot
    add r3, r0, #\published
    vld1.32 {d16-d17}, [r3:128]
    add r3, r1, #CURRENT_G1_OFFSET+\slot*16
    vld1.32 {d18-d19}, [r3:128]
    vsub.f32 q8, q8, q9
    vmla.f32 q9, q8, q12
    vst1.32 {d18-d19}, [r3:128]
.endm
.macro COPY_BASE published, slot
    add r3, r0, #\published
    vld1.32 {d16-d17}, [r3:128]
    add r3, r1, #CURRENT_G1_OFFSET+\slot*16
    vst1.32 {d16-d17}, [r3:128]
.endm
.macro DYNAMIC_COEFFICIENTS
    ldr r3, =SMOOTH_FRACTION_BITS
    vdup.32 q12, r3
    SMOOTH_BASE G1_OFFSET, 0
    SMOOTH_BASE K1_OFFSET, 1
    SMOOTH_BASE G2_OFFSET, 2
    SMOOTH_BASE K2_OFFSET, 3
    vmov.f32 q13, #1.0
    /* The cutoff CV: each lane's input this block, times its octaves per
     * count (zero for a lane without one, whose offset reads input 1A). */
    ldr r12, =CV_BASE
    add r3, r0, #CV_INPUT_OFFSET
    ldr r7, [r3]
    ldr r8, [r3, #4]
    ldrsh r7, [r12, r7]
    ldrsh r8, [r12, r8]
    vmov d20, r7, r8
    ldr r7, [r3, #8]
    ldr r8, [r3, #12]
    ldrsh r7, [r12, r7]
    ldrsh r8, [r12, r8]
    vmov d21, r7, r8
    vcvt.f32.s32 q10, q10
    add r3, r0, #CV_SCALE_OFFSET
    vld1.32 {d16-d17}, [r3:128]
    vmul.f32 q10, q10, q8
    /* m = 2^(CV octaves), as 2^n * (1 + f*(c1 + c2*f)); the offset keeps n
     * positive for the truncating convert. */
    vmov.f32 q8, #24.0
    vadd.f32 q9, q10, q8
    vcvt.s32.f32 q11, q9
    vcvt.f32.s32 q12, q11
    vsub.f32 q9, q9, q12
    ldr r3, =EXP_C2_BITS
    vdup.32 q12, r3
    ldr r3, =EXP_C1_BITS
    vdup.32 q8, r3
    vmla.f32 q8, q12, q9
    vmul.f32 q8, q8, q9
    vadd.f32 q8, q8, q13
    mov r3, #EXPONENT_BIAS_INTEGER
    vdup.32 q12, r3
    vadd.i32 q11, q11, q12
    vshl.i32 q11, q11, #23
    vmul.f32 q8, q8, q11
    /* Each section's g: its base * m. */
    add r3, r1, #CURRENT_G1_OFFSET
    vld1.32 {d18-d19}, [r3:128]
    vmul.f32 q9, q9, q8
    add r3, r1, #CURRENT_G1_OFFSET+32
    vld1.32 {d22-d23}, [r3:128]
    vmul.f32 q11, q11, q8
    ldr r3, =G_MIN_BITS
    vdup.32 q8, r3
    vmax.f32 q9, q9, q8
    vmax.f32 q11, q11, q8
    ldr r3, =G_MAX_BITS
    vdup.32 q8, r3
    vmin.f32 q9, q9, q8
    vmin.f32 q11, q11, q8
    vorr q3, q11, q11
    DYNAMIC_SECTION q9, CURRENT_G1_OFFSET+16, 0
    DYNAMIC_SECTION q3, CURRENT_G1_OFFSET+48, SECTION_CURRENT_BYTES
.endm

/* Four channels in \input. */
.macro FGROUP input, index
    add r0, r4, #\index*GROUP_BYTES
    add r1, r5, #\index*CURRENT_BYTES
    add r2, r6, #\index*STATE_BYTES
    vld1.32 {d0-d1}, [r0:128]
    add r3, r2, #WET_OFFSET
    vld1.32 {d2-d3}, [r3:128]
    vorr q2, q0, q1
    vorr d6, d4, d5
    vmov r3, r12, d6
    orrs r3, r3, r12
    bne .Lactive\@
    /* Idle: no state outlives the lanes' time off, and the next lane on
     * starts from the published coefficients. */
    vmov.i32 q1, #0
    mov r3, r2
    .rept STATE_BYTES/16
    vst1.32 {d2-d3}, [r3:128]!
    .endr
    add r3, r0, #COEFFICIENTS_OFFSET
    mov r12, r1
    .rept SMOOTH_PAIRS
    vld1.32 {d16-d19}, [r3:128]!
    vst1.32 {d16-d19}, [r12:128]!
    .endr
    add r3, r0, #MIX_2_LP1_OFFSET
    vld1.32 {d16-d17}, [r3:128]
    vst1.32 {d16-d17}, [r12:128]
    COPY_BASE G1_OFFSET, 0
    COPY_BASE K1_OFFSET, 1
    COPY_BASE G2_OFFSET, 2
    COPY_BASE K2_OFFSET, 3
    b .Ldone\@
.Lactive\@:
    /* The wet level rises toward full while the mask is on and falls to zero
     * while it is off; q0 becomes the lanes that run, on or still fading. */
    ldr r3, =WET_STEP
    vdup.32 q2, r3
    vqadd.s32 q3, q1, q2
    vqsub.s32 q6, q1, q2
    vmov.i32 q7, #0
    vmax.s32 q6, q6, q7
    vbsl q0, q3, q6
    add r3, r2, #WET_OFFSET
    vst1.32 {d0-d1}, [r3:128]
    vcgt.s32 q0, q0, #0
    vld1.32 {d2-d3}, [r0:128]
    vorr q0, q0, q1
    /* Move the current coefficients and tap mixes toward the published ones,
     * so a change of type crossfades between the taps instead of jumping
     * while the sections ring. */
    add r3, r0, #COEFFICIENTS_OFFSET
    mov r12, r1
    .rept SMOOTH_PAIRS
    vld1.32 {d16-d19}, [r12:128]
    vld1.32 {d20-d23}, [r3:128]!
    vsub.i32 q10, q10, q8
    vsub.i32 q11, q11, q9
    vshr.s32 q10, q10, #SMOOTH_SHIFT
    vshr.s32 q11, q11, #SMOOTH_SHIFT
    vadd.i32 q8, q8, q10
    vadd.i32 q9, q9, q11
    vst1.32 {d16-d19}, [r12:128]!
    .endr
    add r3, r0, #MIX_2_LP1_OFFSET
    vld1.32 {d16-d17}, [r12:128]
    vld1.32 {d20-d21}, [r3:128]
    vsub.i32 q10, q10, q8
    vshr.s32 q10, q10, #SMOOTH_SHIFT
    vadd.i32 q8, q8, q10
    vst1.32 {d16-d17}, [r12:128]
    ldr r3, [r0, #FLAG_DYNAMIC_OFFSET]
    cmp r3, #0
    bne .Ldynamic\@
    /* The bases follow the published ones, for when a lane's cutoff moves. */
    COPY_BASE G1_OFFSET, 0
    COPY_BASE K1_OFFSET, 1
    COPY_BASE G2_OFFSET, 2
    COPY_BASE K2_OFFSET, 3
    b .Lstatic\@
.Ldynamic\@:
    DYNAMIC_COEFFICIENTS
.Lstatic\@:
    vshr.s32 q1, \input, #GUARD_BITS
    add r7, r1, #SECTION_CURRENT_BYTES
    add r8, r2, #SECTION_STATE_BYTES
    SECTION r1, r2
    /* Section 1's lowpass as the output takes it. */
    add r3, r1, #CURRENT_LP1_OFFSET
    vld1.32 {d16-d17}, [r3:128]
    vqrdmulh.s32 q8, q8, q12
    add r3, r2, #LP1_OFFSET
    vst1.32 {d16-d17}, [r3:128]
    /* Section 2 takes a mix of section 1's input and taps. */
    add r3, r1, #CURRENT_MIX_1_OFFSET
    vld1.32 {d16-d19}, [r3:128]!
    vld1.32 {d20-d23}, [r3:128]
    vqrdmulh.s32 q8, q8, q1
    vqrdmulh.s32 q9, q9, q12
    vqrdmulh.s32 q10, q10, q13
    vqrdmulh.s32 q11, q11, q7
    vqadd.s32 q8, q8, q9
    vqadd.s32 q10, q10, q11
    vqadd.s32 q1, q8, q10
    vqshl.s32 q1, q1, #1
    SECTION r7, r8
    /* The output mixes section 2's input and taps, then fades from the dry
     * sample by the wet level; lanes that do not run keep their sample. */
    add r3, r1, #CURRENT_MIX_2_OFFSET
    vld1.32 {d16-d19}, [r3:128]!
    vld1.32 {d20-d23}, [r3:128]
    vqrdmulh.s32 q8, q8, q1
    vqrdmulh.s32 q9, q9, q12
    vqrdmulh.s32 q10, q10, q13
    vqrdmulh.s32 q11, q11, q7
    vqadd.s32 q8, q8, q9
    vqadd.s32 q10, q10, q11
    vqadd.s32 q8, q8, q10
    add r3, r2, #LP1_OFFSET
    vld1.32 {d18-d19}, [r3:128]
    vqadd.s32 q8, q8, q9
    vqshl.s32 q8, q8, #GUARD_BITS+1
    add r3, r2, #WET_OFFSET
    vld1.32 {d18-d19}, [r3:128]
    vqsub.s32 q10, q8, \input
    vqrdmulh.s32 q10, q10, q9
    vqadd.s32 q10, q10, \input
    vbsl q0, q10, \input
    vorr \input, q0, q0
.Ldone\@:
.endm

patch_filter:
    ldr r0, =filter_mailbox
    ldr r4, [r0, #4]
    cmp r4, #0
    beq .Lpublished
    str r4, [r0]
    dmb sy
    mov r1, #0
    str r1, [r0, #4]
.Lpublished:
    ldr r4, [r0]
    cmp r4, #0
    bxeq lr
    ldr r5, =filter_current
    ldr r6, =filter_state
    FGROUP q4, 0
    FGROUP q5, 1
    bx lr
.ltorg
