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
 * Edge-guided motion search effort modulation for AV1.
 *
 * Adapts motion search parameters per-block based on the edge density
 * classification from av1_edge_block_score(). Implements the brain-inspired
 * principle that the visual system allocates more processing resources to
 * contour/edge regions than to flat/uniform regions.
 *
 * Integration points in libaom:
 *   1. motion_search_facade.c :: av1_single_motion_search()
 *      After step_param is computed (~line 153), insert:
 *        if (av1_get_edge_segment(cpi, mbmi) != EDGE_SEG_NORMAL)
 *          step_param = av1_edge_adjust_step_param(cpi, edge_seg, step_param);
 *
 *   2. motion_search_facade.c :: av1_single_motion_search()
 *      After av1_make_default_subpel_ms_params() (~line 408), insert:
 *        av1_edge_adjust_subpel_params(&ms_params, edge_seg);
 *
 *   3. motion_search_facade.c :: av1_single_motion_search()
 *      Before fractional search (~line 398), insert:
 *        if (av1_edge_skip_fractional_search(cpi, edge_seg))
 *          use_fractional_mv = 0;
 *
 * Reference:
 *   "Thalamic activation of the visual cortex at the single-synapse level"
 *   Science 391, 1349 (2026)
 */

#include "av1/encoder/edge_motion_search.h"

#include "av1/encoder/aq_edge.h"
#include "av1/encoder/encoder.h"
#include "av1/encoder/mcomp.h"
#include "av1/encoder/speed_features.h"

#if !CONFIG_REALTIME_ONLY

/* Segment IDs from aq_edge.c */
#define EDGE_SEG_CONTOUR 0
#define EDGE_SEG_TEXTURE 1
#define EDGE_SEG_NORMAL  2
#define EDGE_SEG_FLAT    3

int av1_get_edge_segment(const AV1_COMP *cpi, const MB_MODE_INFO *mbmi) {
  if (cpi->oxcf.q_cfg.aq_mode != EDGE_AQ) return EDGE_SEG_NORMAL;
  if (!cpi->vaq_refresh) return EDGE_SEG_NORMAL;
  return mbmi->segment_id;
}

/*
 * step_param controls the initial search radius in full-pixel motion search.
 * Lower step_param = larger search range (step 0 is the maximum range).
 * Higher step_param = smaller search range (faster but less accurate).
 *
 * Adjustment table (from base step_param):
 *   EDGE_SEG_CONTOUR:  -2  (double the search range)
 *   EDGE_SEG_TEXTURE:  -1  (slightly wider search)
 *   EDGE_SEG_NORMAL:    0  (no change)
 *   EDGE_SEG_FLAT:     +2  (halve the search range)
 */
static const int edge_step_delta[4] = { -2, -1, 0, 2 };

int av1_edge_adjust_step_param(const AV1_COMP *cpi, int edge_seg,
                               int base_step) {
  (void)cpi;
  if (edge_seg < 0 || edge_seg > EDGE_SEG_FLAT) return base_step;

  int adjusted = base_step + edge_step_delta[edge_seg];

  /* Clamp: step_param 0 = max range, MAX_MVSEARCH_STEPS-2 = minimum range */
  if (adjusted < 0) adjusted = 0;
  if (adjusted > MAX_MVSEARCH_STEPS - 2) adjusted = MAX_MVSEARCH_STEPS - 2;

  return adjusted;
}

void av1_edge_adjust_subpel_params(SUBPEL_MOTION_SEARCH_PARAMS *ms_params,
                                   int edge_seg) {
  switch (edge_seg) {
    case EDGE_SEG_CONTOUR:
      /*
       * Edge-dominant block: use the most precise subpel search.
       * Force 1/8-pel and increase diagonal checks per step.
       * Getting the motion vector right on contours is critical for
       * preserving edge structure in the prediction.
       */
      ms_params->forced_stop = EIGHTH_PEL;
      ms_params->iters_per_step = 2;
      break;

    case EDGE_SEG_TEXTURE:
      /* Mixed content: allow 1/8-pel but use standard iterations. */
      ms_params->forced_stop = EIGHTH_PEL;
      break;

    case EDGE_SEG_FLAT:
      /*
       * Flat block: stop at half-pel precision.
       * For flat blocks, sub-pixel precision beyond 1/2 pel adds rate cost
       * for the MV without meaningful distortion reduction. The smooth
       * prediction error surface means 1/2 pel is sufficient.
       */
      ms_params->forced_stop = HALF_PEL;
      ms_params->iters_per_step = 1;
      break;

    case EDGE_SEG_NORMAL:
    default:
      /* Default behavior: no adjustment. */
      break;
  }
}

int av1_edge_skip_fractional_search(const AV1_COMP *cpi, int edge_seg) {
  (void)cpi;
  /*
   * Only skip fractional search on FLAT blocks when using aggressive speed
   * settings (speed >= 4). At lower speeds, we still do at least half-pel
   * even on flat blocks — the adjusted forced_stop handles the savings.
   */
  if (edge_seg == EDGE_SEG_FLAT && cpi->oxcf.speed >= 4) {
    return 1;
  }
  return 0;
}

#endif /* !CONFIG_REALTIME_ONLY */
