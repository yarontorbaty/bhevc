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
 * Temporal ON/OFF event detection for brain-inspired video encoding.
 *
 * Computes a per-MI-unit change map by comparing the current source frame
 * to the previous source frame. Each 4x4 block is classified as:
 *   TEMPORAL_STABLE: mean absolute difference < threshold
 *   TEMPORAL_ON:     net brightness increase above threshold
 *   TEMPORAL_OFF:    net brightness decrease above threshold
 *
 * This mirrors the ON-center / OFF-center temporal response pathways
 * in the lateral geniculate nucleus (LGN).
 */

#include <stdlib.h>
#include <string.h>

#include "aom_dsp/aom_dsp_common.h"
#include "av1/encoder/temporal_events.h"
#include "config/aom_config.h"

#if !CONFIG_REALTIME_ONLY

#define TEMPORAL_THRESH 8

void av1_compute_temporal_change_map(AV1_COMP *cpi) {
  const AV1_COMMON *cm = &cpi->common;
  const int mi_rows = cm->mi_params.mi_rows;
  const int mi_cols = cm->mi_params.mi_cols;
  const int map_size = mi_rows * mi_cols;

  if (cpi->temporal_change_map == NULL ||
      cpi->temporal_change_map_alloc_size < map_size) {
    aom_free(cpi->temporal_change_map);
    cpi->temporal_change_map = aom_calloc(map_size, sizeof(uint8_t));
    cpi->temporal_change_map_alloc_size = map_size;
  }

  if (cpi->source == NULL || cpi->last_source == NULL) {
    memset(cpi->temporal_change_map, TEMPORAL_STABLE, map_size);
    return;
  }

  const YV12_BUFFER_CONFIG *cur = cpi->source;
  const YV12_BUFFER_CONFIG *prev = cpi->last_source;

  if (cur->y_width != prev->y_width || cur->y_height != prev->y_height) {
    memset(cpi->temporal_change_map, TEMPORAL_STABLE, map_size);
    return;
  }

  const int use_hbd = cur->flags & YV12_FLAG_HIGHBITDEPTH;
  const int shift = use_hbd ? (cm->seq_params->bit_depth - 8) : 0;
  const int thresh = TEMPORAL_THRESH << shift;

  for (int mi_row = 0; mi_row < mi_rows; mi_row++) {
    for (int mi_col = 0; mi_col < mi_cols; mi_col++) {
      const int y = mi_row * MI_SIZE;
      const int x = mi_col * MI_SIZE;

      if (y + MI_SIZE > cur->y_height || x + MI_SIZE > cur->y_width) {
        cpi->temporal_change_map[mi_row * mi_cols + mi_col] = TEMPORAL_STABLE;
        continue;
      }

      int sum_diff = 0;
      int sum_abs = 0;

      if (use_hbd) {
        const uint16_t *cur_row =
            CONVERT_TO_SHORTPTR(cur->y_buffer) + y * cur->y_stride + x;
        const uint16_t *prev_row =
            CONVERT_TO_SHORTPTR(prev->y_buffer) + y * prev->y_stride + x;
        for (int r = 0; r < MI_SIZE; r++) {
          for (int c = 0; c < MI_SIZE; c++) {
            int d = (int)cur_row[r * cur->y_stride + c] -
                    (int)prev_row[r * prev->y_stride + c];
            sum_diff += d;
            sum_abs += abs(d);
          }
        }
      } else {
        const uint8_t *cur_row = cur->y_buffer + y * cur->y_stride + x;
        const uint8_t *prev_row = prev->y_buffer + y * prev->y_stride + x;
        for (int r = 0; r < MI_SIZE; r++) {
          for (int c = 0; c < MI_SIZE; c++) {
            int d = (int)cur_row[r * cur->y_stride + c] -
                    (int)prev_row[r * prev->y_stride + c];
            sum_diff += d;
            sum_abs += abs(d);
          }
        }
      }

      int mean_abs = sum_abs / (MI_SIZE * MI_SIZE);
      int map_idx = mi_row * mi_cols + mi_col;

      if (mean_abs < thresh) {
        cpi->temporal_change_map[map_idx] = TEMPORAL_STABLE;
      } else if (sum_diff > 0) {
        cpi->temporal_change_map[map_idx] = TEMPORAL_ON;
      } else {
        cpi->temporal_change_map[map_idx] = TEMPORAL_OFF;
      }
    }
  }
}

int av1_get_temporal_event(const AV1_COMP *cpi, int mi_row, int mi_col) {
  if (cpi->temporal_change_map == NULL) return TEMPORAL_STABLE;
  const int mi_cols = cpi->common.mi_params.mi_cols;
  if (mi_row < 0 || mi_col < 0 || mi_row >= cpi->common.mi_params.mi_rows ||
      mi_col >= mi_cols)
    return TEMPORAL_STABLE;
  return cpi->temporal_change_map[mi_row * mi_cols + mi_col];
}

#endif  // !CONFIG_REALTIME_ONLY
