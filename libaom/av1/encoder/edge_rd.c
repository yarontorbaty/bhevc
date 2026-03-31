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

/*
 * Structure-aware RD cost for brain-inspired video encoding.
 *
 * Adds an edge-preservation penalty to the standard SSE-based distortion
 * metric used in AV1 rate-distortion optimization. The penalty is the
 * Sobel gradient magnitude mismatch between source and reconstruction,
 * weighted by a QP-dependent factor.
 *
 * This implements the principle from thalamocortical visual processing:
 * the visual system is disproportionately sensitive to disruptions in
 * edge structure (ON/OFF boundaries). A prediction that achieves low
 * pixel-domain error but breaks an edge is perceptually worse than one
 * that preserves the contour.
 *
 * Integration into rdopt.c:
 *
 *   In adjust_rdcost(), add after the existing sharpness-3 path:
 *
 *     #if !CONFIG_REALTIME_ONLY
 *     if (cpi->oxcf.q_cfg.aq_mode == EDGE_AQ && is_inter_pred) {
 *       av1_edge_rd_adjust(cpi, x, rd_cost);
 *       return;
 *     }
 *     #endif
 *
 *   Similarly in adjust_cost(), add:
 *
 *     #if !CONFIG_REALTIME_ONLY
 *     if (cpi->oxcf.q_cfg.aq_mode == EDGE_AQ && is_inter_pred) {
 *       *rd_cost += av1_edge_rd_cost_offset(cpi, x);
 *       return;
 *     }
 *     #endif
 *
 * Reference:
 *   "Thalamic activation of the visual cortex at the single-synapse level"
 *   Science 391, 1349 (2026)
 */

#include <stdlib.h>

#include "aom_dsp/aom_dsp_common.h"
#include "aom_ports/mem.h"

#include "av1/common/blockd.h"
#include "av1/common/common.h"
#include "av1/encoder/aq_cyclicrefresh.h"
#include "av1/encoder/edge_rd.h"
#include "av1/encoder/aq_edge.h"
#include "av1/encoder/encoder.h"
#include "av1/encoder/rd.h"
#include "config/aom_config.h"

#if !CONFIG_REALTIME_ONLY

/*
 * QP thresholds for the alpha ramp (linear interpolation zone).
 * Below ALPHA_QIDX_LOW, no edge penalty is applied.
 * Above ALPHA_QIDX_HIGH, the maximum edge penalty is applied.
 */
#define ALPHA_QIDX_LOW  80
#define ALPHA_QIDX_HIGH 180

/*
 * Maximum alpha in Q8 fixed point. 64 = 0.25, meaning edge mismatch
 * contributes at most 25% of the distortion weight relative to SSE.
 *
 * This value was chosen conservatively:
 * - Too high: biases the encoder toward preserving edges at the expense
 *   of overall PSNR, causing texture quality to degrade.
 * - Too low: edge-preservation effect is negligible.
 * - 0.25 provides a meaningful signal without dominating SSE.
 */
#define ALPHA_MAX_Q8 16

/*
 * Sobel gradient magnitude at a single pixel.
 * Returns the integer approximation sqrt(gx^2 + gy^2) using the fast
 * |gx| + |gy| approximation to avoid the sqrt.
 */
/*
 * Signed gradient mismatch: compares per-component Sobel gradients between
 * source and reconstruction. Unlike magnitude-only comparison, this catches
 * polarity inversions (ON<->OFF) which cause ringing/halo artifacts.
 *
 * |src_gx - rec_gx| + |src_gy - rec_gy| >= ||src_mag| - |rec_mag||
 * with equality only when polarities match.
 */

#define SOBEL_GX(buf, stride, r, c)                                \
  (-(int)(buf)[(r - 1) * (stride) + (c - 1)] +                    \
   (int)(buf)[(r - 1) * (stride) + (c + 1)] -                     \
   2 * (int)(buf)[(r) * (stride) + (c - 1)] +                     \
   2 * (int)(buf)[(r) * (stride) + (c + 1)] -                     \
   (int)(buf)[(r + 1) * (stride) + (c - 1)] +                     \
   (int)(buf)[(r + 1) * (stride) + (c + 1)])

#define SOBEL_GY(buf, stride, r, c)                                \
  (-(int)(buf)[(r - 1) * (stride) + (c - 1)] -                    \
   2 * (int)(buf)[(r - 1) * (stride) + (c)] -                     \
   (int)(buf)[(r - 1) * (stride) + (c + 1)] +                     \
   (int)(buf)[(r + 1) * (stride) + (c - 1)] +                     \
   2 * (int)(buf)[(r + 1) * (stride) + (c)] +                     \
   (int)(buf)[(r + 1) * (stride) + (c + 1)])

int64_t av1_compute_edge_mismatch(const uint8_t *src_buf, int src_stride,
                                  const uint8_t *rec_buf, int rec_stride,
                                  int bw, int bh, int hbd, int bit_depth) {
  int64_t mismatch = 0;

  if (bw < 4 || bh < 4) return 0;

  if (hbd) {
    const uint16_t *src16 = CONVERT_TO_SHORTPTR(src_buf);
    const uint16_t *rec16 = CONVERT_TO_SHORTPTR(rec_buf);

    for (int r = 1; r < bh - 1; r++) {
      for (int c = 1; c < bw - 1; c++) {
        int src_gx = SOBEL_GX(src16, src_stride, r, c);
        int src_gy = SOBEL_GY(src16, src_stride, r, c);
        int rec_gx = SOBEL_GX(rec16, rec_stride, r, c);
        int rec_gy = SOBEL_GY(rec16, rec_stride, r, c);
        mismatch += (int64_t)abs(src_gx - rec_gx) +
                    (int64_t)abs(src_gy - rec_gy);
      }
    }

    int shift = bit_depth - 8;
    if (shift > 0) mismatch >>= shift;
  } else {
    (void)bit_depth;
    for (int r = 1; r < bh - 1; r++) {
      for (int c = 1; c < bw - 1; c++) {
        int src_gx = SOBEL_GX(src_buf, src_stride, r, c);
        int src_gy = SOBEL_GY(src_buf, src_stride, r, c);
        int rec_gx = SOBEL_GX(rec_buf, rec_stride, r, c);
        int rec_gy = SOBEL_GY(rec_buf, rec_stride, r, c);
        mismatch += (int64_t)abs(src_gx - rec_gx) +
                    (int64_t)abs(src_gy - rec_gy);
      }
    }
  }

  return mismatch;
}

#undef SOBEL_GX
#undef SOBEL_GY

int av1_edge_rd_alpha(int qindex, int bit_depth) {
  (void)bit_depth;

  if (qindex < ALPHA_QIDX_LOW) return 0;
  if (qindex > ALPHA_QIDX_HIGH) return ALPHA_MAX_Q8;

  /*
   * Linear ramp from 0 to ALPHA_MAX_Q8 across [ALPHA_QIDX_LOW, ALPHA_QIDX_HIGH].
   * Using integer arithmetic: alpha = ALPHA_MAX_Q8 * (q - low) / (high - low)
   */
  return ALPHA_MAX_Q8 * (qindex - ALPHA_QIDX_LOW) /
         (ALPHA_QIDX_HIGH - ALPHA_QIDX_LOW);
}

void av1_edge_rd_adjust(const AV1_COMP *cpi, const MACROBLOCK *x,
                        RD_STATS *rd_cost) {
  if (cpi->oxcf.q_cfg.aq_mode != EDGE_AQ) return;
  if (rd_cost->rate == INT_MAX || rd_cost->dist == INT64_MAX) return;

  const MACROBLOCKD *xd = &x->e_mbd;
  const MB_MODE_INFO *mbmi = xd->mi[0];

  if (!is_inter_block(mbmi)) return;

  /* Only adjust CONTOUR blocks -- texture and flat blocks don't benefit
   * from edge preservation penalty (texture has high visual masking,
   * flat has negligible gradients). */
  if (mbmi->segment_id > EDGE_SEG_CONTOUR_MIXED) return;

  const int hbd = is_cur_buf_hbd(xd);
  const int bit_depth = cpi->common.seq_params->bit_depth;
  const int qindex = cyclic_refresh_segment_id_boosted(mbmi->segment_id)
                         ? cpi->common.quant_params.base_qindex
                         : av1_get_qindex(&cpi->common.seg, mbmi->segment_id,
                                          cpi->common.quant_params.base_qindex);

  int alpha_q8 = av1_edge_rd_alpha(qindex, bit_depth);
  if (alpha_q8 == 0) return;

  /*
   * Compute block dimensions. The source buffer x->plane[0].src.buf is
   * already offset to the block origin. The destination (reconstruction)
   * is xd->plane[0].dst.buf.
   */
  const BLOCK_SIZE bsize = mbmi->bsize;
  const int bw = block_size_wide[bsize];
  const int bh = block_size_high[bsize];

  if (bw < 4 || bh < 4) return;

  const uint8_t *src_buf = x->plane[0].src.buf;
  const int src_stride = x->plane[0].src.stride;
  const uint8_t *rec_buf = xd->plane[0].dst.buf;
  const int rec_stride = xd->plane[0].dst.stride;

  int64_t edge_mismatch = av1_compute_edge_mismatch(
      src_buf, src_stride, rec_buf, rec_stride, bw, bh, hbd, bit_depth);

  if (edge_mismatch == 0) return;

  /*
   * Normalize per interior pixel, then apply alpha weight.
   *
   * Without normalization, the raw sum scales with block area, causing
   * large blocks to have mismatch values that dominate SSE. Dividing by
   * (bw-2)*(bh-2) — the Sobel interior pixel count — makes the penalty
   * block-size-independent, then we scale back by total block area so
   * the offset is in the same units as SSE (which is also a sum).
   */
  const int interior = (bw - 2) * (bh - 2);
  int64_t per_pixel_mismatch = edge_mismatch / interior;
  int64_t dist_offset = (per_pixel_mismatch * bw * bh *
                          (int64_t)alpha_q8) >> 8;

  /*
   * Clamp the edge penalty to at most 50% of the original distortion.
   * This prevents edge mismatch from overwhelming the SSE signal and
   * causing pathological mode decisions.
   */
  if (rd_cost->dist > 0 && dist_offset > rd_cost->dist / 2) {
    dist_offset = rd_cost->dist / 2;
  }

  rd_cost->dist += dist_offset;
  rd_cost->rdcost = RDCOST(x->rdmult, rd_cost->rate, rd_cost->dist);
}

#endif /* !CONFIG_REALTIME_ONLY */
