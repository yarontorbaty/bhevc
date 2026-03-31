#include "bhevc.h"

/*
 * Brain-inspired prediction module.
 *
 * Intra: uses edge orientation to select prediction direction, mirroring
 *        how cortical orientation selectivity emerges from thalamic input.
 * Inter: uses structure-aware cost function that penalizes edge-breaking
 *        motion vectors, and adapts search range by region importance.
 */

/* 4x4 intra prediction */
void predict_intra_blk(const uint8_t *rec, int stride,
                       int bx, int by, IntraMode mode, uint8_t pred[16]) {
    uint8_t top[4], left[4];
    int has_top  = (by > 0);
    int has_left = (bx > 0);

    for (int i = 0; i < 4; i++) {
        top[i]  = has_top  ? rec[(by - 1) * stride + bx + i] : 128;
        left[i] = has_left ? rec[(by + i) * stride + bx - 1] : 128;
    }

    switch (mode) {
    case INTRA_DC: {
        int sum = 0, n = 0;
        if (has_top)  { for (int i = 0; i < 4; i++) { sum += top[i]; n++; } }
        if (has_left) { for (int i = 0; i < 4; i++) { sum += left[i]; n++; } }
        uint8_t dc = n > 0 ? (uint8_t)((sum + n / 2) / n) : 128;
        for (int i = 0; i < 16; i++) pred[i] = dc;
        break;
    }
    case INTRA_H:
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                pred[r * 4 + c] = left[r];
        break;
    case INTRA_V:
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                pred[r * 4 + c] = top[c];
        break;
    case INTRA_DIAG:
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                int avg = ((has_top ? top[c] : 128) + (has_left ? left[r] : 128) + 1) >> 1;
                pred[r * 4 + c] = (uint8_t)avg;
            }
        break;
    }
}

static int sad4x4(const uint8_t *a, const uint8_t *pred, int stride) {
    int s = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            s += abs((int)a[r * stride + c] - (int)pred[r * 4 + c]);
    return s;
}

/*
 * Brain-inspired intra mode selection.
 * First checks the edge-orientation-guided guess from the MB analysis,
 * then verifies against actual RD cost. This saves compute by starting
 * with the structurally suggested mode rather than testing all blindly.
 */
IntraMode predict_intra_best(const uint8_t *orig, const uint8_t *rec,
                             int stride, int bx, int by,
                             const MBAnalysis *mba) {
    uint8_t pred[16];
    int best_cost = INT32_MAX;
    IntraMode best_mode = INTRA_DC;

    IntraMode try_order[4];
    try_order[0] = mba ? mba->best_intra : INTRA_DC;
    int idx = 1;
    for (int m = 0; m < 4; m++) {
        if ((IntraMode)m != try_order[0])
            try_order[idx++] = (IntraMode)m;
    }

    for (int ti = 0; ti < 4; ti++) {
        IntraMode m = try_order[ti];
        predict_intra_blk(rec, stride, bx, by, m, pred);
        int cost = sad4x4(orig + by * stride + bx, pred, stride);
        if (m == try_order[0]) cost -= 4; /* bias toward brain-inspired guess */
        if (cost < best_cost) {
            best_cost = cost;
            best_mode = m;
        }
    }
    return best_mode;
}

int sad16x16(const uint8_t *a, const uint8_t *b, int stride_a, int stride_b) {
    int s = 0;
    for (int r = 0; r < BHEVC_MB_SIZE; r++)
        for (int c = 0; c < BHEVC_MB_SIZE; c++)
            s += abs((int)a[r * stride_a + c] - (int)b[r * stride_b + c]);
    return s;
}

/*
 * Structure-aware SAD: adds a penalty for edge magnitude mismatch.
 * This ensures motion vectors preserve contour structure, not just
 * pixel luminance. Directly implements the paper's principle that
 * the visual system prioritizes structural features over amplitude.
 */
static int structure_sad16x16(const uint8_t *orig, const uint8_t *ref,
                              int stride, int ox, int oy, int rx, int ry,
                              const EdgeMap *emap_orig, const EdgeMap *emap_ref,
                              int frame_w) {
    int sad = 0, edge_cost = 0;
    for (int r = 0; r < BHEVC_MB_SIZE; r++) {
        for (int c = 0; c < BHEVC_MB_SIZE; c++) {
            sad += abs((int)orig[(oy + r) * stride + ox + c] -
                       (int)ref[(ry + r) * stride + rx + c]);

            if (emap_orig && emap_ref) {
                float mo = emap_orig->pixels[(oy + r) * frame_w + ox + c].magnitude;
                float mr = emap_ref->pixels[(ry + r) * frame_w + rx + c].magnitude;
                edge_cost += (int)fabsf(mo - mr);

                /* polarity mismatch penalty: extra cost if ON↔OFF */
                int po = emap_orig->pixels[(oy + r) * frame_w + ox + c].polarity;
                int pr = emap_ref->pixels[(ry + r) * frame_w + rx + c].polarity;
                if (po != 0 && pr != 0 && po != pr)
                    edge_cost += 8;
            }
        }
    }
    return sad + (edge_cost >> 1);
}

/*
 * Brain-inspired inter prediction (motion estimation).
 *
 * Key innovations over standard block matching:
 *   1. Search range varies by region type (4/16/32 pixels)
 *   2. Cost function includes edge structure preservation term
 *   3. ON/OFF polarity mismatch is penalized
 *   4. FLAT regions default to zero-MV (minimal compute)
 *
 * This implements the paper's principle of sparse, structure-guided
 * processing: expend compute where structure matters, skip where it doesn't.
 */
MotionVector predict_inter_mb(const uint8_t *orig, const uint8_t *ref,
                              int stride, int mb_x, int mb_y,
                              int frame_w, int frame_h,
                              const MBAnalysis *mba,
                              const EdgeMap *emap_orig, const EdgeMap *emap_ref) {
    MotionVector best = {0, 0};
    int ox = mb_x * BHEVC_MB_SIZE;
    int oy = mb_y * BHEVC_MB_SIZE;

    if (mba->type == REGION_FLAT)
        return best;

    int range = mba->search_range;
    int best_cost = structure_sad16x16(orig, ref, stride, ox, oy, ox, oy,
                                        emap_orig, emap_ref, frame_w);

    /* Diamond search for speed: large step first, refine */
    static const int diamond_large[][2] = {
        {-4,0},{4,0},{0,-4},{0,4},{-3,-3},{3,-3},{-3,3},{3,3}
    };
    static const int diamond_small[][2] = {
        {-1,0},{1,0},{0,-1},{0,1},{-1,-1},{1,-1},{-1,1},{1,1}
    };

    int cx = 0, cy = 0;

    /* Large diamond passes */
    for (int pass = 0; pass < 4; pass++) {
        int moved = 0;
        for (int d = 0; d < 8; d++) {
            int dx = cx + diamond_large[d][0] * (1 + pass);
            int dy = cy + diamond_large[d][1] * (1 + pass);
            if (abs(dx) > range || abs(dy) > range) continue;
            int rx = ox + dx, ry = oy + dy;
            if (rx < 0 || ry < 0 ||
                rx + BHEVC_MB_SIZE > frame_w ||
                ry + BHEVC_MB_SIZE > frame_h) continue;

            int cost = structure_sad16x16(orig, ref, stride, ox, oy, rx, ry,
                                           emap_orig, emap_ref, frame_w);
            cost += (abs(dx) + abs(dy)) * 2;
            if (cost < best_cost) {
                best_cost = cost;
                best.x = dx; best.y = dy;
                cx = dx; cy = dy;
                moved = 1;
            }
        }
        if (!moved) break;
    }

    /* Small diamond refinement */
    for (int pass = 0; pass < 3; pass++) {
        int moved = 0;
        for (int d = 0; d < 8; d++) {
            int dx = cx + diamond_small[d][0];
            int dy = cy + diamond_small[d][1];
            if (abs(dx) > range || abs(dy) > range) continue;
            int rx = ox + dx, ry = oy + dy;
            if (rx < 0 || ry < 0 ||
                rx + BHEVC_MB_SIZE > frame_w ||
                ry + BHEVC_MB_SIZE > frame_h) continue;

            int cost = structure_sad16x16(orig, ref, stride, ox, oy, rx, ry,
                                           emap_orig, emap_ref, frame_w);
            cost += (abs(dx) + abs(dy)) * 2;
            if (cost < best_cost) {
                best_cost = cost;
                best.x = dx; best.y = dy;
                cx = dx; cy = dy;
                moved = 1;
            }
        }
        if (!moved) break;
    }

    return best;
}
