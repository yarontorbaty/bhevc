#!/bin/bash
set -euo pipefail

# Download standard test sequences for AOM/JVET evaluation.
# Uses Xiph.org derf collection (freely available) and generates
# synthetic sequences for fast smoke testing.
#
# Usage: bash download_ctc_sequences.sh [sequences_dir]

SEQDIR="${1:-sequences}"
mkdir -p "$SEQDIR"

XIPH_BASE="https://media.xiph.org/video/derf/y4m"

echo "=== CTC Sequence Downloader ==="
echo "Target directory: $SEQDIR"
echo ""

XIPH_FILES="
foreman_cif.y4m
akiyo_cif.y4m
coastguard_cif.y4m
container_cif.y4m
news_cif.y4m
mobile_cif.y4m
tempete_cif.y4m
park_joy_420_720p50.y4m
ducks_take_off_420_720p50.y4m
old_town_cross_420_720p50.y4m
"

for file in $XIPH_FILES; do
    dest="$SEQDIR/$file"
    if [ -f "$dest" ]; then
        echo "[skip] $file (already exists)"
        continue
    fi
    url="$XIPH_BASE/$file"
    echo "[download] $file"
    if curl -fSL --progress-bar -o "$dest" "$url"; then
        echo "  OK"
    else
        echo "  FAILED (may not be available at $url)"
        rm -f "$dest"
    fi
done

echo ""
echo "=== Generating synthetic sequences ==="

generate_synth() {
    local name=$1 filter=$2 size=$3
    local dest="$SEQDIR/${name}.y4m"
    if [ -f "$dest" ]; then
        echo "[skip] $name (already exists)"
        return
    fi
    echo "[generate] $name"
    ffmpeg -y -f lavfi -i "${filter}=duration=4:size=${size}:rate=30" \
        -frames:v 100 -pix_fmt yuv420p "$dest" 2>/dev/null && echo "  OK" || echo "  FAILED"
}

generate_synth "edges"      "testsrc2"   "352x288"
generate_synth "testsrc"    "testsrc"    "352x288"
generate_synth "edges_720p" "testsrc2"   "1280x720"

# Mandelbrot and life use different option syntax
for spec in \
    "fractal:mandelbrot=size=352x288:rate=30:maxiter=100" \
    "life:life=size=352x288:rate=30:rule=S23/B3:stitch=1" \
    "fractal_720p:mandelbrot=size=1280x720:rate=30:maxiter=100"; do
    name="${spec%%:*}"
    filter="${spec#*:}"
    dest="$SEQDIR/${name}.y4m"
    if [ -f "$dest" ]; then
        echo "[skip] $name (already exists)"
        continue
    fi
    echo "[generate] $name"
    ffmpeg -y -f lavfi -i "$filter" -frames:v 100 -pix_fmt yuv420p "$dest" 2>/dev/null && echo "  OK" || echo "  FAILED"
done

echo ""
echo "=== Available sequences ==="
ls -lh "$SEQDIR"/*.y4m 2>/dev/null | while read line; do
    echo "  $line"
done
echo ""
echo "Done. Use with: python3 bdrate_analysis.py --sequences-dir $SEQDIR"
