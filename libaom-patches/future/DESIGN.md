# Brain-Inspired AV1 Encoder Extensions: Technical Design

## Overview

This document describes two encoder-side features for libaom (AV1) inspired by
the thalamocortical ON/OFF visual processing model from:

> "Thalamic activation of the visual cortex at the single-synapse level"
> *Science* 391, 1349 (2026)

Both features build on the existing **EDGE_AQ** infrastructure (`--aq-mode=4`),
which uses Sobel-based edge density per superblock to classify blocks into four
segments (CONTOUR, TEXTURE, NORMAL, FLAT) and applies per-segment QP deltas.

These two features extend the brain-inspired approach from QP allocation into
two new dimensions of the encoder pipeline: **motion search** and **RD cost**.

---

## Concept 1: Edge-Guided Motion Search Effort

### Motivation

AV1 uses the same motion search effort for all blocks within a frame. The search
range (`step_param`), fractional precision (`forced_stop`), and diagonal checks
(`iters_per_step`) are controlled globally by `--cpu-used` and speed features.

This is suboptimal because:

- **FLAT blocks** (sky, walls, gradients): Motion search adds very little value.
  The prediction error surface is smooth, so the optimal MV is easily found with
  coarse search. Full 1/8-pel refinement wastes cycles.

- **EDGE blocks** (contours, text, graphics): Motion accuracy is critical.
  A 1-pixel MV error on an edge creates visible ringing and broken contours.
  These blocks benefit from wider search ranges and full fractional precision.

The brain analogy: the visual system allocates more processing resources
(synaptic connections, cortical activation) to regions where significant
ON/OFF boundaries exist, while spending minimal effort on uniform regions.

### Algorithm

```
For each block during inter mode search:
  1. Retrieve edge_seg from mbmi->segment_id (already set by EDGE_AQ)
  2. Adjust step_param:
       CONTOUR: step -= 2  (4x larger search radius)
       TEXTURE: step -= 1  (2x larger search radius)
       NORMAL:  no change
       FLAT:    step += 2  (4x smaller search radius)
  3. Adjust subpel parameters:
       CONTOUR: forced_stop = EIGHTH_PEL, iters_per_step = 2
       TEXTURE: forced_stop = EIGHTH_PEL (default iters)
       NORMAL:  no change
       FLAT:    forced_stop = HALF_PEL, iters_per_step = 1
  4. At speed >= 4, skip fractional search entirely for FLAT blocks
```

### Files Modified

| File | Function | Change |
|------|----------|--------|
| `motion_search_facade.c` | `av1_single_motion_search()` | Adjust `step_param` after initial computation |
| `motion_search_facade.c` | `av1_single_motion_search()` | Adjust subpel params after `av1_make_default_subpel_ms_params()` |
| `motion_search_facade.c` | `av1_single_motion_search()` | Gate fractional search on FLAT blocks |
| `av1.cmake` | Build system | Add new source files |

### New Files

| File | Purpose |
|------|---------|
| `edge_motion_search.h` | Public API declarations |
| `edge_motion_search.c` | Implementation |

### Data Flow

```
partition_search.c                  motion_search_facade.c
  setup_block_rdmult()                av1_single_motion_search()
    |                                   |
    v                                   v
  av1_edge_block_score()  -------->  av1_get_edge_segment()
    |                          (reads mbmi->segment_id)
    v                                   |
  mbmi->segment_id = edge_seg           v
                                    av1_edge_adjust_step_param()
                                    av1_edge_adjust_subpel_params()
                                    av1_edge_skip_fractional_search()
```

The edge segment classification is computed once per block (in
`setup_block_rdmult` when EDGE_AQ is active) and reused by the motion
search modulation. No additional Sobel computation is needed.

### Expected Impact

**Content types that benefit most:**
- Animation, anime (strong contours, large flat regions)
- Screen content, UI, text (sharp edges, uniform backgrounds)
- Sports with graphics overlays
- Studio-quality footage with clean edges

**BD-rate estimate:** -1% to -3% on edge-heavy content (anime, screen content)
at speed 3-5, with 5-15% encoder speedup due to reduced effort on flat blocks.
At speed 0-2, quality improvement dominates (estimated -0.5% to -1.5% BD-rate
on edge content) because the search range increase on edge blocks finds better
MVs that reduce residual energy.

**Speed impact:** Net positive (faster). The time saved on flat blocks (reduced
search range, skipped fractional refinement) outweighs the extra time on edge
blocks, because typical video has more flat area than edge area (~60-80% flat
vs 5-15% edge-dominant).

**Risk:** Minimal. All changes are gated behind `aq_mode == EDGE_AQ`, so
standard encoding paths are untouched. The step_param adjustments are clamped
to valid ranges.

---

## Concept 2: Structure-Aware RD Cost

### Motivation

The standard AV1 RD cost is:

```
RD = D + λ·R
```

where `D = SSE` (sum of squared pixel errors). SSE treats all pixel errors
equally — an error on a flat region and an error on an edge contribute the
same distortion per pixel.

But human perception is much more sensitive to edge disruption. A prediction
that achieves low SSE but shifts an edge by 1 pixel, or blurs a sharp contour,
or creates ringing near a boundary, is perceptually worse than one that
preserves the contour structure even at slightly higher overall SSE.

The brain-inspired approach adds an edge-preservation penalty:

```
D_brain = SSE + α · edge_mismatch
```

where `edge_mismatch` = Σ |Sobel_src(x,y) - Sobel_rec(x,y)| over the block,
measuring how much edge structure has been lost or displaced.

### Algorithm

```
For each inter-predicted block during mode decision:
  1. Compute alpha weight based on QP:
       qindex < 80:   alpha = 0     (SSE is sufficient at high quality)
       qindex 80-180: alpha ramps linearly from 0 to 0.25
       qindex > 180:  alpha = 0.25  (maximum edge penalty)
  2. If alpha > 0:
       a. Compute Sobel magnitude at each pixel in source block
       b. Compute Sobel magnitude at each pixel in reconstructed block
       c. edge_mismatch = Σ |sobel_src - sobel_rec| over interior pixels
       d. dist_offset = (edge_mismatch * alpha_q8) >> 8
       e. Clamp dist_offset to at most 50% of original SSE distortion
       f. rd_cost->dist += dist_offset
       g. Recompute rd_cost->rdcost = RDCOST(rdmult, rate, dist)
```

### Why QP-dependent alpha?

At low QP (high quality), SSE is already a good proxy for perceptual quality —
edges are well-preserved because quantization is fine enough to retain gradient
structure. The edge penalty would add computational cost without significant
benefit.

At high QP (low quality), aggressive quantization causes systematic edge
degradation: ringing, blurring, and contour displacement. Here the edge penalty
steers the encoder toward modes that preserve structure, even at slightly
higher SSE cost. This trades a small amount of texture quality for
significantly better edge preservation.

### Files Modified

| File | Function | Change |
|------|----------|--------|
| `rdopt.c` | `adjust_rdcost()` | Add edge penalty when EDGE_AQ is active |
| `rdopt.c` | `adjust_cost()` | Skip for EDGE_AQ (handled by adjust_rdcost) |
| `av1.cmake` | Build system | Add new source files |

### New Files

| File | Purpose |
|------|---------|
| `edge_rd.h` | Public API declarations |
| `edge_rd.c` | Implementation: Sobel mismatch + alpha + RD adjustment |

### Data Flow

```
rdopt.c                           edge_rd.c
  handle_inter_mode()
    |
    v
  (RD computation: rate, dist, rdcost)
    |
    v
  adjust_rdcost()
    |
    v
  av1_edge_rd_adjust()  -------->  av1_edge_rd_alpha(qindex)
    |                                |
    v                                v
  av1_compute_edge_mismatch()     alpha_q8 (Q8 fixed-point weight)
    |
    v
  rd_cost->dist += (mismatch * alpha) >> 8
  rd_cost->rdcost = RDCOST(rdmult, rate, dist)
```

### Key Design Decisions

1. **L1 norm on gradient magnitudes, not L2:** Using |gx|+|gy| as the Sobel
   approximation (instead of sqrt(gx²+gy²)) and summing absolute differences
   (instead of squared differences) keeps the penalty linear and fast. The L1
   norm is more robust to outlier pixels and aligns better with perceptual edge
   sensitivity.

2. **50% distortion clamp:** The edge penalty is clamped to at most half the
   SSE distortion. This prevents the edge metric from dominating SSE in
   pathological cases (e.g., very flat blocks with a single faint edge where
   Sobel noise could cause disproportionate penalty).

3. **Inter-only:** The penalty only applies to inter-predicted blocks. For intra
   mode selection, EDGE_AQ's per-segment QP adjustment already steers the
   encoder toward higher quality on edge blocks. Adding an edge penalty to
   intra RD would double-count the effect.

4. **HBD normalization:** For 10-bit and 12-bit content, Sobel outputs are
   proportionally larger. The mismatch is right-shifted by (bit_depth - 8) to
   normalize to 8-bit scale, ensuring consistent alpha behavior.

### Expected Impact

**Content types that benefit most:**
- Animation and anime (where broken edges cause visible contour artifacts)
- Text and UI rendering (sharp edges are critical)
- Sports graphics and scoreboards
- Any content with strong geometric structure

**BD-rate estimate:** -1% to -4% on perceptual metrics (SSIM, VMAF) for
edge-heavy content at mid-to-high QP (qindex 100-220). The improvement on
PSNR may be smaller (-0.5% to -1%) or neutral, because the feature trades
some texture PSNR for better edge preservation.

**Speed impact:** The Sobel computation on both source and reconstruction adds
cost. For a 64x64 superblock, this is ~3600 multiplies and ~3600 additions per
mode evaluation. On modern hardware this is ~1-3μs per block. With AV1's
multi-mode RD search evaluating 10-50 modes per block, this adds roughly
10-150μs per superblock, which is 3-8% encoder slowdown.

Mitigation: The computation is only performed when alpha > 0 (qindex >= 80),
and only for inter-predicted blocks. At low QP, there is zero overhead.

**Risk:** Medium. The edge penalty changes mode decisions, which can affect
rate control accuracy. The 50% distortion clamp limits the maximum perturbation.
Extensive testing across diverse content at multiple QP points is needed to
validate the alpha curve and ensure no regressions on texture-heavy content.

---

## Interaction Between the Two Features

The two features are complementary and independently valuable:

1. **Edge-guided motion search** improves the quality of motion vectors on edge
   blocks while saving time on flat blocks. Better MVs reduce residual energy,
   which helps at all subsequent stages (transform, quantization, entropy
   coding).

2. **Structure-aware RD cost** biases mode selection toward modes that preserve
   edge structure. Even with perfect MVs, the encoder might choose a mode that
   has lower SSE but breaks edges (e.g., a larger block size that averages across
   an edge). The edge penalty corrects this.

Together, they create a consistent edge-priority pipeline:
- **QP allocation** (existing EDGE_AQ): edges get more bits
- **Motion search** (Concept 1): edges get better MVs
- **Mode decision** (Concept 2): edges get structure-preserving modes

This mirrors the hierarchical refinement observed in the visual cortex:
primitive feedforward signals (edge detection) guide progressively more
sophisticated processing (motion estimation, mode selection).

---

## Testing Plan

### Unit Tests

1. `av1_edge_adjust_step_param`: Verify clamping, verify each segment produces
   expected delta, verify no-op when EDGE_AQ is not active.

2. `av1_compute_edge_mismatch`: Verify zero mismatch for identical buffers,
   verify correct mismatch for synthetic edge patterns, verify HBD equivalence
   with 8-bit (after normalization).

3. `av1_edge_rd_alpha`: Verify piecewise-linear ramp values, verify boundaries.

### Integration Tests

1. Encode test sequences with `--aq-mode=4` and verify no assertion failures,
   no decoder mismatches.

2. Compare BD-rate curves (PSNR, SSIM, VMAF) on:
   - anime: `Sintel` trailer, `BQTerrace`
   - screen: `sc_desktop`, `sc_web_browsing`
   - natural: `Cactus`, `BasketballDrive`, `ParkScene`

3. Verify speed delta: measure `--cpu-used=3` encoding time with and without
   the features on 1080p30 content.

### Regression Tests

1. Ensure `--aq-mode=0,1,2,3` behavior is completely unchanged.
2. Ensure realtime mode (`CONFIG_REALTIME_ONLY`) compiles and runs without
   including the new code.

---

## File Summary

```
libaom-patches/future/
├── DESIGN.md                  — This document
├── edge_motion_search.h       — Concept 1: header
├── edge_motion_search.c       — Concept 1: implementation
├── edge_rd.h                  — Concept 2: header
├── edge_rd.c                  — Concept 2: implementation
└── integration_patch.c        — Exact change sites in existing libaom files
```
