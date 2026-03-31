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

#ifndef AOM_AV1_ENCODER_TEMPORAL_EVENTS_H_
#define AOM_AV1_ENCODER_TEMPORAL_EVENTS_H_

#include "av1/encoder/encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TEMPORAL_STABLE 0
#define TEMPORAL_ON     1
#define TEMPORAL_OFF    2

#if !CONFIG_REALTIME_ONLY

void av1_compute_temporal_change_map(AV1_COMP *cpi);

int av1_get_temporal_event(const AV1_COMP *cpi, int mi_row, int mi_col);

#endif

#ifdef __cplusplus
}
#endif

#endif  // AOM_AV1_ENCODER_TEMPORAL_EVENTS_H_
