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

#ifndef AOM_AV1_ENCODER_EDGE_MOTION_SEARCH_H_
#define AOM_AV1_ENCODER_EDGE_MOTION_SEARCH_H_

#include "av1/encoder/encoder.h"
#include "av1/encoder/mcomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Brain-inspired edge-guided motion search effort modulation.
 *
 * Uses the edge density score from av1_edge_block_score() to adapt
 * motion search effort per superblock:
 *   - FLAT blocks:    reduce search range + skip fractional refinement
 *   - EDGE blocks:    increase search range + always do 1/8-pel refinement
 *   - TEXTURE blocks: use default search parameters
 *
 * This saves encoder time on flat regions (where motion search adds little
 * value) and improves quality on edge regions (where motion accuracy matters
 * most for perceptual quality).
 *
 * Requires EDGE_AQ (--aq-mode=4) to be active.
 *
 * Reference:
 *   "Thalamic activation of the visual cortex at the single-synapse level"
 *   Science 391, 1349 (2026)
 */

#if !CONFIG_REALTIME_ONLY

/*
 * Adjust fullpel motion search step parameter based on edge classification.
 *
 * Called after step_param is initially computed in av1_single_motion_search().
 * Returns the adjusted step_param.
 *
 * @param cpi           Top-level encoder structure.
 * @param edge_seg      Edge segment classification from av1_edge_block_score().
 *                      One of EDGE_SEG_CONTOUR, EDGE_SEG_TEXTURE,
 *                      EDGE_SEG_NORMAL, EDGE_SEG_FLAT.
 * @param base_step     The step_param value computed by normal logic.
 * @return              Adjusted step_param.
 */
int av1_edge_adjust_step_param(const AV1_COMP *cpi, int edge_seg,
                               int base_step);

/*
 * Adjust subpel search parameters based on edge classification.
 *
 * Called after av1_make_default_subpel_ms_params() to override forced_stop
 * and iters_per_step for edge-guided motion search.
 *
 * @param ms_params     Subpel motion search parameters to modify in place.
 * @param edge_seg      Edge segment classification.
 */
void av1_edge_adjust_subpel_params(SUBPEL_MOTION_SEARCH_PARAMS *ms_params,
                                   int edge_seg);

/*
 * Determine whether to skip fractional motion search entirely for flat blocks.
 *
 * When the source block is classified as FLAT by the edge detector, fractional
 * refinement rarely helps (the prediction error surface is smooth). Skipping
 * it saves significant encoder time.
 *
 * @param cpi           Top-level encoder structure.
 * @param edge_seg      Edge segment classification.
 * @return              1 if fractional search should be skipped, 0 otherwise.
 */
int av1_edge_skip_fractional_search(const AV1_COMP *cpi, int edge_seg);

/*
 * Get the edge segment classification for the current block.
 *
 * When EDGE_AQ is active, the segment_id has already been set by
 * setup_block_rdmult(). This helper retrieves it safely.
 *
 * @param cpi           Top-level encoder structure.
 * @param mbmi          Mode info for the current block.
 * @return              Edge segment classification, or EDGE_SEG_NORMAL
 *                      if EDGE_AQ is not active.
 */
int av1_get_edge_segment(const AV1_COMP *cpi, const MB_MODE_INFO *mbmi);

#endif  /* !CONFIG_REALTIME_ONLY */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* AOM_AV1_ENCODER_EDGE_MOTION_SEARCH_H_ */
