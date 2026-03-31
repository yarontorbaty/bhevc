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

#ifndef AOM_AV1_ENCODER_AQ_EDGE_H_
#define AOM_AV1_ENCODER_AQ_EDGE_H_

#include "av1/encoder/encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Brain-inspired edge-aware adaptive quantization.
 *
 * Segment assignments (lower = higher priority = lower QP):
 *   0 = CONTOUR_COHERENT: uniform ON/OFF polarity (text, UI, line art)
 *   1 = CONTOUR_MIXED:    high edge density, random polarity (natural edges)
 *   2 = TEXTURE:          moderate edge density
 *   3 = NORMAL:           low-moderate edge density
 *   4 = FLAT:             smooth, minimal edges
 */
#define EDGE_SEG_CONTOUR_COHERENT 0
#define EDGE_SEG_CONTOUR_MIXED    1
#define EDGE_SEG_TEXTURE          2
#define EDGE_SEG_NORMAL           3
#define EDGE_SEG_FLAT             4

#define NUM_ORI_BINS 8

#if !CONFIG_REALTIME_ONLY
void av1_edge_aq_frame_setup(AV1_COMP *cpi);

int av1_edge_block_score(const AV1_COMP *cpi, const MACROBLOCK *x,
                         BLOCK_SIZE bs, int mi_row, int mi_col);

/*
 * Returns dominant orientation bin (0-7) for the block, or -1 if
 * orientation is weak (no dominant direction). Requires EDGE_AQ.
 */
int av1_edge_block_orientation(const AV1_COMP *cpi, const MACROBLOCK *x,
                               BLOCK_SIZE bs, int mi_row, int mi_col);
#endif

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AOM_AV1_ENCODER_AQ_EDGE_H_
