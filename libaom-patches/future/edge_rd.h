/*
 * Copyright (c) 2026, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#ifndef AOM_AV1_ENCODER_EDGE_RD_H_
#define AOM_AV1_ENCODER_EDGE_RD_H_

#include <stdint.h>

#include "av1/encoder/block.h"
#include "av1/encoder/encoder.h"
#include "av1/encoder/rd.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Structure-aware RD cost adjustment for brain-inspired video encoding.
 *
 * Modifies the standard RD cost:
 *     RD = D + lambda * R
 * where D = SSE, to include an edge-preservation penalty:
 *     D_brain = SSE + alpha * edge_mismatch
 *
 * edge_mismatch is computed as the sum of absolute differences in Sobel
 * gradient magnitude between the source and reconstructed blocks. This
 * penalizes predictions that break, blur, or shift edge structure.
 *
 * The penalty weight alpha scales with QP: higher QP (lower quality) gets a
 * larger penalty because edge preservation becomes more perceptually
 * important as overall quality decreases. At very low QP, SSE already
 * captures edge accuracy well enough.
 *
 * Integration:
 *   Hooks into the existing adjust_rdcost() pathway in rdopt.c, activated
 *   only when EDGE_AQ (--aq-mode=4) is enabled.
 *
 * Reference:
 *   "Thalamic activation of the visual cortex at the single-synapse level"
 *   Science 391, 1349 (2026)
 */

#if !CONFIG_REALTIME_ONLY

/*
 * Compute edge-structure mismatch between source and reconstructed blocks.
 *
 * Applies a 3x3 Sobel operator to both buffers, computes gradient magnitude
 * at each pixel, and returns the sum of absolute differences in magnitude.
 * Handles both 8-bit and HBD (high bit depth) buffers.
 *
 * This measures how much edge structure has been lost or displaced by the
 * prediction + transform coding pipeline.
 *
 * @param src_buf       Source luma buffer (block-relative, already offset).
 * @param src_stride    Source buffer stride.
 * @param rec_buf       Reconstructed/predicted luma buffer.
 * @param rec_stride    Reconstructed buffer stride.
 * @param bw            Block width in pixels.
 * @param bh            Block height in pixels.
 * @param hbd           Non-zero if buffers are high bit depth.
 * @param bit_depth     Bit depth (8, 10, or 12).
 * @return              Sum of |Sobel_src - Sobel_rec| over the block interior.
 */
int64_t av1_compute_edge_mismatch(const uint8_t *src_buf, int src_stride,
                                  const uint8_t *rec_buf, int rec_stride,
                                  int bw, int bh, int hbd, int bit_depth);

/*
 * Compute the QP-dependent edge penalty weight (alpha).
 *
 * Returns a fixed-point weight (Q8 format, i.e., 256 = 1.0) that scales
 * the edge mismatch penalty relative to SSE distortion.
 *
 * The weight follows a piecewise-linear ramp:
 *   - qindex < 80:   alpha = 0      (low QP, SSE is sufficient)
 *   - qindex 80-180: alpha ramps from 0 to alpha_max
 *   - qindex > 180:  alpha = alpha_max
 *
 * alpha_max is configurable but defaults to 64 (0.25 in Q8), meaning
 * edge mismatch contributes up to 25% of the total distortion signal
 * at high QP.
 *
 * @param qindex        Current quantizer index (0-255).
 * @param bit_depth     Bit depth.
 * @return              Alpha weight in Q8 fixed point.
 */
int av1_edge_rd_alpha(int qindex, int bit_depth);

/*
 * Apply structure-aware RD cost adjustment.
 *
 * This is the top-level function called from adjust_rdcost() in rdopt.c.
 * It computes the edge mismatch for the current block and adds the
 * weighted penalty to rd_cost->dist, then recomputes rd_cost->rdcost.
 *
 * Only applies when:
 *   - EDGE_AQ is active (aq_mode == 4)
 *   - The block is inter-predicted (edge-aware intra mode selection uses
 *     different mechanisms via QP modulation)
 *   - The block is large enough for Sobel (>= 4x4 interior)
 *
 * @param cpi           Top-level encoder structure.
 * @param x             Macroblock data (contains source and reconstruction).
 * @param rd_cost       RD statistics to adjust in place.
 */
void av1_edge_rd_adjust(const AV1_COMP *cpi, const MACROBLOCK *x,
                        RD_STATS *rd_cost);

#endif  /* !CONFIG_REALTIME_ONLY */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* AOM_AV1_ENCODER_EDGE_RD_H_ */
