/* The stock eight-channel sinc convolution with PCM preload hints.
 *
 * The convolution's instructions are Rossum's and are not in this file. The
 * space below is filled when the patch is applied (patch_layout.json,
 * "splices"): the stock routine is copied from the user's own software file
 * in nine pieces, with the preload block from this file between them, once
 * before each channel's 128-tap kernel. The routine uses no literals and
 * branches only within a piece, so it runs unchanged from here.
 *
 * At each insertion r6 addresses the channel's 128 consecutive Q31 PCM
 * words, already selected by the stock PM and boundary calculations. Each
 * hint stays inside that span; offset 508 covers its last cache line when
 * the PCM address is only word aligned. No cache policy is changed, and the
 * arithmetic, rounding and interpolation are the stock routine's own.
 */
.syntax unified
.cpu cortex-a8
.arm
.text
.equ CONVOLUTION_BYTES, 1900                  @ nine stock pieces and eight preload blocks
.equ PCM_CACHE_LINE_BYTES, 64
.equ PCM_LAST_WORD_LINE, 508                  @ the last line of 128 words from a word-aligned address
.global patch_audio_convolve_channel_0, patch_audio_convolve_end
patch_audio_convolve_channel_0:
    .space CONVOLUTION_BYTES
patch_audio_convolve_end:
    b patch_grain_mix_stage

/* The preload block: not part of the image as it stands, only of the splice. */
.section .splice.preload, "a", %progbits
    pld [r6, #(0 * PCM_CACHE_LINE_BYTES)]
    pld [r6, #(1 * PCM_CACHE_LINE_BYTES)]
    pld [r6, #(2 * PCM_CACHE_LINE_BYTES)]
    pld [r6, #(3 * PCM_CACHE_LINE_BYTES)]
    pld [r6, #(4 * PCM_CACHE_LINE_BYTES)]
    pld [r6, #(5 * PCM_CACHE_LINE_BYTES)]
    pld [r6, #(6 * PCM_CACHE_LINE_BYTES)]
    pld [r6, #(7 * PCM_CACHE_LINE_BYTES)]
    pld [r6, #PCM_LAST_WORD_LINE]
