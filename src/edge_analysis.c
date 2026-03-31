#include "bhevc.h"

/*
 * Brain-inspired edge analysis module.
 *
 * Models the thalamocortical ON/OFF center-surround processing described in
 * "Thalamic activation of the visual cortex at the single-synapse level"
 * (Science 391, 2026).
 *
 * Key principles implemented:
 *   - Signed (ON/OFF) edge detection mimicking LGN center-surround cells
 *   - Orientation estimation from spatial arrangement of ON/OFF fields
 *   - Sparse local representation (only edge pixels carry structure)
 *   - Region classification: flat / edge-dominant / textured
 *   - Temporal change events for inter-frame analysis
 */

EdgeMap *edgemap_alloc(int w, int h) {
    EdgeMap *m = calloc(1, sizeof(EdgeMap));
    if (!m) return NULL;
    m->width = w;
    m->height = h;
    m->pixels = calloc(w * h, sizeof(EdgePixel));
    if (!m->pixels) { free(m); return NULL; }
    return m;
}

void edgemap_free(EdgeMap *m) {
    if (!m) return;
    free(m->pixels);
    free(m);
}

/*
 * Compute signed edge map using Sobel operator.
 * Each pixel gets: gradient magnitude, direction, and ON/OFF polarity.
 *
 * ON polarity  = luminance increases along gradient direction (bright side)
 * OFF polarity = luminance decreases along gradient direction (dark side)
 *
 * This directly mirrors the ON-center / OFF-center receptive field
 * classification from the paper's thalamocortical synapse mapping.
 */
void edge_compute(const Frame *f, EdgeMap *m) {
    int w = f->width, h = f->height;
    const uint8_t *y = f->y;

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            EdgePixel *ep = &m->pixels[row * w + col];

            if (row == 0 || row == h - 1 || col == 0 || col == w - 1) {
                *ep = (EdgePixel){0, 0, 0, 0, 0};
                continue;
            }

            int gx = -y[(row-1)*w + col-1] +     y[(row-1)*w + col+1]
                   - 2*y[row*w + col-1]     + 2*y[row*w + col+1]
                   -   y[(row+1)*w + col-1] +   y[(row+1)*w + col+1];

            int gy = -y[(row-1)*w + col-1] - 2*y[(row-1)*w + col] - y[(row-1)*w + col+1]
                   +  y[(row+1)*w + col-1] + 2*y[(row+1)*w + col] + y[(row+1)*w + col+1];

            ep->gx = (float)gx;
            ep->gy = (float)gy;
            ep->magnitude = sqrtf((float)(gx * gx + gy * gy));

            float dir = atan2f((float)gy, (float)gx);
            if (dir < 0) dir += (float)M_PI;
            ep->direction = dir;

            if (ep->magnitude > BHEVC_EDGE_THRESH) {
                if (fabsf(ep->gx) > fabsf(ep->gy))
                    ep->polarity = (ep->gx > 0) ? 1 : -1;
                else
                    ep->polarity = (ep->gy > 0) ? 1 : -1;
            } else {
                ep->polarity = 0;
            }
        }
    }
}

/*
 * Classify macroblocks using brain-inspired region segmentation.
 *
 * FLAT regions   → cheap encode, minimal bits (like background cortical silence)
 * EDGE regions   → high priority, low QP (like strong thalamocortical drive)
 * TEXTURE regions → moderate encode (mixed selectivity)
 *
 * Also derives dominant orientation per MB to guide intra prediction,
 * mirroring how orientation preference emerges from ON/OFF spatial organization.
 */
void edge_classify_mbs(const EdgeMap *m, MBAnalysis *info, int mb_w, int mb_h) {
    int w = m->width;

    for (int mby = 0; mby < mb_h; mby++) {
        for (int mbx = 0; mbx < mb_w; mbx++) {
            MBAnalysis *mb = &info[mby * mb_w + mbx];
            int sx = mbx * BHEVC_MB_SIZE;
            int sy = mby * BHEVC_MB_SIZE;
            int edge_count = 0, total = 0;
            float sum_mag = 0;
            float dir_hist[8] = {0};

            for (int dy = 0; dy < BHEVC_MB_SIZE && sy + dy < m->height; dy++) {
                for (int dx = 0; dx < BHEVC_MB_SIZE && sx + dx < w; dx++) {
                    const EdgePixel *ep = &m->pixels[(sy + dy) * w + (sx + dx)];
                    total++;
                    if (ep->polarity != 0) {
                        edge_count++;
                        sum_mag += ep->magnitude;
                        int bin = (int)(ep->direction / (float)M_PI * 8.0f);
                        if (bin >= 8) bin = 7;
                        dir_hist[bin] += ep->magnitude;
                    }
                }
            }

            mb->edge_density = total > 0 ? (float)edge_count / total : 0;
            mb->avg_magnitude = edge_count > 0 ? sum_mag / edge_count : 0;

            int best_bin = 0;
            for (int i = 1; i < 8; i++)
                if (dir_hist[i] > dir_hist[best_bin]) best_bin = i;
            mb->dominant_orientation = (best_bin + 0.5f) * (float)M_PI / 8.0f;

            float total_dir = 0;
            for (int i = 0; i < 8; i++) total_dir += dir_hist[i];
            mb->contour_confidence = total_dir > 0 ? dir_hist[best_bin] / total_dir : 0;

            /*
             * Brain-inspired region classification and resource allocation.
             * EDGE blocks get QP-6 (4x quality) and 32-pixel search range.
             * FLAT blocks get QP+6 (1/4 quality) and 4-pixel search range.
             * This implements the paper's principle: robust primitives get
             * the cheap path, important structure gets the expensive path.
             */
            if (mb->edge_density < BHEVC_FLAT_DENSITY) {
                mb->type = REGION_FLAT;
                mb->qp_offset = 6;
                mb->search_range = 4;
            } else if (mb->edge_density > BHEVC_EDGE_DENSITY) {
                mb->type = REGION_EDGE;
                mb->qp_offset = -6;
                mb->search_range = 32;
            } else {
                mb->type = REGION_TEXTURE;
                mb->qp_offset = 0;
                mb->search_range = 16;
            }

            /*
             * Edge-orientation-guided intra mode selection.
             * Mirrors how orientation preference emerges from the spatial
             * arrangement of ON and OFF receptive fields (paper Fig S7).
             */
            if (mb->contour_confidence > 0.3f) {
                float a = mb->dominant_orientation;
                if (a < M_PI * 0.25f || a > M_PI * 0.75f)
                    mb->best_intra = INTRA_H;
                else
                    mb->best_intra = INTRA_V;
            } else {
                mb->best_intra = INTRA_DC;
            }
        }
    }
}

/*
 * Temporal ON/OFF event detection for P-frame analysis.
 * Identifies where luminance changed between frames and classifies
 * the change as ON (brighter) or OFF (darker).
 *
 * This mirrors how the visual cortex detects temporal luminance events
 * through ON-center and OFF-center pathways operating on change signals.
 */
void edge_temporal(const Frame *cur, const Frame *ref, EdgeMap *ev) {
    int w = cur->width, h = cur->height;

    for (int i = 0; i < w * h; i++) {
        int diff = (int)cur->y[i] - (int)ref->y[i];
        EdgePixel *ep = &ev->pixels[i];
        ep->magnitude = (float)abs(diff);
        ep->gx = (float)diff;
        ep->gy = 0;
        ep->direction = 0;
        if (abs(diff) > (int)BHEVC_EDGE_THRESH) {
            ep->polarity = (diff > 0) ? 1 : -1;
        } else {
            ep->polarity = 0;
        }
    }
}
