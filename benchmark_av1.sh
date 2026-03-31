#!/bin/bash
set -euo pipefail

AOMENC=./libaom-build/aomenc
AOMDEC=./libaom-build/aomdec
FFMPEG=ffmpeg
OUTDIR=benchmark_av1_results
SEQ_DIR="${OUTDIR}/sequences"
NFRAMES=30
QP_LEVELS="32 40 48 56"
CPU_LEVELS="${CPU_LEVELS:-6}"
THREADS="${THREADS:-1}"

mkdir -p "$SEQ_DIR" "$OUTDIR/encoded" "$OUTDIR/decoded"

echo "=== AV1 Edge-AQ Benchmark ==="
echo "no AQ (0) vs variance AQ (1) vs EDGE-AQ (4)"
echo "cpu-used: $CPU_LEVELS | threads: $THREADS"
echo ""

echo "Generating test sequences..."
$FFMPEG -y -f lavfi -i "testsrc2=duration=2:size=352x288:rate=30" \
  -frames:v $NFRAMES -pix_fmt yuv420p "$SEQ_DIR/edges.y4m" 2>/dev/null
$FFMPEG -y -f lavfi -i "mandelbrot=size=352x288:rate=30:maxiter=100" \
  -frames:v $NFRAMES -pix_fmt yuv420p "$SEQ_DIR/fractal.y4m" 2>/dev/null
$FFMPEG -y -f lavfi -i "life=size=352x288:rate=30:rule=S23/B3:stitch=1" \
  -frames:v $NFRAMES -pix_fmt yuv420p "$SEQ_DIR/life.y4m" 2>/dev/null
$FFMPEG -y -f lavfi -i "testsrc=duration=2:size=352x288:rate=30" \
  -frames:v $NFRAMES -pix_fmt yuv420p "$SEQ_DIR/testsrc.y4m" 2>/dev/null
echo "Done."
echo ""

encode_av1() {
  local seq=$1 qp=$2 aq=$3 label=$4 cpu=$5

  local W H
  read W H < <(ffprobe -v error -select_streams v:0 \
    -show_entries stream=width,height -of csv=p=0 "$SEQ_DIR/${seq}.y4m" | tr ',' ' ')

  local input="$SEQ_DIR/${seq}.y4m"
  local encoded="$OUTDIR/encoded/${seq}_cpu${cpu}_aq${aq}_qp${qp}.ivf"
  local decoded="$OUTDIR/decoded/${seq}_cpu${cpu}_aq${aq}_qp${qp}.y4m"

  local start_ms=$(python3 -c "import time; print(int(time.time()*1000))")
  $AOMENC --codec=av1 \
    --limit=$NFRAMES \
    --end-usage=q --cq-level=$qp \
    --aq-mode=$aq \
    --cpu-used=$cpu --threads=$THREADS \
    --kf-max-dist=8 \
    --passes=1 \
    -o "$encoded" "$input" 2>/dev/null
  local end_ms=$(python3 -c "import time; print(int(time.time()*1000))")
  local enc_time=$((end_ms - start_ms))

  $AOMDEC --codec=av1 -o "$decoded" "$encoded" 2>/dev/null

  local fsize=$(stat -f%z "$encoded" 2>/dev/null || stat -c%s "$encoded")
  local bitrate=$((fsize * 8 * 30 / NFRAMES / 1000))

  local psnr=$($FFMPEG -i "$input" -i "$decoded" \
    -frames:v $NFRAMES -lavfi psnr -f null - 2>&1 | grep -oE 'average:[0-9.]+' | tail -1 | cut -d: -f2)
  local ssim=$($FFMPEG -i "$input" -i "$decoded" \
    -frames:v $NFRAMES -lavfi ssim -f null - 2>&1 | grep -oE 'All:[0-9.]+' | tail -1 | cut -d: -f2)

  local ms_per_frame=$((enc_time / NFRAMES))
  printf "  %-12s | cpu=%d | QP=%-2d | %6d B | %4d kbps | PSNR=%6s | SSIM=%8s | %3d ms/f\n" \
    "$label" "$cpu" "$qp" "$fsize" "$bitrate" "${psnr:-N/A}" "${ssim:-N/A}" "$ms_per_frame"

  rm -f "$decoded"
}

for cpu in $CPU_LEVELS; do
  echo "=== Speed preset: cpu-used=$cpu ==="
  for seq in edges fractal life testsrc; do
    echo "--- Sequence: $seq ---"
    for qp in $QP_LEVELS; do
      encode_av1 "$seq" "$qp" 0 "no-AQ" "$cpu"
      encode_av1 "$seq" "$qp" 1 "variance-AQ" "$cpu"
      encode_av1 "$seq" "$qp" 4 "EDGE-AQ" "$cpu"
      echo "  ---"
    done
    echo ""
  done
done
