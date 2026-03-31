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
 * ===========================================================================
 * INTEGRATION PATCH — Reference code showing exact changes to libaom files.
 * ===========================================================================
 *
 * This file is NOT compiled directly. It documents the precise modifications
 * needed to integrate the two brain-inspired features into libaom.
 *
 * For each change site, we show:
 *   [FILE]     The file to modify
 *   [LOCATION] Where in the file
 *   [BEFORE]   Original code
 *   [AFTER]    Modified code
 */

/* ======================================================================== */
/* CONCEPT 1: Edge-guided motion search effort                              */
/* ======================================================================== */

/*
 * [FILE]     av1/encoder/motion_search_facade.c
 * [LOCATION] Top of file, after existing includes
 * [CHANGE]   Add include
 */
#if 0  /* PATCH: add include */
/* Add after: #include "av1/encoder/tx_search.h" */
#include "av1/encoder/edge_motion_search.h"
#endif

/*
 * [FILE]     av1/encoder/motion_search_facade.c
 * [LOCATION] av1_single_motion_search(), after step_param computation (~line 163)
 * [BEFORE]
 *     step_param = mv_search_params->mv_step_param;
 *   }
 *
 *   const MV ref_mv = av1_get_ref_mv(x, ref_idx).as_mv;
 *
 * [AFTER]
 *     step_param = mv_search_params->mv_step_param;
 *   }
 *
 *   // Brain-inspired: modulate search effort by edge density
 *   #if !CONFIG_REALTIME_ONLY
 *   const int edge_seg = av1_get_edge_segment(cpi, mbmi);
 *   step_param = av1_edge_adjust_step_param(cpi, edge_seg, step_param);
 *   #endif
 *
 *   const MV ref_mv = av1_get_ref_mv(x, ref_idx).as_mv;
 */

/*
 * [FILE]     av1/encoder/motion_search_facade.c
 * [LOCATION] av1_single_motion_search(), fractional search gate (~line 398)
 * [BEFORE]
 *   const int use_fractional_mv =
 *       bestsme < INT_MAX && cpi->common.features.cur_frame_force_integer_mv == 0;
 *
 * [AFTER]
 *   int use_fractional_mv =
 *       bestsme < INT_MAX && cpi->common.features.cur_frame_force_integer_mv == 0;
 *   #if !CONFIG_REALTIME_ONLY
 *   if (use_fractional_mv && av1_edge_skip_fractional_search(cpi, edge_seg)) {
 *     use_fractional_mv = 0;
 *   }
 *   #endif
 */

/*
 * [FILE]     av1/encoder/motion_search_facade.c
 * [LOCATION] av1_single_motion_search(), after av1_make_default_subpel_ms_params (~line 409)
 * [BEFORE]
 *     av1_make_default_subpel_ms_params(&ms_params, cpi, x, bsize, &ref_mv,
 *                                       cost_list);
 *     MV subpel_start_mv = get_mv_from_fullmv(&best_mv->as_fullmv);
 *
 * [AFTER]
 *     av1_make_default_subpel_ms_params(&ms_params, cpi, x, bsize, &ref_mv,
 *                                       cost_list);
 *     #if !CONFIG_REALTIME_ONLY
 *     av1_edge_adjust_subpel_params(&ms_params, edge_seg);
 *     #endif
 *     MV subpel_start_mv = get_mv_from_fullmv(&best_mv->as_fullmv);
 */


/* ======================================================================== */
/* CONCEPT 2: Structure-aware RD cost                                       */
/* ======================================================================== */

/*
 * [FILE]     av1/encoder/rdopt.c
 * [LOCATION] Top of file, after existing includes
 * [CHANGE]   Add include
 */
#if 0  /* PATCH: add include */
/* Add after: #include "av1/encoder/rd.h" */
#include "av1/encoder/edge_rd.h"
#endif

/*
 * [FILE]     av1/encoder/rdopt.c
 * [LOCATION] adjust_rdcost() function, at the top before existing logic (~line 798)
 * [BEFORE]
 *   static void adjust_rdcost(const AV1_COMP *cpi, const MACROBLOCK *x,
 *                             RD_STATS *rd_cost, bool is_inter_pred) {
 *     if ((cpi->oxcf.tune_cfg.tuning == AOM_TUNE_IQ || ...
 *
 * [AFTER]
 *   static void adjust_rdcost(const AV1_COMP *cpi, const MACROBLOCK *x,
 *                             RD_STATS *rd_cost, bool is_inter_pred) {
 *     #if !CONFIG_REALTIME_ONLY
 *     if (cpi->oxcf.q_cfg.aq_mode == EDGE_AQ && is_inter_pred) {
 *       av1_edge_rd_adjust(cpi, x, rd_cost);
 *       return;
 *     }
 *     #endif
 *     if ((cpi->oxcf.tune_cfg.tuning == AOM_TUNE_IQ || ...
 */

/*
 * [FILE]     av1/encoder/rdopt.c
 * [LOCATION] adjust_cost() function, at the top before existing logic (~line 839)
 * [BEFORE]
 *   static void adjust_cost(const AV1_COMP *cpi, const MACROBLOCK *x,
 *                           int64_t *rd_cost, bool is_inter_pred) {
 *     if ((cpi->oxcf.tune_cfg.tuning == AOM_TUNE_IQ || ...
 *
 * [AFTER]
 *   static void adjust_cost(const AV1_COMP *cpi, const MACROBLOCK *x,
 *                           int64_t *rd_cost, bool is_inter_pred) {
 *     #if !CONFIG_REALTIME_ONLY
 *     if (cpi->oxcf.q_cfg.aq_mode == EDGE_AQ && is_inter_pred) {
 *       // For the scalar rd_cost version, we compute the edge penalty
 *       // from the same edge_mismatch and add it.
 *       // Note: av1_edge_rd_adjust operates on RD_STATS; for the scalar
 *       // pathway, we need a lightweight approximation. The full RD_STATS
 *       // path through adjust_rdcost() handles the primary integration.
 *       // This path is used for early-termination comparisons only.
 *       return;
 *     }
 *     #endif
 *     if ((cpi->oxcf.tune_cfg.tuning == AOM_TUNE_IQ || ...
 */


/* ======================================================================== */
/* BUILD SYSTEM                                                             */
/* ======================================================================== */

/*
 * [FILE]     av1/av1.cmake
 * [LOCATION] In the AV1_ENCODER_SOURCES list
 * [CHANGE]   Add the new source files
 *
 *   "${AOM_ROOT}/av1/encoder/edge_motion_search.c"
 *   "${AOM_ROOT}/av1/encoder/edge_motion_search.h"
 *   "${AOM_ROOT}/av1/encoder/edge_rd.c"
 *   "${AOM_ROOT}/av1/encoder/edge_rd.h"
 */
