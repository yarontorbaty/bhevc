# Brain-Inspired Video Encoding: Research Gap Analysis

**Date:** 2026-03-30
**Source:** *brain_inspired_video_encoding_notes.md* (based on "Thalamic activation of the visual cortex at the single-synapse level", Science 391, 1349, 2026)
**Scope:** Concepts proposed in the research document that are NOT yet implemented in the bhevc project.

---

## Current Implementation Inventory

Before identifying gaps, here is what has been built:

| Component | File | Status | What it does |
|-----------|------|--------|-------------|
| Edge-aware AQ | `aq_edge.c` | Implemented, compiles | Sobel **magnitude** → 4-way classification (FLAT/NORMAL/TEXTURE/CONTOUR) → QP delta via AV1 segmentation |
| Edge-guided ME | `edge_motion_search.c` | Designed (future/) | Adjusts fullpel step_param, subpel forced_stop, and fractional-search skip based on edge segment |
| Structure-aware RD | `edge_rd.c` | Designed (future/) | Adds Sobel magnitude mismatch between source and reconstruction as a weighted RD penalty |

**Common characteristic of all three:** They compute Sobel gradients `gx` and `gy` but immediately discard the signs by computing `gx²+gy²` or `|gx|+|gy|`. All analysis is single-frame. No temporal, no orientation, no polarity, no contour connectivity.

---

## Gap 1: ON/OFF Polarity (Signed Edge Classification)

### What the research says

> "local **ON/OFF** luminance changes" (§Bottom line)
>
> "**edge polarity** and spatial alignment" (§Bottom line)
>
> "The ON/OFF organization strongly implies that the visual system cares about **where brightness flips occur** and how those flips align into oriented contours." (§2.3)
>
> "polarity-aware structure maps" (§2.2)
>
> "signed gradient maps" (§Top 5, idea 1)
>
> "edge sign inversions" (§6, structure-aware distortion metric)

The paper's core finding is that individual thalamocortical synapses are classified as either ON-center (respond to luminance increase) or OFF-center (respond to luminance decrease). The brain doesn't just detect "there is an edge here" — it specifically encodes whether brightness goes up or down at each location.

### What's implemented

All three files (`aq_edge.c`, `edge_rd.c`, `edge_motion_search.c`) compute the Sobel kernels producing signed `gx` and `gy`, but then immediately take the magnitude:

```c
// aq_edge.c, line 135
int64_t mag_sq = (int64_t)gx * gx + (int64_t)gy * gy;

// edge_rd.c, line 101
return abs(gx) + abs(gy);
```

The sign of `gx` and `gy` is discarded. The 4-way classification (FLAT/NORMAL/TEXTURE/CONTOUR) is based purely on edge density — the fraction of pixels where magnitude exceeds a threshold. Two blocks with identical edge density but opposite polarity patterns are treated identically.

### What's missing

**1a. Polarity-aware AQ segmentation**

The current classification asks: "how many edges?" A polarity-aware version would also ask: "are the edges consistently ON→OFF (dark-to-light) or mixed?" This matters because:

- A block on a sharp text boundary has uniform polarity (e.g., all gradients point dark-to-light). This is an extremely perceptually salient region.
- A block in noisy texture has high edge density but random polarity. This is less salient.

The gradient sign could split the CONTOUR segment into CONTOUR_COHERENT (uniform polarity → maximum protection) and CONTOUR_MIXED (random polarity → moderate protection), yielding finer QP allocation.

**Implementation in AV1/libaom:**

In `av1_edge_block_score()`, after computing `gx` and `gy` for each pixel, accumulate signed sums:

```c
int sum_gx = 0, sum_gy = 0;
int abs_sum = 0;

for (each pixel) {
    sum_gx += gx;
    sum_gy += gy;
    abs_sum += abs(gx) + abs(gy);
}

// Polarity coherence: ratio of |signed sum| to absolute sum.
// 1.0 = all gradients point the same direction (coherent boundary)
// 0.0 = gradients cancel out (noise/texture)
double coherence = 0.0;
if (abs_sum > 0) {
    coherence = (double)(abs(sum_gx) + abs(sum_gy)) / abs_sum;
}
```

A block with `coherence > 0.6` and high edge density would be classified as CONTOUR_COHERENT. This adds zero extra Sobel computation — only a signed accumulator alongside the existing magnitude check.

**1b. Polarity-aware RD penalty**

In `edge_rd.c`, the mismatch is `|Sobel_mag(src) - Sobel_mag(rec)|`. This catches magnitude changes but misses polarity inversions. Consider a case where the source has a dark-to-light edge at a pixel and the reconstruction has a light-to-dark edge at the same location. The magnitudes could be similar, but the polarity is inverted — a severe perceptual artifact (a "halo" or "ringing reversal"). A polarity-aware mismatch would be:

```c
int src_gx = ..., rec_gx = ...;
int src_gy = ..., rec_gy = ...;

// Signed mismatch: catches polarity inversions
int mismatch_x = abs(src_gx - rec_gx);
int mismatch_y = abs(src_gy - rec_gy);
int signed_mismatch = mismatch_x + mismatch_y;

// Compare: unsigned magnitude mismatch
int mag_mismatch = abs((abs(src_gx)+abs(src_gy)) - (abs(rec_gx)+abs(rec_gy)));

// signed_mismatch >= mag_mismatch, with equality when polarities match.
// The excess (signed_mismatch - mag_mismatch) is the polarity violation.
```

This is a drop-in replacement for `sobel_mag_8bit()` / `sobel_mag_hbd()` comparisons in `av1_compute_edge_mismatch()`.

### Evaluation

| Criterion | Assessment |
|-----------|-----------|
| **Theoretical basis** | Very strong — ON/OFF polarity is the paper's central finding |
| **Implementation complexity** | Low — reuse existing Sobel, add signed accumulators |
| **Expected impact** | BD-rate: 0.1-0.3% on content with sharp boundaries (text, animation, UI). Perceptual: noticeable reduction in ringing/halo at contour edges. Speed: negligible cost (same Sobel pass, one more accumulator) |
| **Priority** | **1** — Highest impact, lowest risk, directly addresses the paper's core insight |

---

## Gap 2: Temporal ON/OFF Events (Inter-Frame Change Maps)

### What the research says

> "ON/OFF temporal events" (§1, front-end outputs)
>
> "temporal change events" (§1, front-end outputs)
>
> "temporal change confidence" (§1, front-end outputs)
>
> "Use signed temporal-change maps to reduce motion-search workload and prioritize important regions." (§Top 5, idea 5)
>
> "where the important signed changes occurred... which contours moved... which regions are structurally stable" (§5, event-guided motion estimation)
>
> "coarse change timing" and "polarity maps" can guide some decisions instead of full-precision reconstructed pixels (§4.5)

The paper argues that the visual system responds to temporal luminance change events — not to absolute pixel values, but to *where brightness increased or decreased compared to the previous time step*. This is fundamentally different from standard motion estimation, which tries to find where blocks moved. A temporal ON/OFF map says: "at this location, the scene got brighter" without trying to explain *why* (motion, lighting change, occlusion).

### What's implemented

Nothing temporal. All three implemented/designed components operate on a single frame:

- `aq_edge.c`: classifies the current frame's edge density
- `edge_motion_search.c`: adjusts search effort based on the current frame's edge density
- `edge_rd.c`: compares source and reconstruction gradients within the current frame

No frame-to-frame comparison exists. The previous frame's edge map is not stored or compared.

### What's missing

**2a. Temporal change map computation**

Before encoding a P-frame, compute a per-pixel or per-block temporal change map:

```c
// For each pixel (r, c):
int delta = cur_frame[r][c] - prev_recon[r][c];

// Classification:
//   delta > threshold  → TEMPORAL_ON  (brightness increased)
//   delta < -threshold → TEMPORAL_OFF (brightness decreased)
//   otherwise          → TEMPORAL_STABLE
```

The previous reconstructed frame (`prev_recon`) is already available in AV1 as the reference frame buffer. The computation is a simple subtraction — cheaper than a Sobel pass.

**2b. Temporal change map → motion search guidance**

This is the highest-value application. Currently, `edge_motion_search.c` modulates search effort based on spatial edge density. Temporal change maps provide a complementary signal:

- **TEMPORAL_STABLE blocks:** No significant brightness change. The previous frame's prediction is likely excellent. Reduce motion search effort aggressively (larger step_param, skip fractional).
- **TEMPORAL_ON/OFF blocks with spatial edge:** A contour has moved or changed. Maximum search effort — this is where motion accuracy matters most.
- **TEMPORAL_ON/OFF blocks without spatial edge:** Uniform brightness change (e.g., fade, lighting shift). Standard search is sufficient.

**Integration into `edge_motion_search.c`:**

```c
typedef struct {
    int spatial_seg;    // EDGE_SEG_CONTOUR..FLAT (existing)
    int temporal_event; // TEMPORAL_ON, TEMPORAL_OFF, TEMPORAL_STABLE
} EdgeTemporalScore;

int av1_edge_temporal_adjust_step_param(
    const AV1_COMP *cpi, EdgeTemporalScore score, int base_step) {
    if (score.temporal_event == TEMPORAL_STABLE) {
        // Scene didn't change here — skip expensive search
        return AOMMIN(base_step + 3, MAX_MVSEARCH_STEPS - 2);
    }
    if (score.spatial_seg == EDGE_SEG_CONTOUR &&
        score.temporal_event != TEMPORAL_STABLE) {
        // Edge region that changed — maximum search effort
        return AOMMAX(base_step - 2, 0);
    }
    return base_step;
}
```

**2c. Temporal change map → inter/intra mode decision**

The research document proposes using temporal events to guide inter/intra mode selection. In AV1, the `inter_intra_search_order` and `skip_intra_in_interframe` speed features already make mode selection decisions early. A temporal change map could feed into these:

- Blocks that are TEMPORAL_STABLE with no spatial edge: strong bias toward LAST_FRAME reference, skip intra evaluation
- Blocks with strong TEMPORAL_ON/OFF events and no good motion match: consider intra more seriously (new content appeared)

**Implementation approach:**

Store a per-superblock temporal event summary in a frame-level array (allocated alongside the existing segment map). Compute it once during the lookahead or first-pass analysis. This array would be consulted by `handle_inter_mode()` and `skip_interp_filter_search()`.

**Storage cost:** One byte per 4×4 MI unit = (frame_width/4) × (frame_height/4) bytes. For 1080p: ~518 KB. Negligible.

### Evaluation

| Criterion | Assessment |
|-----------|-----------|
| **Theoretical basis** | Strong — the paper explicitly describes temporal ON/OFF as a primary signal. The visual system responds to change, not to static values. |
| **Implementation complexity** | Medium — requires storing the previous frame's edge/change map, computing per-block temporal deltas, and wiring the result into motion search and mode decision. The computation itself is trivial (pixel subtraction), but the integration touches multiple files. |
| **Expected impact** | Speed: 5-15% encoder speedup on temporally stable content (talking heads, slides, surveillance) from reduced motion search on stable blocks. BD-rate: 0.1-0.5% improvement from better motion search allocation. Perceptual: better contour tracking on moving edges. |
| **Priority** | **2** — High impact, moderate implementation effort. The speed benefit alone justifies it. |

---

## Gap 3: Two-Stage Codec — Structure Then Refinement (Partition Search Modulation)

### What the research says

> "A promising architecture: Stage A: Cheap structural code... Stage B: Conditional refinement — only where needed" (§3)
>
> "cheap coarse representation + optional expensive refinement" (§2.4)
>
> "conditional compute and layered decoding" (§2.4)
>
> "The silencing experiments suggest that a primitive feedforward representation can still exist even when higher-level selectivity is reduced." (§2.3)

The paper's silencing experiments show that when cortical processing is suppressed, basic structural representation (feedforward thalamic input) is preserved, but fine-grained selectivity disappears. This maps to a codec architecture where a cheap first pass captures structure and an expensive second pass refines only where needed.

### What's implemented

The edge-guided motion search (`edge_motion_search.c`) reduces effort on flat blocks and increases it on edge blocks. This is a *form* of conditional compute, but it only modulates motion search. It does not affect:

- Partition search depth (the most expensive encoder operation)
- Transform search exhaustiveness
- Intra mode candidate pruning
- Reference frame selection effort

The encoder still runs the full AV1 pipeline for every block, with only step_param and subpel precision varying.

### What's missing

**3a. Edge-guided partition search early termination**

AV1's partition search is the dominant cost center. The encoder recursively tries splitting each superblock into smaller partitions (PARTITION_NONE, SPLIT, HORZ, VERT, etc.) and evaluates RD cost for each. Early termination heuristics already exist (e.g., `sf->part_sf.ml_partition_search_breakout_thresh`).

Edge classification provides a strong prior:

- **FLAT blocks:** Extremely aggressive early termination. If a 64×64 block is classified FLAT, try PARTITION_NONE first. If its RD cost is within 5% of the estimated minimum, accept it without trying SPLIT. Flat blocks have no internal structure that would benefit from smaller partitions.
- **CONTOUR blocks:** Never early-terminate. Always explore the full partition tree. Edges that cross partition boundaries create blocking artifacts; smaller partitions can align boundaries with the edge, preserving it.
- **TEXTURE blocks:** Standard early termination thresholds.

**Implementation in AV1/libaom:**

In `av1_rd_pick_partition()` (in `partition_search.c`), after computing the initial partition cost, insert an edge-guided early exit:

```c
// After evaluating PARTITION_NONE:
if (cpi->oxcf.q_cfg.aq_mode == EDGE_AQ) {
    int edge_seg = av1_edge_block_score(cpi, x, bsize, mi_row, mi_col);
    if (edge_seg == EDGE_SEG_FLAT && none_rd < INT64_MAX) {
        // Flat block: accept PARTITION_NONE if RD is reasonable
        int64_t threshold = none_rd + (none_rd >> 4); // 6.25% margin
        if (none_rd < best_rd + threshold) {
            // Skip all partition splits
            goto END_PARTITION_SEARCH;
        }
    }
    if (edge_seg == EDGE_SEG_CONTOUR) {
        // Force full partition exploration — never early terminate
        sf->part_sf.ml_partition_search_breakout_thresh[0] = -1;
    }
}
```

**3b. Edge-guided transform search pruning**

For FLAT blocks, the optimal transform is almost always DCT-DCT (the DC coefficient dominates). The encoder can skip trying ADST, Flip-ADST, and Identity transforms. For CONTOUR blocks, directional transforms (ADST variants) may better preserve edge structure, so the full transform search should be preserved.

```c
if (edge_seg == EDGE_SEG_FLAT) {
    // Only try DCT_DCT
    txfm_search_params.use_default_only = 1;
} else if (edge_seg == EDGE_SEG_CONTOUR) {
    // Try all directional transforms
    txfm_search_params.prune_level = 0;
}
```

### Evaluation

| Criterion | Assessment |
|-----------|-----------|
| **Theoretical basis** | Strong — directly analogous to the paper's feedforward vs. cortical refinement separation |
| **Implementation complexity** | Medium — partition search code is complex but well-understood. The edge classification is already computed. The main work is inserting conditional early-termination logic and tuning the thresholds. |
| **Expected impact** | Speed: 10-30% encoder speedup (partition search is 40-60% of encode time; skipping it on flat blocks is a large win). BD-rate: 0-0.2% regression on average (flat blocks lose little from PARTITION_NONE), but potential improvement on edge blocks from more exhaustive search. Perceptual: better edge preservation from forcing full partition search on contour blocks. |
| **Priority** | **1** — Among the highest-impact changes. The speed gain is large and the perceptual argument is strong. |

---

## Gap 4: Overlapping Local Experts (Content-Type-Driven Speed Features)

### What the research says

> "patch-local prediction... overlapping windows... sparse experts... content-specialized decoders" (§4)
>
> "one expert for line art... one for natural image texture... one for text/UI... one for faces... one for motion boundaries" (§4)
>
> "The paper's measurements emphasize small local receptive fields and local synaptic organization." (§4)

The paper argues that the visual system uses *different processing strategies* for different content types, not a single general-purpose pipeline. Different synaptic inputs have different receptive field structures tuned to different features.

### What's implemented

The 4-way classification (FLAT/NORMAL/TEXTURE/CONTOUR) in `aq_edge.c` is a rudimentary form of content classification, but it's one-dimensional (edge density only). It doesn't distinguish:

- Text (high edge density, sharp corners, binary ON/OFF) from natural edges (softer gradients)
- Faces (smooth gradients, skin tones, feature structure) from flat sky
- Line art (thin high-contrast edges, large flat regions) from dense natural texture
- Motion boundaries (temporal discontinuities) from static edges

### What's missing

**4a. Extended block classification taxonomy**

Extend the edge scorer with additional lightweight features computed from the same Sobel pass:

```c
typedef enum {
    CONTENT_FLAT,            // Low edge density, low variance
    CONTENT_SMOOTH_GRADIENT, // Low edge density, moderate variance (sky, skin)
    CONTENT_NATURAL_TEXTURE, // High edge density, low coherence (grass, gravel)
    CONTENT_SHARP_BOUNDARY,  // High edge density, high coherence (text, UI, line art)
    CONTENT_COMPLEX,         // High edge density, high variance, mixed orientation
} ContentType;
```

The additional features beyond edge density:

- **Polarity coherence** (from Gap 1): high coherence = sharp boundary, low = texture
- **Variance** (already computed by AV1's variance-based AQ): distinguishes smooth gradient from truly flat
- **Orientation entropy**: a block where all edges have similar orientation (text, line art) vs. random orientation (natural texture). Computable from `atan2(gy, gx)` binned into 4-8 orientation bins.

**4b. Content-type-driven speed feature selection**

AV1's speed features (`speed_features.h`) are currently set globally per frame based on `--cpu-used`. Content-type classification could override specific speed features per-block:

| Content Type | Speed Feature Overrides |
|-------------|------------------------|
| FLAT | Skip intra mode search, skip transform search, use PARTITION_NONE, skip OBMC, skip warped motion |
| SMOOTH_GRADIENT | Reduce intra modes to smooth predictors (SMOOTH, SMOOTH_V, SMOOTH_H, DC), skip angular modes |
| SHARP_BOUNDARY | Force full intra mode search (all angular modes), full transform search, disable deblocking strength reduction |
| NATURAL_TEXTURE | Standard speed features, but allow identity transform (preserves texture better) |
| COMPLEX | Full search everywhere |

**Implementation approach:**

Add a `content_type` field to `MACROBLOCK` or `MB_MODE_INFO`. Compute it once per superblock during the AQ setup pass. Then, in the speed feature application code (primarily in `set_good_speed_features_framesize_dependent()` and the per-block speed feature checks scattered through `rdopt.c`), add conditional overrides.

This is architecturally similar to how `prune_ref_frame_for_rect_partitions` already varies by block properties.

### Evaluation

| Criterion | Assessment |
|-----------|-----------|
| **Theoretical basis** | Moderate — the paper's local receptive field specialization supports this conceptually, but the mapping from neuroscience to specific codec speed features is an engineering leap, not a direct translation. |
| **Implementation complexity** | High — touching speed features per-block is architecturally invasive in libaom. Many speed features are set per-frame and assume they apply uniformly. Per-block overrides would require plumbing content_type through many functions. |
| **Expected impact** | Speed: 5-20% improvement from aggressive pruning on flat/smooth blocks. BD-rate: 0-0.3% from better mode selection on sharp boundaries. Risk: regressions on content that doesn't fit the taxonomy cleanly. |
| **Priority** | **3** — Good idea but high implementation complexity and risk. Better to implement Gaps 1-3 first, which give most of the benefit with less invasiveness. |

---

## Gap 5: Contour Continuity and Temporal Stability

### What the research says

> "broken edges... ringing near contours... polarity inversions... contour jitter over time" — things humans are very sensitive to (§2.3)
>
> "temporal contour shimmer" (§2, contour integrity)
>
> "temporal contour stability" (§6, structure-aware distortion metric)
>
> "temporal edge flicker" (§6)
>
> "contour displacement... edge sign inversions... fragmentation of continuous boundaries... temporal edge flicker" (§6, brain-inspired metric penalties)
>
> "contour graph" (§Encoder Pass 1)
>
> "contour continuity" (§4, bit allocation priorities)

The paper argues that the brain is exquisitely sensitive to temporal instability of edges. An edge that exists in frame N, disappears in frame N+1, and reappears in N+2 is perceived as "shimmering" or "flickering" — a highly objectionable artifact, especially at low bitrates. This is distinct from and worse than simply having a blurry edge.

### What's implemented

Nothing. All analysis is per-frame and per-block:

- No spatial connectivity between edge pixels (each pixel's Sobel magnitude is compared to a threshold independently)
- No cross-block contour tracking (a contour that spans two superblocks is scored independently in each)
- No temporal persistence (frame N's edge map is not compared to frame N-1's)

### What's missing

**5a. Spatial contour continuity analysis**

Within a frame, after computing the per-pixel edge map, perform connected-component analysis on edge pixels to identify contours. Long, continuous contours are perceptually important; isolated edge pixels are noise.

```c
typedef struct {
    int contour_length;      // Number of connected edge pixels
    int avg_polarity;        // Net gradient direction (from Gap 1)
    int fragmentation_count; // Number of gaps in the contour within the block
} ContourInfo;
```

Blocks that contain long continuous contours get maximum protection (lowest QP, full partition search). Blocks with fragmented short edges get moderate protection.

**Implementation approach:** Connected-component labeling on the binary edge map (edge pixels above threshold). This can be done with a single-pass union-find algorithm on the 8-connected grid. Cost: O(pixels) with very low constant, but requires a per-frame edge bitmap buffer.

**Storage:** 1 bit per pixel. For 1080p: ~259 KB. Acceptable.

**5b. Temporal edge stability tracking**

Store the previous frame's edge map (binary: edge/not-edge per pixel, or per 4×4 block). Compare with the current frame's edge map:

```c
// Per-block temporal edge stability:
typedef enum {
    EDGE_STABLE,        // Edge existed in both frames
    EDGE_APPEARED,      // Edge in current frame but not previous (ON event)
    EDGE_DISAPPEARED,   // Edge in previous frame but not current (OFF event, shimmer risk)
    EDGE_NONE,          // No edge in either frame
} TemporalEdgeState;
```

Blocks containing EDGE_DISAPPEARED are at high risk of contour shimmer. These blocks should receive:

- Lower QP (preserve the edge if it's still present in the source but was quantized away)
- Bias toward the reference frame prediction that preserves the edge (affects mode selection)

**5c. Temporal edge flicker penalty in RD**

Extend `edge_rd.c` to add a flicker penalty:

```c
// If prev_frame had an edge at this location but current reconstruction doesn't,
// add a penalty proportional to the edge's strength in the previous frame.
int64_t flicker_penalty = 0;
for (each pixel in block) {
    if (prev_had_edge[r][c] && !cur_recon_has_edge[r][c]) {
        flicker_penalty += prev_edge_strength[r][c];
    }
}
rd_cost->dist += (flicker_penalty * flicker_alpha_q8) >> 8;
```

This requires access to the previous frame's edge map from within `edge_rd.c`. The edge map would need to be stored as a frame-level buffer in `AV1_COMP` or `AV1_COMMON`.

### Evaluation

| Criterion | Assessment |
|-----------|-----------|
| **Theoretical basis** | Very strong — contour stability is one of the most well-established principles in visual perception. The paper's temporal ON/OFF event model directly predicts shimmer sensitivity. |
| **Implementation complexity** | High — requires per-frame edge bitmap storage, connected-component analysis, temporal comparison, and integration into both AQ and RD. The temporal comparison requires careful handling of motion-compensated alignment (an edge that moved due to camera pan should not be flagged as "disappeared"). |
| **Expected impact** | Perceptual: significant reduction in edge shimmer at low bitrates, especially on animation, text, and UI content. BD-rate (PSNR): likely 0-0.1% regression (allocating bits to edge stability reduces bits for texture). BD-rate (VMAF/SSIM): 0.2-0.5% improvement (perceptual metrics penalize shimmer). |
| **Priority** | **4** — Strong theoretical basis but high complexity and risk of false positives (motion-caused edge changes being treated as shimmer). Best attempted after Gaps 1-3 are validated. |

---

## Gap 6: Orientation Field for Directional Intra Mode Pruning

### What the research says

> "**Orientation preference** emerges from spatial organization of ON/OFF inputs" (§What the paper shows, point 2)
>
> "orientation field" (§Encoder Pass 1)
>
> "orientation histograms per block or patch" (§Top 5, idea 1)
>
> "dominant orientation" (§1, front-end outputs)

The paper's Figure S7 shows that the spatial arrangement of ON and OFF thalamocortical inputs determines the neuron's orientation preference. The brain builds orientation detectors from the spatial pattern of polarity-classified inputs.

### What's implemented

The Sobel kernels in all three files compute `gx` (horizontal gradient) and `gy` (vertical gradient), which together encode orientation as `θ = atan2(gy, gx)`. However, orientation is never extracted or used. The gradients are immediately reduced to magnitude.

### What's missing

**6a. Per-block dominant orientation computation**

During the edge analysis pass in `av1_edge_block_score()`, accumulate an orientation histogram:

```c
// 8 orientation bins (each 22.5°)
#define NUM_ORI_BINS 8
int ori_hist[NUM_ORI_BINS] = {0};

for (each edge pixel where mag > threshold) {
    // Compute angle and bin it
    // Using integer-only approximation to avoid atan2():
    int bin = edge_orientation_bin(gx, gy); // 0-7
    ori_hist[bin]++;
}

// Find dominant orientation
int dominant_bin = argmax(ori_hist);
int dominant_count = ori_hist[dominant_bin];
double orientation_strength = (double)dominant_count / total_edge_pixels;
```

The `edge_orientation_bin()` function can avoid floating-point by using octant detection with comparisons:

```c
static inline int edge_orientation_bin(int gx, int gy) {
    // Map gradient direction to 8 bins using magnitude comparisons
    int ax = abs(gx), ay = abs(gy);
    int bin;
    if (ax > 2 * ay) bin = 0;          // ~horizontal gradient → vertical edge
    else if (ay > 2 * ax) bin = 2;     // ~vertical gradient → horizontal edge
    else bin = 1;                       // ~diagonal
    // Adjust for quadrant
    if (gx < 0) bin = 4 - bin;
    if (gy < 0) bin += 4;
    return bin & 7;
}
```

**6b. Orientation → AV1 intra mode pruning**

AV1 has 56 directional intra prediction modes (angles from 45° to 203° in ~1.5° steps, grouped into 8 nominal modes: V_PRED, H_PRED, D45_PRED, D135_PRED, D113_PRED, D157_PRED, D203_PRED, D67_PRED, plus angle deltas).

The dominant edge orientation in a block strongly predicts which intra prediction mode will be optimal:

| Dominant gradient direction | Edge direction | Best intra modes |
|---------------------------|---------------|-----------------|
| Horizontal gradient (gx dominant) | Vertical edge | V_PRED ± delta |
| Vertical gradient (gy dominant) | Horizontal edge | H_PRED ± delta |
| 45° gradient | 135° edge | D135_PRED ± delta |
| 135° gradient | 45° edge | D45_PRED ± delta |

**Pruning strategy:**

For CONTOUR blocks with strong orientation coherence (orientation_strength > 0.5), prune intra mode candidates to:
1. The directional mode matching the dominant orientation
2. Its two neighboring angular modes
3. DC_PRED and SMOOTH as fallbacks

This prunes ~70% of directional intra modes. The current AV1 encoder tries up to 8+ intra modes; this would reduce it to 4-5 for strongly oriented blocks.

**Integration point:** In `av1_rd_pick_intra_mode_sb()` (in `intra_mode_search.c`), where the candidate mode list is constructed, add:

```c
if (cpi->oxcf.q_cfg.aq_mode == EDGE_AQ && edge_seg == EDGE_SEG_CONTOUR) {
    int dom_ori = av1_get_block_dominant_orientation(cpi, x, bsize, mi_row, mi_col);
    if (dom_ori >= 0) {
        av1_prune_intra_modes_by_orientation(mode_candidates, dom_ori);
    }
}
```

**6c. Orientation → transform type selection**

AV1's transform type selection (DCT vs. ADST vs. Flip-ADST for each axis) is related to edge position within the block. The orientation field provides a prior:

- Vertical edge near left boundary → ADST horizontally (energy concentrated near one side)
- Horizontal edge near top → ADST vertically
- Diagonal edge → consider identity transform on the non-edge axis

This is more speculative and would require careful RD validation.

### Evaluation

| Criterion | Assessment |
|-----------|-----------|
| **Theoretical basis** | Strong — orientation selectivity is one of the most studied properties of V1 neurons, and the paper directly connects it to ON/OFF input spatial organization. The mapping to directional intra modes is natural. |
| **Implementation complexity** | Low-Medium — the orientation computation is simple (integer comparisons on existing gx/gy values). The intra mode pruning integration requires understanding libaom's mode search pipeline but is localized. |
| **Expected impact** | Speed: 3-8% improvement on intra frames (and intra blocks in inter frames) from reduced mode evaluation. BD-rate: 0-0.1% (the pruned modes are rarely optimal for blocks with strong orientation). Perceptual: no change (we're pruning modes that wouldn't be selected anyway). |
| **Priority** | **2** — Clean implementation, good speed benefit, low risk. The orientation computation is nearly free since we already run Sobel. |

---

## Gap Summary: What Has Been Implemented vs. What the Research Proposes

### Concept Coverage Matrix

| Research Concept | Paper Section | Implemented? | Gap ID |
|-----------------|--------------|-------------|--------|
| Sobel edge detection | §1, §2.3 | **Yes** — `aq_edge.c` | — |
| Edge density classification | §1 | **Yes** — 4 segments | — |
| QP modulation by edge class | §2, §Top 5 #2 | **Yes** — segment QP deltas | — |
| Edge-guided motion search effort | §5 | **Designed** — `edge_motion_search.c` | — |
| Edge-aware RD penalty | §6 | **Designed** — `edge_rd.c` | — |
| **ON/OFF polarity** (signed gradient) | §1, §2.3, §Top 5 #1 | **No** — sign discarded | **Gap 1** |
| **Temporal ON/OFF events** | §1, §5, §Top 5 #5 | **No** — single-frame only | **Gap 2** |
| **Two-stage structure → refinement** | §3, §2.4 | **Partial** — motion search modulated, but partition/transform search not | **Gap 3** |
| **Overlapping local experts** | §4 | **No** — classification is 1D (density) | **Gap 4** |
| **Contour continuity & temporal stability** | §2, §6 | **No** — no connectivity or temporal tracking | **Gap 5** |
| **Orientation field** | §1, Figure S7, §Encoder Pass 1 | **No** — gx/gy computed but orientation discarded | **Gap 6** |

### Priority Ranking

| Priority | Gap | Concept | Rationale |
|----------|-----|---------|-----------|
| **1** | Gap 1 | ON/OFF Polarity | Core paper insight; near-zero implementation cost; directly improves all three existing components |
| **1** | Gap 3 | Partition Search Modulation | Largest speed impact (10-30%); reuses existing edge classification; directly maps to paper's two-stage principle |
| **2** | Gap 6 | Orientation Field | Clean speed win (3-8% on intra); low risk; natural extension of existing Sobel computation |
| **2** | Gap 2 | Temporal ON/OFF Events | Strong theoretical basis; good speed win on stable content; medium integration complexity |
| **3** | Gap 4 | Local Experts | Good idea but architecturally invasive; defer until classification taxonomy is validated |
| **4** | Gap 5 | Contour Continuity & Temporal Stability | Strongest perceptual argument but highest complexity; risk of false positives from motion; implement after Gaps 1-3 are validated |

---

## Recommended Implementation Sequence

### Phase 1: Polarity and Orientation (Low-Risk, High-Value)

1. **Add signed polarity coherence to `av1_edge_block_score()`** — Gap 1a
   - Split CONTOUR into CONTOUR_COHERENT and CONTOUR_MIXED
   - ~50 lines of code change in `aq_edge.c`
   - Validate: encode test sequences, check segment map makes intuitive sense

2. **Add signed gradient mismatch to `av1_compute_edge_mismatch()`** — Gap 1b
   - Replace `|mag_src - mag_rec|` with `|gx_src - gx_rec| + |gy_src - gy_rec|`
   - ~20 lines of code change in `edge_rd.c`
   - Validate: BD-rate test on SSIM/VMAF for text, animation, natural content

3. **Add dominant orientation computation** — Gap 6a
   - Compute orientation histogram alongside existing Sobel in `av1_edge_block_score()`
   - ~40 lines of code, integer-only
   - Store dominant orientation per superblock

### Phase 2: Conditional Compute (Speed Impact)

4. **Edge-guided partition search early termination** — Gap 3a
   - FLAT blocks → aggressive PARTITION_NONE acceptance
   - CONTOUR blocks → never early-terminate
   - Integration in `partition_search.c`
   - Validate: speed vs. BD-rate tradeoff on standard test sets

5. **Orientation-guided intra mode pruning** — Gap 6b
   - For CONTOUR blocks with strong orientation, prune to 4-5 intra candidates
   - Integration in `intra_mode_search.c`
   - Validate: BD-rate neutral on directional content

6. **Edge-guided transform search pruning** — Gap 3b
   - FLAT → DCT-only, CONTOUR → full transform search
   - Integration in `txfm_search.c`

### Phase 3: Temporal Intelligence

7. **Temporal change map computation** — Gap 2a
   - Per-block delta from previous reconstructed frame
   - Store as frame-level buffer
   - ~100 lines of new code

8. **Temporal change → motion search modulation** — Gap 2b
   - TEMPORAL_STABLE → aggressive search reduction
   - TEMPORAL_ON/OFF + CONTOUR → maximum search
   - Extension of `edge_motion_search.c`

### Phase 4: Advanced Perceptual (Research-Grade)

9. **Spatial contour continuity** — Gap 5a
   - Connected-component analysis on edge map
   - Contour length → QP modulation weight

10. **Temporal edge stability** — Gap 5b/5c
    - Cross-frame edge persistence tracking
    - Flicker penalty in RD cost
    - Motion-compensated edge comparison (hardest part)

11. **Content-type classification** — Gap 4
    - Extended taxonomy beyond edge density
    - Per-block speed feature overrides

---

## Appendix: Detailed Code Locations for Integration

### Where polarity coherence (Gap 1a) would be added

**File:** `libaom/av1/encoder/aq_edge.c`
**Function:** `av1_edge_block_score()`
**Location:** Inside the `for` loop at lines 126-139, after computing `gx` and `gy`.
**Change:** Add accumulators `sum_gx`, `sum_gy`; after the loop, compute coherence ratio; use coherence to refine the segment selection at lines 147-150.

### Where signed RD mismatch (Gap 1b) would be added

**File:** `libaom-patches/future/edge_rd.c`
**Functions:** `sobel_mag_8bit()`, `sobel_mag_hbd()`, and `av1_compute_edge_mismatch()`
**Change:** Instead of computing magnitude per buffer and then differencing magnitudes, compute per-component gradient for both source and reconstruction, then difference the components: `|src_gx - rec_gx| + |src_gy - rec_gy|`.

### Where orientation (Gap 6) would be added

**File:** `libaom/av1/encoder/aq_edge.c` (computation)
**File:** `libaom/av1/encoder/intra_mode_search.c` (pruning consumer)
**Header:** `libaom/av1/encoder/aq_edge.h` (new function declaration)
**Change:** Add `av1_edge_block_orientation()` returning dominant orientation bin. In intra mode search, prune candidates not aligned with dominant orientation.

### Where partition search modulation (Gap 3) would be added

**File:** `libaom/av1/encoder/partition_search.c`
**Function:** `av1_rd_pick_partition()` or `rd_pick_sqr_partition()` / `rd_pick_rect_partition()`
**Location:** After PARTITION_NONE evaluation, before evaluating SPLIT/HORZ/VERT.
**Change:** Check edge segment; if FLAT and PARTITION_NONE RD is good, skip remaining partition types.

### Where temporal change maps (Gap 2) would be added

**File:** New file `libaom/av1/encoder/temporal_events.c` (or extend `aq_edge.c`)
**Storage:** New buffer in `AV1_COMP` or `EncodeFrameParams`: `uint8_t *temporal_change_map`
**Computation:** During first-pass or lookahead, after reference frame is established.
**Consumers:** `edge_motion_search.c` (search effort), `rdopt.c` (inter/intra bias)

---

## Closing Note

The research document proposes a rich set of brain-inspired principles. The current bhevc implementation captures the most fundamental one — that edge density should influence bit allocation — but discards the richer information that the Sobel operator already computes (sign, orientation) and does not perform any temporal analysis. The six gaps identified here represent a progression from "use what we already compute but throw away" (Gaps 1, 6) through "add lightweight new computation" (Gaps 2, 3) to "build new subsystems" (Gaps 4, 5).

The most impactful next steps are Gaps 1 and 3: adding polarity awareness costs almost nothing and directly addresses the paper's central insight, while partition search modulation offers the largest speed win with the edge classification we already have.
