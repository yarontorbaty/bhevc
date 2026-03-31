#!/bin/bash
#
# BHEVC Benchmark — compare brain-inspired codec vs x264 (H.264) and x265 (H.265)
#
# Generates test sequences, encodes at multiple QP levels, measures:
#   - File size (compression ratio)
#   - PSNR (Y-channel peak signal-to-noise ratio)
#   - SSIM (structural similarity)
#   - Encoding speed (ms per frame)
#
set -euo pipefail

BHEVC=./bhevc
FFMPEG=ffmpeg
OUTDIR=benchmark_results
W=352
H=288
NFRAMES=30
GOP=8
QP_LEVELS="22 28 34 40"

mkdir -p "$OUTDIR/sequences" "$OUTDIR/encoded" "$OUTDIR/decoded"

SEPARATOR="────────────────────────────────────────────────────────────────────────────────"

echo "╔══════════════════════════════════════════════════════════════════════════════╗"
echo "║              BHEVC Benchmark — Brain-Inspired Video Codec                  ║"
echo "║         Comparing: BHEVC vs x264 (H.264) vs x265 (H.265)                  ║"
echo "╚══════════════════════════════════════════════════════════════════════════════╝"
echo ""

# ─── Generate test sequences ───

echo "▸ Generating test sequences (${W}x${H}, ${NFRAMES} frames)..."

# Sequence 1: testsrc2 — sharp edges, text, color bars, geometric patterns
# Best case for brain-inspired: strong contours, flat regions, clear structure
$FFMPEG -y -f lavfi -i "testsrc2=duration=$((NFRAMES/30+1)):size=${W}x${H}:rate=30" \
  -frames:v $NFRAMES -pix_fmt yuv420p -f rawvideo \
  "$OUTDIR/sequences/edges_${W}x${H}.yuv" 2>/dev/null
echo "  ✓ edges (testsrc2: geometric, text, color bars)"

# Sequence 2: mandelbrot — complex natural-like fractal texture
# Harder for brain-inspired: mixed edge/texture regions
$FFMPEG -y -f lavfi -i "mandelbrot=size=${W}x${H}:rate=30:maxiter=100" \
  -frames:v $NFRAMES -pix_fmt yuv420p -f rawvideo \
  "$OUTDIR/sequences/fractal_${W}x${H}.yuv" 2>/dev/null
echo "  ✓ fractal (mandelbrot: complex texture + edges)"

# Sequence 3: SMPTE bars + moving text overlay — broadcast-style content
$FFMPEG -y -f lavfi \
  -i "smptebars=duration=$((NFRAMES/30+1)):size=${W}x${H}:rate=30" \
  -frames:v $NFRAMES -pix_fmt yuv420p -f rawvideo \
  "$OUTDIR/sequences/bars_${W}x${H}.yuv" 2>/dev/null
echo "  ✓ bars (SMPTE: flat regions + sharp boundaries)"

SEQUENCES="edges fractal bars"

# ─── Encoding functions ───

encode_bhevc() {
    local seq=$1 qp=$2
    local input="$OUTDIR/sequences/${seq}_${W}x${H}.yuv"
    local encoded="$OUTDIR/encoded/${seq}_bhevc_qp${qp}.bhevc"
    local decoded="$OUTDIR/decoded/${seq}_bhevc_qp${qp}.yuv"

    local start_ms=$(python3 -c "import time; print(int(time.time()*1000))")
    $BHEVC encode -i "$input" -o "$encoded" -w $W -h $H -q $qp -g $GOP -n $NFRAMES 2>/dev/null
    local end_ms=$(python3 -c "import time; print(int(time.time()*1000))")
    local enc_time=$((end_ms - start_ms))

    $BHEVC decode -i "$encoded" -o "$decoded" -w $W -h $H -n $NFRAMES 2>/dev/null

    local fsize=$(stat -f%z "$encoded" 2>/dev/null || stat -c%s "$encoded")
    local bitrate=$((fsize * 8 * 30 / NFRAMES / 1000))

    local metrics
    metrics=$($BHEVC metrics -a "$input" -b "$decoded" -w $W -h $H -n $NFRAMES 2>/dev/null | tail -1)
    local psnr=$(echo "$metrics" | awk '{print $NF}' | head -1)
    # re-parse: "Average over N frames: PSNR=XX.XX dB  SSIM=XX.XXXXXX"
    psnr=$(echo "$metrics" | grep -oE 'PSNR=[0-9.]+' | cut -d= -f2)
    local ssim=$(echo "$metrics" | grep -oE 'SSIM=[0-9.]+' | cut -d= -f2)

    local ms_per_frame=$((enc_time * 1000 / NFRAMES / 1000))
    printf "  bhevc  │ QP=%-2d │ %7d B │ %5d kbps │ PSNR=%6.2f │ SSIM=%8.6f │ %4d ms/f\n" \
        "$qp" "$fsize" "$bitrate" "$psnr" "$ssim" "$ms_per_frame"
}

encode_x264() {
    local seq=$1 qp=$2
    local input="$OUTDIR/sequences/${seq}_${W}x${H}.yuv"
    local encoded="$OUTDIR/encoded/${seq}_x264_qp${qp}.264"
    local decoded="$OUTDIR/decoded/${seq}_x264_qp${qp}.yuv"

    local start_ms=$(python3 -c "import time; print(int(time.time()*1000))")
    $FFMPEG -y -f rawvideo -pix_fmt yuv420p -s ${W}x${H} -r 30 -i "$input" \
        -frames:v $NFRAMES -c:v libx264 -preset medium -qp $qp -g $GOP \
        -bf 0 "$encoded" 2>/dev/null
    local end_ms=$(python3 -c "import time; print(int(time.time()*1000))")
    local enc_time=$((end_ms - start_ms))

    $FFMPEG -y -i "$encoded" -f rawvideo -pix_fmt yuv420p "$decoded" 2>/dev/null

    local fsize=$(stat -f%z "$encoded" 2>/dev/null || stat -c%s "$encoded")
    local bitrate=$((fsize * 8 * 30 / NFRAMES / 1000))

    # use ffmpeg for PSNR/SSIM
    local psnr_line
    psnr_line=$($FFMPEG -f rawvideo -pix_fmt yuv420p -s ${W}x${H} -r 30 -i "$input" \
        -f rawvideo -pix_fmt yuv420p -s ${W}x${H} -r 30 -i "$decoded" \
        -frames:v $NFRAMES -lavfi psnr -f null - 2>&1 | grep "average" | tail -1)
    local psnr=$(echo "$psnr_line" | grep -oE 'average:[0-9.]+' | cut -d: -f2)

    local ssim_line
    ssim_line=$($FFMPEG -f rawvideo -pix_fmt yuv420p -s ${W}x${H} -r 30 -i "$input" \
        -f rawvideo -pix_fmt yuv420p -s ${W}x${H} -r 30 -i "$decoded" \
        -frames:v $NFRAMES -lavfi ssim -f null - 2>&1 | grep "All:" | tail -1)
    local ssim=$(echo "$ssim_line" | grep -oE 'All:[0-9.]+' | cut -d: -f2)

    local ms_per_frame=$((enc_time * 1000 / NFRAMES / 1000))
    printf "  x264   │ QP=%-2d │ %7d B │ %5d kbps │ PSNR=%6.2f │ SSIM=%8.6f │ %4d ms/f\n" \
        "$qp" "$fsize" "$bitrate" "$psnr" "$ssim" "$ms_per_frame"
}

encode_x265() {
    local seq=$1 qp=$2
    local input="$OUTDIR/sequences/${seq}_${W}x${H}.yuv"
    local encoded="$OUTDIR/encoded/${seq}_x265_qp${qp}.265"
    local decoded="$OUTDIR/decoded/${seq}_x265_qp${qp}.yuv"

    local start_ms=$(python3 -c "import time; print(int(time.time()*1000))")
    $FFMPEG -y -f rawvideo -pix_fmt yuv420p -s ${W}x${H} -r 30 -i "$input" \
        -frames:v $NFRAMES -c:v libx265 -preset medium -qp $qp -g $GOP \
        -bf 0 -x265-params "bframes=0:keyint=$GOP:min-keyint=$GOP" \
        "$encoded" 2>/dev/null
    local end_ms=$(python3 -c "import time; print(int(time.time()*1000))")
    local enc_time=$((end_ms - start_ms))

    $FFMPEG -y -i "$encoded" -f rawvideo -pix_fmt yuv420p "$decoded" 2>/dev/null

    local fsize=$(stat -f%z "$encoded" 2>/dev/null || stat -c%s "$encoded")
    local bitrate=$((fsize * 8 * 30 / NFRAMES / 1000))

    local psnr_line
    psnr_line=$($FFMPEG -f rawvideo -pix_fmt yuv420p -s ${W}x${H} -r 30 -i "$input" \
        -f rawvideo -pix_fmt yuv420p -s ${W}x${H} -r 30 -i "$decoded" \
        -frames:v $NFRAMES -lavfi psnr -f null - 2>&1 | grep "average" | tail -1)
    local psnr=$(echo "$psnr_line" | grep -oE 'average:[0-9.]+' | cut -d: -f2)

    local ssim_line
    ssim_line=$($FFMPEG -f rawvideo -pix_fmt yuv420p -s ${W}x${H} -r 30 -i "$input" \
        -f rawvideo -pix_fmt yuv420p -s ${W}x${H} -r 30 -i "$decoded" \
        -frames:v $NFRAMES -lavfi ssim -f null - 2>&1 | grep "All:" | tail -1)
    local ssim=$(echo "$ssim_line" | grep -oE 'All:[0-9.]+' | cut -d: -f2)

    local ms_per_frame=$((enc_time * 1000 / NFRAMES / 1000))
    printf "  x265   │ QP=%-2d │ %7d B │ %5d kbps │ PSNR=%6.2f │ SSIM=%8.6f │ %4d ms/f\n" \
        "$qp" "$fsize" "$bitrate" "$psnr" "$ssim" "$ms_per_frame"
}

# ─── Run benchmarks ───

RESULTS_FILE="$OUTDIR/results.txt"
: > "$RESULTS_FILE"

for seq in $SEQUENCES; do
    echo ""
    echo "$SEPARATOR"
    echo "  Sequence: $seq"
    echo "$SEPARATOR"
    echo "  Codec  │  QP   │    Size   │  Bitrate  │    PSNR    │     SSIM     │  Speed"
    echo "  ───────┼───────┼──────────┼───────────┼────────────┼──────────────┼─────────"

    for qp in $QP_LEVELS; do
        encode_bhevc "$seq" "$qp"
        encode_x264  "$seq" "$qp"
        encode_x265  "$seq" "$qp"
        echo "  ───────┼───────┼──────────┼───────────┼────────────┼──────────────┼─────────"
    done
done | tee -a "$RESULTS_FILE"

echo ""
echo "$SEPARATOR"
echo "  ANALYSIS"
echo "$SEPARATOR"
echo ""
echo "  Key observations:"
echo "  • BHEVC uses brain-inspired ON/OFF edge analysis for region classification"
echo "  • Edge regions get QP-6 (4x more bits); flat regions get QP+6 (4x fewer bits)"
echo "  • Motion search range adapts: 4px (flat) → 16px (texture) → 32px (edge)"
echo "  • Structure-aware SAD penalizes MVs that break edge polarity"
echo ""
echo "  Compare at equivalent bitrates to see quality profile differences."
echo "  BHEVC should show better edge preservation (SSIM) on structured content"
echo "  like 'edges' and 'bars' sequences."
echo ""
echo "  Note: BHEVC is a proof-of-concept without CABAC, sub-pixel ME, deblocking,"
echo "  or rate control. x264/x265 have decades of optimization. The comparison"
echo "  demonstrates the brain-inspired features, not raw compression efficiency."
echo ""
echo "  Results saved to: $OUTDIR/"
