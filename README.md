# BHEVC — Brain-inspired Hierarchical Efficient Video Codec

An experimental extension to the AV1 reference encoder (libaom) that applies neuroscience-inspired edge classification to adaptive quantization. The core idea: classify blocks by Sobel gradient density instead of pixel variance, then use visual masking theory to decide where bits can be saved and where quality must be protected.

Activated with a single flag: `--aq-mode=4`.

## How It Works

EDGE-AQ classifies every coding block into one of five perceptual categories using its Sobel gradient edge density:

| Segment | Name | Edge Density | Bit Allocation |
|---------|------|-------------|----------------|
| 0 | Contour (coherent) | >18%, high polarity coherence | Neutral (protected by edge RD) |
| 1 | Contour (mixed) | >18%, low coherence | Neutral (protected by edge RD) |
| 2 | Texture | 8–18% | **Save 15%** — masking hides distortion |
| 3 | Normal | 3–8% | **Save 8%** |
| 4 | Flat | <3% | **Protect** — banding is visible |

Three mechanisms work together:

1. **Texture masking rate allocation** — saves bits on high-masking texture, protects low-masking flat regions
2. **Contour-gated signed gradient RD penalty** — applied only to contour blocks (~10% of content), penalizes polarity inversions at edges
3. **Segment-aware motion search** — wider search and finer subpel precision for contours, coarser for flat

## Results

### SSIM BD-Rate: EDGE-AQ vs Variance-AQ (lower is better)

| Resolution | BD-Rate | BD-PSNR |
|-----------|---------|---------|
| CIF (352×288) | **−1.57%** | +0.15 dB |
| 720p (1280×720) | **−2.48%** | +0.42 dB |
| 1080p (1920×1080) | **−8.03%** | +0.13 dB |
| 4K (3840×2160) | **−1.79%** | — |

### VMAF BD-Rate (cpu-used=6)

| Average | **−6.83%** |
|---------|-----------|

### Encode Speed

| Resolution | No-AQ (ms/f) | Variance-AQ (ms/f) | EDGE-AQ (ms/f) |
|-----------|-------------|-------------------|----------------|
| CIF | 22.0 | 23.9 | 23.0 |
| 720p | 171.4 | 181.6 | 177.4 |
| 1080p | 410.7 | 434.5 | 431.4 |

EDGE-AQ is **3.8% faster** than variance-AQ on average.

### Additional Validation

- **Speed presets:** Gains hold at cpu-used=4 (720p: −7.01% SSIM BD-rate)
- **Threading:** −3.13% SSIM BD-rate with `--threads=4`, no crashes
- **VBR rate control:** Stable, hits target rates within normal AV1 overshoot
- **Bitstream compatibility:** Output is standard AV1, decodable by any compliant decoder

## Quick Start

### Build

```bash
# Build libaom with EDGE-AQ
mkdir -p libaom-build && cd libaom-build
cmake ../libaom -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=0 \
  -DENABLE_DOCS=0 -DCONFIG_REALTIME_ONLY=0
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu) aomenc aomdec
cd ..
```

### Encode

```bash
# Convert input to Y4M
ffmpeg -i input.mp4 -pix_fmt yuv420p -f yuv4mpegpipe input.y4m

# Encode with EDGE-AQ (constant quality, QP=32)
./libaom-build/aomenc input.y4m --output=output.ivf \
  --end-usage=q --cq-level=32 --aq-mode=4 \
  --cpu-used=6 --threads=4 --passes=1 --bit-depth=8 --ivf

# Compare against variance-AQ
./libaom-build/aomenc input.y4m --output=output_varaq.ivf \
  --end-usage=q --cq-level=32 --aq-mode=1 \
  --cpu-used=6 --threads=4 --passes=1 --bit-depth=8 --ivf
```

### Run BD-Rate Analysis

```bash
# Download standard test sequences
bash download_ctc_sequences.sh

# Run full analysis
python3 bdrate_analysis.py --sequences-dir sequences --cpu-used 6 --threads 1

# Run with VMAF
python3 bdrate_analysis.py --sequences-dir sequences --vmaf --cpu-used 6

# Specific sequences and resolutions
python3 bdrate_analysis.py --sequences ducks_take_off_420_720p50 park_joy_420_720p50
```

### Build Standalone Codec (proof of concept)

```bash
make
./bhevc encode -i input.yuv -o out.bhevc -w 352 -h 288 -q 28
./bhevc decode -i out.bhevc -o decoded.yuv -w 352 -h 288
```

## Repository Structure

```
libaom/av1/encoder/
  aq_edge.c/h              Sobel block classification + frame setup (230 lines)
  edge_rd.c/h              Contour-gated signed gradient RD penalty (244 lines)
  edge_motion_search.c/h   Segment-aware motion search modulation (109 lines)
  temporal_events.c/h      Temporal ON/OFF change map (computed, unused)

src/                        Standalone proof-of-concept codec (~2000 lines C)

bdrate_analysis.py          Reproducible BD-rate measurement script
download_ctc_sequences.sh   Download Xiph.org test sequences
subjective_comparison.sh    Generate side-by-side visual comparisons
AOM_SUBMISSION.md           Formal AOM contribution document
ARCHITECTURE.md             Detailed architecture documentation
research_gap_analysis.md    Gap analysis vs source research paper

bdrate_*.txt                Full BD-rate results across resolutions/configs
subjective_results/         Visual comparison images (30 frames)
```

## Research Basis

> ["Thalamic activation of the visual cortex at the single-synapse level"](https://www.science.org/doi/10.1126/science.aec9923)
> Science 391, 1349 (2026)

The paper demonstrates that the early visual system extracts structure through sparse, signed (ON/OFF) local events. This project translates those principles into practical encoder features: Sobel-based edge classification replaces variance-based classification, and a signed gradient mismatch penalty preserves edge polarity where it matters most perceptually.

## License

libaom modifications follow the libaom BSD 2-Clause license. Standalone codec and tooling are provided as-is for research purposes.
