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

#include <math.h>
#include <stdlib.h>

#include "aom_dsp/aom_dsp_common.h"
#include "aom_ports/mem.h"

#include "av1/encoder/aq_edge.h"
#include "av1/common/seg_common.h"
#include "av1/encoder/ratectrl.h"
#include "av1/encoder/rd.h"
#include "av1/encoder/segmentation.h"
#include "config/aom_config.h"

#if !CONFIG_REALTIME_ONLY

static const double edge_rate_ratio[MAX_SEGMENTS] = {
    1.0, 1.0, 0.85, 0.92, 1.05, 1.0, 1.0, 1.0
};

#define EDGE_THRESH_HIGH  0.18
#define EDGE_THRESH_MED   0.08
#define EDGE_THRESH_LOW   0.03

void av1_edge_aq_frame_setup(AV1_COMP *cpi) {
  AV1_COMMON *cm = &cpi->common;
  const RefreshFrameInfo *const refresh_frame = &cpi->refresh_frame;
  const int base_qindex = cm->quant_params.base_qindex;
  struct segmentation *seg = &cm->seg;

  int resolution_change =
      cm->prev_frame && (cm->width != cm->prev_frame->width ||
                         cm->height != cm->prev_frame->height);

  if (resolution_change) {
    memset(cpi->enc_seg.map, 0,
           cm->mi_params.mi_rows * cm->mi_params.mi_cols);
    av1_clearall_segfeatures(seg);
    av1_disable_segmentation(seg);
    return;
  }

  if (frame_is_intra_only(cm) || cm->features.error_resilient_mode ||
      refresh_frame->alt_ref_frame ||
      (refresh_frame->golden_frame && !cpi->rc.is_src_frame_alt_ref)) {
    cpi->vaq_refresh = 1;

    av1_enable_segmentation(seg);
    av1_clearall_segfeatures(seg);

    const double avg_ratio = edge_rate_ratio[EDGE_SEG_TEXTURE];

    for (int i = 0; i < MAX_SEGMENTS; ++i) {
      int qindex_delta = av1_compute_qdelta_by_rate(
          cpi, cm->current_frame.frame_type, base_qindex,
          edge_rate_ratio[i] / avg_ratio);

      if ((base_qindex != 0) && ((base_qindex + qindex_delta) == 0)) {
        qindex_delta = -base_qindex + 1;
      }

      av1_set_segdata(seg, i, SEG_LVL_ALT_Q, qindex_delta);
      av1_enable_segfeature(seg, i, SEG_LVL_ALT_Q);
    }
  }
}

int av1_edge_block_score(const AV1_COMP *cpi, const MACROBLOCK *x,
                         BLOCK_SIZE bs, int mi_row, int mi_col) {
  const MACROBLOCKD *xd = &x->e_mbd;
  const int pic_w = cpi->common.width;
  const int pic_h = cpi->common.height;
  const int bw = MI_SIZE * mi_size_wide[bs];
  const int bh = MI_SIZE * mi_size_high[bs];
  const int stride = x->plane[0].src.stride;
  const int abs_row = mi_row << MI_SIZE_LOG2;
  const int abs_col = mi_col << MI_SIZE_LOG2;

  if (bw < 4 || bh < 4) return EDGE_SEG_NORMAL;

  int edge_count = 0;
  int total_count = 0;
  int64_t sum_gx = 0, sum_gy = 0;
  int64_t abs_sum = 0;

  const int hbd = is_cur_buf_hbd(xd);
  const uint8_t *buf8 = x->plane[0].src.buf;
  const uint16_t *buf16 = hbd ? CONVERT_TO_SHORTPTR(buf8) : NULL;
  const int sobel_thresh =
      hbd ? (30 << (cpi->common.seq_params->bit_depth - 8)) : 30;
  const int64_t thresh_sq = (int64_t)sobel_thresh * sobel_thresh;

#define PIX(r, c) \
  (hbd ? (int)buf16[(r) * stride + (c)] : (int)buf8[(r) * stride + (c)])

  for (int r = 1; r < bh - 1 && (abs_row + r) < pic_h - 1; r++) {
    for (int c = 1; c < bw - 1 && (abs_col + c) < pic_w - 1; c++) {
      int gx = -PIX(r - 1, c - 1) + PIX(r - 1, c + 1) -
               2 * PIX(r, c - 1) + 2 * PIX(r, c + 1) -
               PIX(r + 1, c - 1) + PIX(r + 1, c + 1);

      int gy = -PIX(r - 1, c - 1) - 2 * PIX(r - 1, c) -
               PIX(r - 1, c + 1) + PIX(r + 1, c - 1) +
               2 * PIX(r + 1, c) + PIX(r + 1, c + 1);

      int64_t mag_sq = (int64_t)gx * gx + (int64_t)gy * gy;
      if (mag_sq > thresh_sq) {
        edge_count++;
        sum_gx += gx;
        sum_gy += gy;
        abs_sum += abs(gx) + abs(gy);
      }
      total_count++;
    }
  }

#undef PIX

  if (total_count == 0) return EDGE_SEG_NORMAL;

  double density = (double)edge_count / total_count;

  if (density > EDGE_THRESH_HIGH) {
    /*
     * Polarity coherence: |vector_sum| / scalar_sum.
     * High coherence means edges have consistent direction (text, UI lines).
     * Low coherence means edges point in many directions (natural texture).
     */
    if (abs_sum > 0) {
      int64_t vec_mag_sq = sum_gx * sum_gx + sum_gy * sum_gy;
      int64_t abs_sum_sq = abs_sum * abs_sum;
      if (vec_mag_sq * 4 > abs_sum_sq)
        return EDGE_SEG_CONTOUR_COHERENT;
    }
    return EDGE_SEG_CONTOUR_MIXED;
  }
  if (density > EDGE_THRESH_MED) return EDGE_SEG_TEXTURE;
  if (density > EDGE_THRESH_LOW) return EDGE_SEG_NORMAL;
  return EDGE_SEG_FLAT;
}

int av1_edge_block_orientation(const AV1_COMP *cpi, const MACROBLOCK *x,
                               BLOCK_SIZE bs, int mi_row, int mi_col) {
  const MACROBLOCKD *xd = &x->e_mbd;
  const int pic_w = cpi->common.width;
  const int pic_h = cpi->common.height;
  const int bw = MI_SIZE * mi_size_wide[bs];
  const int bh = MI_SIZE * mi_size_high[bs];
  const int stride = x->plane[0].src.stride;
  const int abs_row = mi_row << MI_SIZE_LOG2;
  const int abs_col = mi_col << MI_SIZE_LOG2;

  if (bw < 4 || bh < 4) return -1;

  const int hbd = is_cur_buf_hbd(xd);
  const uint8_t *buf8 = x->plane[0].src.buf;
  const uint16_t *buf16 = hbd ? CONVERT_TO_SHORTPTR(buf8) : NULL;
  const int sobel_thresh =
      hbd ? (30 << (cpi->common.seq_params->bit_depth - 8)) : 30;
  const int64_t thresh_sq = (int64_t)sobel_thresh * sobel_thresh;

  int ori_hist[NUM_ORI_BINS] = { 0 };
  int edge_count = 0;

#define PIX_ORI(r, c) \
  (hbd ? (int)buf16[(r) * stride + (c)] : (int)buf8[(r) * stride + (c)])

  for (int r = 1; r < bh - 1 && (abs_row + r) < pic_h - 1; r++) {
    for (int c = 1; c < bw - 1 && (abs_col + c) < pic_w - 1; c++) {
      int gx = -PIX_ORI(r - 1, c - 1) + PIX_ORI(r - 1, c + 1) -
               2 * PIX_ORI(r, c - 1) + 2 * PIX_ORI(r, c + 1) -
               PIX_ORI(r + 1, c - 1) + PIX_ORI(r + 1, c + 1);

      int gy = -PIX_ORI(r - 1, c - 1) - 2 * PIX_ORI(r - 1, c) -
               PIX_ORI(r - 1, c + 1) + PIX_ORI(r + 1, c - 1) +
               2 * PIX_ORI(r + 1, c) + PIX_ORI(r + 1, c + 1);

      int64_t mag_sq = (int64_t)gx * gx + (int64_t)gy * gy;
      if (mag_sq <= thresh_sq) continue;
      edge_count++;

      /*
       * Map gradient direction to 8 octant bins using integer arithmetic.
       * Gradient angle = atan2(gy, gx). We classify into 8 bins of 45 deg
       * each using comparisons against gx, gy, and their combinations.
       */
      int agx = abs(gx), agy = abs(gy);
      int bin;
      if (agx > 2 * agy)
        bin = (gx > 0) ? 0 : 4;
      else if (agy > 2 * agx)
        bin = (gy > 0) ? 2 : 6;
      else if (gx > 0)
        bin = (gy > 0) ? 1 : 7;
      else
        bin = (gy > 0) ? 3 : 5;

      ori_hist[bin]++;
    }
  }

#undef PIX_ORI

  if (edge_count < 4) return -1;

  int best_bin = 0, best_count = ori_hist[0];
  for (int i = 1; i < NUM_ORI_BINS; i++) {
    if (ori_hist[i] > best_count) {
      best_count = ori_hist[i];
      best_bin = i;
    }
  }

  if (best_count < edge_count / 3) return -1;

  return best_bin;
}

#endif  // !CONFIG_REALTIME_ONLY
