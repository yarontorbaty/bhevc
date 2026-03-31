#!/bin/bash
set -euo pipefail

# Generate side-by-side subjective comparison frames.
#
# Extracts frames from edge-heavy sequences encoded at QP=48
# with no-AQ, variance-AQ, and EDGE-AQ, then creates
# side-by-side crop comparisons for visual inspection.
#
# Usage: bash subjective_comparison.sh [sequences_dir] [output_dir]

SEQDIR="${1:-sequences}"
OUTDIR="${2:-subjective_results}"
AOMENC=./libaom-build/aomenc
AOMDEC=./libaom-build/aomdec

QP=48
CPU=6
LIMIT=30
FRAME_NUMS="0 14 29"

mkdir -p "$OUTDIR/encoded" "$OUTDIR/decoded" "$OUTDIR/frames" "$OUTDIR/crops" "$OUTDIR/sidebyside"

SEQUENCES="edges testsrc"
if [ -f "$SEQDIR/foreman_cif.y4m" ]; then
    SEQUENCES="$SEQUENCES foreman_cif"
fi
if [ -f "$SEQDIR/news_cif.y4m" ]; then
    SEQUENCES="$SEQUENCES news_cif"
fi
if [ -f "$SEQDIR/mobile_cif.y4m" ]; then
    SEQUENCES="$SEQUENCES mobile_cif"
fi

echo "=== Subjective Comparison Generator ==="
echo "QP=$QP | cpu-used=$CPU | Sequences: $SEQUENCES"
echo ""

for seq in $SEQUENCES; do
    input="$SEQDIR/${seq}.y4m"
    if [ ! -f "$input" ]; then
        echo "[skip] $seq (not found)"
        continue
    fi

    echo "--- $seq ---"

    for aq in 0 1 4; do
        case $aq in
            0) label="none";;
            1) label="variance";;
            4) label="edge";;
        esac

        ivf="$OUTDIR/encoded/${seq}_aq${aq}.ivf"
        dec="$OUTDIR/decoded/${seq}_aq${aq}.y4m"

        echo "  Encoding aq=$aq ($label)..."
        $AOMENC "$input" \
            --output="$ivf" \
            --cq-level=$QP --aq-mode=$aq \
            --cpu-used=$CPU --threads=1 \
            --limit=$LIMIT --end-usage=q --passes=1 \
            --kf-max-dist=8 --ivf 2>/dev/null

        $AOMDEC --codec=av1 -o "$dec" "$ivf" 2>/dev/null

        fsize=$(stat -f%z "$ivf" 2>/dev/null || stat -c%s "$ivf")
        echo "    File size: $fsize bytes"

        for fnum in $FRAME_NUMS; do
            frame_out="$OUTDIR/frames/${seq}_aq${aq}_f${fnum}.png"
            ffmpeg -y -i "$dec" -vf "select=eq(n\\,$fnum)" -vsync vfr \
                -frames:v 1 "$frame_out" 2>/dev/null
        done
    done

    # Extract reference frames
    for fnum in $FRAME_NUMS; do
        ref_frame="$OUTDIR/frames/${seq}_ref_f${fnum}.png"
        ffmpeg -y -i "$input" -vf "select=eq(n\\,$fnum)" -vsync vfr \
            -frames:v 1 "$ref_frame" 2>/dev/null
    done

    W=$(ffprobe -v error -select_streams v:0 \
        -show_entries stream=width -of csv=p=0 "$input")
    H=$(ffprobe -v error -select_streams v:0 \
        -show_entries stream=height -of csv=p=0 "$input")

    CROP_W=$((W / 3))
    CROP_H=$((H / 3))
    CROP_X=$(( (W - CROP_W) / 2 ))
    CROP_Y=$(( (H - CROP_H) / 2 ))

    echo "  Generating side-by-side comparisons (crop: ${CROP_W}x${CROP_H}+${CROP_X}+${CROP_Y})..."

    for fnum in $FRAME_NUMS; do
        for aq in 0 1 4; do
            src="$OUTDIR/frames/${seq}_aq${aq}_f${fnum}.png"
            crop="$OUTDIR/crops/${seq}_aq${aq}_f${fnum}_crop.png"
            ffmpeg -y -i "$src" -vf "crop=${CROP_W}:${CROP_H}:${CROP_X}:${CROP_Y}" \
                "$crop" 2>/dev/null
        done

        ref_src="$OUTDIR/frames/${seq}_ref_f${fnum}.png"
        ref_crop="$OUTDIR/crops/${seq}_ref_f${fnum}_crop.png"
        ffmpeg -y -i "$ref_src" -vf "crop=${CROP_W}:${CROP_H}:${CROP_X}:${CROP_Y}" \
            "$ref_crop" 2>/dev/null

        sbs="$OUTDIR/sidebyside/${seq}_f${fnum}_comparison.png"
        ffmpeg -y \
            -i "$OUTDIR/crops/${seq}_ref_f${fnum}_crop.png" \
            -i "$OUTDIR/crops/${seq}_aq0_f${fnum}_crop.png" \
            -i "$OUTDIR/crops/${seq}_aq1_f${fnum}_crop.png" \
            -i "$OUTDIR/crops/${seq}_aq4_f${fnum}_crop.png" \
            -filter_complex "
                [0]drawtext=text='Reference':x=5:y=5:fontsize=14:fontcolor=yellow:box=1:boxcolor=black@0.5[r];
                [1]drawtext=text='No AQ (mode=0)':x=5:y=5:fontsize=14:fontcolor=yellow:box=1:boxcolor=black@0.5[a];
                [2]drawtext=text='Variance AQ (mode=1)':x=5:y=5:fontsize=14:fontcolor=yellow:box=1:boxcolor=black@0.5[b];
                [3]drawtext=text='EDGE AQ (mode=4)':x=5:y=5:fontsize=14:fontcolor=yellow:box=1:boxcolor=black@0.5[c];
                [r][a]hstack[top];
                [b][c]hstack[bottom];
                [top][bottom]vstack" \
            "$sbs" 2>/dev/null
        echo "    Created: $sbs"
    done

    echo ""
done

echo "=== Full-frame comparisons ==="
for seq in $SEQUENCES; do
    input="$SEQDIR/${seq}.y4m"
    if [ ! -f "$input" ]; then continue; fi

    for fnum in $FRAME_NUMS; do
        full="$OUTDIR/sidebyside/${seq}_f${fnum}_fullframe.png"
        if [ ! -f "$OUTDIR/frames/${seq}_ref_f${fnum}.png" ]; then continue; fi
        ffmpeg -y \
            -i "$OUTDIR/frames/${seq}_ref_f${fnum}.png" \
            -i "$OUTDIR/frames/${seq}_aq0_f${fnum}.png" \
            -i "$OUTDIR/frames/${seq}_aq1_f${fnum}.png" \
            -i "$OUTDIR/frames/${seq}_aq4_f${fnum}.png" \
            -filter_complex "
                [0]drawtext=text='Reference':x=5:y=5:fontsize=16:fontcolor=yellow:box=1:boxcolor=black@0.5[r];
                [1]drawtext=text='No AQ':x=5:y=5:fontsize=16:fontcolor=yellow:box=1:boxcolor=black@0.5[a];
                [2]drawtext=text='Variance AQ':x=5:y=5:fontsize=16:fontcolor=yellow:box=1:boxcolor=black@0.5[b];
                [3]drawtext=text='EDGE AQ':x=5:y=5:fontsize=16:fontcolor=yellow:box=1:boxcolor=black@0.5[c];
                [r][a]hstack[top];
                [b][c]hstack[bottom];
                [top][bottom]vstack" \
            "$full" 2>/dev/null
        echo "  Created: $full"
    done
done

echo ""
echo "=== Summary ==="
echo "Side-by-side crops: $OUTDIR/sidebyside/*_comparison.png"
echo "Full-frame comparisons: $OUTDIR/sidebyside/*_fullframe.png"
echo "Individual frames: $OUTDIR/frames/"
echo ""
echo "Done."
