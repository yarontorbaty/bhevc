# BHEVC Architecture Document

**Brain-inspired Hierarchical Efficient Video Codec**

## Research Foundation

Based on principles from "Thalamic activation of the visual cortex at the single-synapse level" (Science 391, 1349, 2026). The paper demonstrates that the early visual system extracts structure through sparse, signed (ON/OFF) local events rather than dense pixel arrays. This project translates those principles into video encoder features.

Core principles applied:
- **ON/OFF polarity**: the brain classifies edges as luminance-increasing (ON) or luminance-decreasing (OFF), not just "edge present"
- **Orientation selectivity**: edge direction emerges from the spatial arrangement of ON/OFF receptive fields
- **Structure before detail**: robust primitives (edges, contours) are processed cheaply; expensive refinement is applied only where needed
- **Sparse local representation**: not every pixel matters equally; edges and contours carry disproportionate perceptual importance
- **Temporal change events**: the visual system responds to *where brightness changed*, not absolute pixel values

## System Overview

```
bhevc/
├── src/                        Standalone proof-of-concept codec (C)
│   ├── bhevc.h                 Types, constants, API
│   ├── edge_analysis.c         Sobel ON/OFF edge detection, region classification
│   ├── prediction.c            Edge-guided intra/inter prediction
│   ├── transform.c             4x4 DCT, quantization
│   ├── bitstream.c             Exp-Golomb entropy coding
│   ├── encoder.c               Encode pipeline
│   ├── decoder.c               Decode pipeline
│   ├── frame.c                 YUV frame management
│   ├── metrics.c               PSNR, SSIM computation
│   └── main.c                  CLI entry point
│
├── libaom/                     Modified AV1 reference encoder
│   └── av1/encoder/
│       ├── aq_edge.c/h         Edge-aware adaptive quantization (EDGE_AQ)
│       ├── edge_rd.c/h         Contour-gated signed gradient RD penalty
│       ├── edge_motion_search.c/h   Segment-aware motion search modulation
│       ├── temporal_events.c/h      Temporal ON/OFF change map
│       ├── encoder.h           EDGE_AQ=4 enum, temporal_change_map buffer
│       ├── encoder.c           Frame setup hooks
│       ├── partition_search.c  Block-level segment classification
│       ├── intra_mode_search.c Orientation pruning (disabled)
│       ├── motion_search_facade.c  Motion search hooks (active)
│       ├── rdopt.c             RD cost adjustment hook (active)
│       ├── encoder_alloc.h     Buffer deallocation
│       └── av1.cmake           Build integration
│
├── libaom-patches/future/      Designed features not yet stable
│   ├── edge_motion_search.c/h  Full ME modulation design
│   ├── edge_rd.c/h             Full RD adjustment design
│   └── integration_patch.c     Reference showing exact change sites
│
├── Makefile                    Builds standalone codec
├── benchmark.sh                Standalone vs x264/x265 comparison
├── benchmark_av1.sh            AV1 no-AQ vs variance-AQ vs EDGE-AQ
├── bdrate_analysis.py          BD-rate computation (VCEG-M33)
├── download_ctc_sequences.sh   Download standard test sequences
├── subjective_comparison.sh    Generate side-by-side quality comparisons
├── AOM_SUBMISSION.md           Formal AOM contribution document
├── research_gap_analysis.md    Gap analysis vs research paper
│
├── bdrate_*.txt                BD-rate results (CIF, 720p, 1080p, 4K, VMAF, VBR)
└── subjective_results/         Visual comparison images
```

## Component Architecture

### Standalone Codec (`src/`)

A complete video codec demonstrating all brain-inspired concepts in ~2000 lines of C. Processes raw YUV 4:2:0, supports I/P frames with 16x16 macroblocks.

```
Input YUV
    │
    ▼
┌───────────────────────┐
│  edge_analysis.c      │  Sobel → ON/OFF polarity map
│  - edge_compute()     │  Orientation histogram per MB
│  - edge_classify_mbs()│  Region: FLAT / TEXTURE / EDGE
│  - edge_temporal()    │  Temporal ON/OFF change events
└─────────┬─────────────┘
          │
          ▼
┌───────────────────────┐
│  prediction.c         │  Intra: edge-orientation-guided mode
│  - predict_intra_*()  │  Inter: structure-aware SAD + polarity penalty
│  - predict_inter_mb() │  Search range: 4px (flat) → 32px (edge)
└─────────┬─────────────┘
          │
          ▼
┌───────────────────────┐
│  transform.c          │  4x4 normalized DCT-II
│  encoder.c            │  Contour-priority QP: edges QP-6, flat QP+6
│  bitstream.c          │  Exp-Golomb entropy coding
└─────────┬─────────────┘
          │
          ▼
     BHEVC bitstream
```

### AV1/libaom Integration

Extends the production AV1 encoder with brain-inspired features via `--aq-mode=4`.

```
aomenc --aq-mode=4
    │
    ▼
┌────────────────────────────────────────────────────┐
│  encoder.c :: encode_frame_to_data_rate()          │
│    ├── av1_edge_aq_frame_setup()                   │  ← Frame-level segmentation
│    │     Sets SEG_LVL_ALT_Q per segment            │    with texture-masking ratios
│    │     Segments: CONTOUR_COHERENT (1.0x, neutral) │
│    │               CONTOUR_MIXED (1.0x, neutral)    │
│    │               TEXTURE (0.85x, save bits)       │
│    │               NORMAL (0.92x, save bits)        │
│    │               FLAT (1.05x, protect quality)    │
│    │                                                │
│    └── av1_compute_temporal_change_map()            │  ← Per-MI temporal ON/OFF/STABLE
│          Source vs last_source pixel delta           │
└────────────────┬───────────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────────┐
│  partition_search.c :: pick_sb_modes()             │
│    └── setup_block_rdmult()                        │
│          ├── av1_edge_block_score()                │  ← Per-block Sobel edge density
│          │     Classifies block as segment 0-4     │    Returns segment ID
│          │     Sets mbmi->segment_id               │
│          └── set_rdmult(segment_id)                │  ← QP/rdmult follows segment
└────────────────┬───────────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────────┐
│  rdopt.c :: adjust_rdcost()                        │
│    └── av1_edge_rd_adjust()                        │  ← Contour-gated RD penalty
│          Only applied to CONTOUR blocks (seg 0-1)  │    Signed gradient mismatch
│          Alpha ramps 0→16/256 over qindex 80-180   │
└────────────────┬───────────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────────┐
│  motion_search_facade.c                            │
│    ├── av1_edge_adjust_step_param()                │  ← Wider search for contours,
│    │     CONTOUR: step_delta=-1                    │    narrower for flat
│    │     FLAT: step_delta=+1                       │
│    ├── av1_edge_adjust_subpel_params()             │  ← EIGHTH_PEL for contours,
│    │     CONTOUR: 2 iters, eighth-pel              │    HALF_PEL for flat
│    │     FLAT: 1 iter, half-pel                    │
│    └── av1_edge_skip_fractional_search()           │  ← Skip subpel for flat @ speed≥4
└────────────────────────────────────────────────────┘
```

### Data Flow: Edge Analysis

The Sobel operator is the core signal. Applied per-block in `av1_edge_block_score()`:

```
Source pixels (block-relative, HBD-aware)
    │
    ├── Sobel 3x3 → gx, gy (signed gradient components)
    │
    ├── Magnitude: mag² = gx² + gy²
    │   └── Edge pixel if mag² > threshold²
    │
    ├── Edge density = edge_pixels / total_pixels
    │   └── Classifies: CONTOUR (>18%) / TEXTURE (>8%) / NORMAL (>3%) / FLAT
    │
    ├── Polarity coherence (for high-density blocks)
    │   └── |vector_sum(gx,gy)| / scalar_sum(|gx|+|gy|)
    │       High coherence → CONTOUR_COHERENT (sharp boundary)
    │       Low coherence  → CONTOUR_MIXED (natural texture edges)
    │
    └── Orientation histogram (stub, returns -1)
        └── 8-bin integer octant detection from gx, gy
            Dominant bin → guides intra mode pruning (disabled)
```

### Texture Masking Strategy

The key insight differentiating EDGE-AQ from variance-AQ and from naive "boost edges" approaches:

```
Segment              Ratio    Strategy
─────────────────────────────────────────────────────────
CONTOUR_COHERENT     1.00     Neutral — quality protected by edge RD penalty
CONTOUR_MIXED        1.00     Neutral — quality protected by edge RD penalty
TEXTURE              0.85     Save 15% — visual masking hides distortion
NORMAL               0.92     Save  8% — moderate masking
FLAT                 1.05     Protect  — banding/artifacts immediately visible
```

Instead of allocating more bits to edges (which inflates bitrate), we save bits where the human visual system is least sensitive (texture) and protect where it is most sensitive (flat gradients).

### Signed Gradient Mismatch (edge_rd.c)

Applied exclusively to CONTOUR blocks (segments 0-1) to avoid overhead on the ~90% of blocks where it is not helpful:

```
For CONTOUR blocks only:
  edge_mismatch = Σ |src_gx - rec_gx| + |src_gy - rec_gy|
  dist_offset = normalize(edge_mismatch) * alpha
  rd_cost->dist += min(dist_offset, 0.5 * original_dist)

Alpha ramps linearly: 0 at qindex=80, 16/256 at qindex=180
Stronger edge protection at higher QP where edges are most at risk
```

### Temporal Change Map

Computed once per frame in `temporal_events.c`:

```
For each 4x4 MI unit:
    delta = mean(source[block]) - mean(last_source[block])

    if |delta| < threshold → TEMPORAL_STABLE
    if delta > 0           → TEMPORAL_ON  (brightness increased)
    if delta < 0           → TEMPORAL_OFF (brightness decreased)

Storage: 1 byte per MI unit (~518 KB for 1080p)
Status: Computed but not consumed by active features
```

## Feature Status Matrix

| Feature | File | Status | Effect |
|---------|------|--------|--------|
| Edge-aware AQ (5-segment) | aq_edge.c | **Active** | Per-block QP via texture masking ratios |
| Contour-gated signed gradient RD | edge_rd.c | **Active** | Edge preservation on contour blocks only |
| Segment-aware motion search | edge_motion_search.c | **Active** | Wider search + finer subpel for contours, coarser for flat |
| Temporal change map | temporal_events.c | Computed | Not consumed by active code |
| Orientation pruning | intra_mode_search.c | Disabled | `if (0 && ...)` guard |

## Build Instructions

### Standalone codec
```bash
make        # Builds ./bhevc
./bhevc encode -i input.yuv -o out.bhevc -w 352 -h 288 -q 28
./bhevc decode -i out.bhevc -o decoded.yuv -w 352 -h 288
```

### AV1/libaom
```bash
mkdir -p libaom-build && cd libaom-build
cmake ../libaom -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=0 \
  -DENABLE_DOCS=0 -DCONFIG_REALTIME_ONLY=0
make -j$(sysctl -n hw.ncpu) aomenc aomdec
```

### Benchmarks
```bash
bash benchmark.sh         # Standalone vs x264/x265
bash benchmark_av1.sh     # AV1 no-AQ vs variance-AQ vs EDGE-AQ
python3 bdrate_analysis.py --help  # BD-rate computation with many options
```

## Citation

> ["Thalamic activation of the visual cortex at the single-synapse level"](https://www.science.org/doi/10.1126/science.aec9923)
> Science 391, 1349 (2026)
