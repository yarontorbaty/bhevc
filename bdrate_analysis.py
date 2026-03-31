#!/usr/bin/env python3
"""
BD-rate analysis for EDGE-AQ (--aq-mode=4) in libaom.

Encodes test sequences at multiple QP points with different AQ modes,
measures PSNR/SSIM/VMAF, and computes Bjontegaard Delta Rate (BD-rate)
using the VCEG-M33 cubic interpolation method.

Supports:
  - Custom sequence directories (--sequences-dir) with auto-resolution detection
  - Multiple cpu-used presets (--cpu-used)
  - VMAF measurement via ffmpeg libvmaf
  - VBR/CBR rate control modes (--rate-modes)
  - Multi-threaded encoding (--threads)
  - Encode time tracking
"""

import argparse
import subprocess
import os
import sys
import re
import json
import math
import time
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
AOMENC = BASE_DIR / "libaom-build" / "aomenc"
AOMDEC = BASE_DIR / "libaom-build" / "aomdec"
DEFAULT_SEQ_DIR = BASE_DIR / "sequences"
WORK_DIR = BASE_DIR / "bdrate_work"

AQ_MODES = {0: "none", 1: "variance", 4: "edge"}
CQ_POINTS = [32, 40, 48, 56]


def parse_args():
    p = argparse.ArgumentParser(description="BD-rate analysis for EDGE-AQ")
    p.add_argument("--sequences-dir", type=Path, default=DEFAULT_SEQ_DIR,
                    help="Directory containing .y4m test sequences")
    p.add_argument("--sequences", nargs="*", default=None,
                    help="Specific sequence names (without .y4m). Default: all in dir")
    p.add_argument("--cpu-used", nargs="*", type=int, default=[6],
                    help="Speed presets to test (default: 6)")
    p.add_argument("--qp-points", nargs="*", type=int, default=CQ_POINTS,
                    help="QP/CQ-level points for CQ mode")
    p.add_argument("--threads", type=int, default=1,
                    help="Number of encoder threads")
    p.add_argument("--limit", type=int, default=100,
                    help="Max frames to encode per sequence")
    p.add_argument("--rate-modes", nargs="*", default=["cq"],
                    choices=["cq", "vbr", "cbr"],
                    help="Rate control modes to test")
    p.add_argument("--vbr-targets", nargs="*", type=int,
                    default=[200, 500, 1000, 2000],
                    help="Target bitrates (kbps) for VBR/CBR modes")
    p.add_argument("--no-vmaf", action="store_true",
                    help="Skip VMAF measurement")
    p.add_argument("--output", type=Path, default=None,
                    help="Output file (default: bdrate_results_<timestamp>.txt)")
    return p.parse_args()


def run(cmd, **kwargs):
    result = subprocess.run(cmd, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        print(f"  COMMAND FAILED: {' '.join(str(c) for c in cmd)}")
        print(f"  STDERR: {result.stderr[:500]}")
        raise RuntimeError(f"Command failed with exit code {result.returncode}")
    return result


def detect_y4m_resolution(y4m_path):
    """Parse Y4M header to extract width, height, fps."""
    with open(y4m_path, "rb") as f:
        header = f.read(256).split(b"\n")[0].decode("ascii", errors="ignore")
    if not header.startswith("YUV4MPEG2"):
        raise ValueError(f"Not a valid Y4M file: {y4m_path}")
    w = h = fps_num = fps_den = None
    for token in header.split():
        if token.startswith("W"):
            w = int(token[1:])
        elif token.startswith("H"):
            h = int(token[1:])
        elif token.startswith("F"):
            parts = token[1:].split(":")
            fps_num, fps_den = int(parts[0]), int(parts[1])
    fps = fps_num / fps_den if fps_num and fps_den else 30.0
    return w, h, fps


def check_vmaf_available():
    """Check if ffmpeg has libvmaf support."""
    try:
        result = subprocess.run(["ffmpeg", "-filters"], capture_output=True, text=True)
        return "libvmaf" in result.stdout
    except FileNotFoundError:
        return False


def encode_cq(seq_path, output_ivf, qp, aq_mode, cpu_used, threads, limit):
    """Encode with constant-quality (CQ) mode. Returns (file_size, elapsed_ms)."""
    cmd = [
        str(AOMENC), str(seq_path),
        f"--output={output_ivf}",
        f"--cq-level={qp}",
        f"--aq-mode={aq_mode}",
        f"--cpu-used={cpu_used}",
        f"--threads={threads}",
        f"--limit={limit}",
        "--end-usage=q", "--passes=1",
        "--kf-max-dist=8", "--bit-depth=8", "--ivf",
    ]
    t0 = time.monotonic()
    run(cmd)
    elapsed_ms = (time.monotonic() - t0) * 1000
    return os.path.getsize(output_ivf), elapsed_ms


def encode_rate_controlled(seq_path, output_ivf, target_kbps, aq_mode,
                           cpu_used, threads, limit, mode="vbr"):
    """Encode with VBR or CBR mode. Returns (file_size, elapsed_ms)."""
    end_usage = mode
    cmd = [
        str(AOMENC), str(seq_path),
        f"--output={output_ivf}",
        f"--target-bitrate={target_kbps}",
        f"--aq-mode={aq_mode}",
        f"--cpu-used={cpu_used}",
        f"--threads={threads}",
        f"--limit={limit}",
        f"--end-usage={end_usage}", "--passes=1",
        "--kf-max-dist=8", "--bit-depth=8", "--ivf",
    ]
    t0 = time.monotonic()
    run(cmd)
    elapsed_ms = (time.monotonic() - t0) * 1000
    return os.path.getsize(output_ivf), elapsed_ms


def decode_to_y4m(ivf_path, y4m_path):
    cmd = [str(AOMDEC), "--codec=av1", "-o", str(y4m_path), str(ivf_path)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0 and not Path(y4m_path).exists():
        raise RuntimeError(f"Decode failed: {result.stderr[:300]}")


def count_y4m_frames(y4m_path):
    """Count frames in a Y4M file using ffprobe."""
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-count_frames", "-select_streams", "v",
         "-show_entries", "stream=nb_read_frames", "-of", "csv=p=0",
         str(y4m_path)],
        capture_output=True, text=True)
    try:
        return int(result.stdout.strip())
    except ValueError:
        return None


def measure_psnr_ssim(ref_y4m, dec_y4m):
    """Measure Y-PSNR and SSIM using ffmpeg lavfi on Y4M inputs."""
    nframes = count_y4m_frames(dec_y4m)
    frame_limit = ["-frames:v", str(nframes)] if nframes else []

    psnr_cmd = [
        "ffmpeg",
        "-i", str(dec_y4m), "-i", str(ref_y4m),
    ] + frame_limit + [
        "-lavfi", "[0][1]psnr", "-f", "null", "-",
    ]
    result = subprocess.run(psnr_cmd, capture_output=True, text=True)
    combined = result.stdout + result.stderr
    psnr_match = re.search(r"PSNR y:([0-9.]+)", combined)
    if not psnr_match:
        psnr_match = re.search(r"average:([0-9.]+)", combined)
    psnr_y = float(psnr_match.group(1)) if psnr_match else None

    ssim_cmd = [
        "ffmpeg",
        "-i", str(dec_y4m), "-i", str(ref_y4m),
    ] + frame_limit + [
        "-lavfi", "[0][1]ssim", "-f", "null", "-",
    ]
    result = subprocess.run(ssim_cmd, capture_output=True, text=True)
    combined = result.stdout + result.stderr
    ssim_match = re.search(r"All:([0-9.]+)", combined)
    ssim_val = float(ssim_match.group(1)) if ssim_match else None

    return psnr_y, ssim_val


def measure_vmaf(ref_y4m, dec_y4m):
    """Measure VMAF using ffmpeg libvmaf filter on Y4M inputs."""
    nframes = count_y4m_frames(dec_y4m)
    frame_limit = ["-frames:v", str(nframes)] if nframes else []
    cmd = [
        "ffmpeg",
        "-i", str(dec_y4m), "-i", str(ref_y4m),
    ] + frame_limit + [
        "-lavfi", "[0][1]libvmaf=model=version=vmaf_v0.6.1:log_fmt=json:log_path=/dev/stderr",
        "-f", "null", "-",
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    combined = result.stdout + result.stderr

    vmaf_match = re.search(r'"VMAF score"\s*:\s*([0-9.]+)', combined)
    if vmaf_match:
        return float(vmaf_match.group(1))

    vmaf_match = re.search(r"VMAF score\s*=\s*([0-9.]+)", combined)
    if vmaf_match:
        return float(vmaf_match.group(1))

    vmaf_match = re.search(r'"mean"\s*:\s*([0-9.]+)', combined)
    if vmaf_match:
        return float(vmaf_match.group(1))

    return None


def bd_rate(rate_a, qual_a, rate_b, qual_b):
    """
    Bjontegaard Delta Rate (BD-rate) per VCEG-M33.
    Negative means test (b) is better.
    """
    if len(rate_a) < 4 or len(rate_b) < 4:
        return None

    log_rate_a = [math.log10(max(r, 1)) for r in rate_a]
    log_rate_b = [math.log10(max(r, 1)) for r in rate_b]

    pairs_a = sorted(zip(qual_a, log_rate_a))
    pairs_b = sorted(zip(qual_b, log_rate_b))
    qa = [p[0] for p in pairs_a]
    lra = [p[1] for p in pairs_a]
    qb = [p[0] for p in pairs_b]
    lrb = [p[1] for p in pairs_b]

    min_q = max(min(qa), min(qb))
    max_q = min(max(qa), max(qb))
    if max_q <= min_q:
        return None

    int_a = _cubic_interp_integral(qa, lra, min_q, max_q)
    int_b = _cubic_interp_integral(qb, lrb, min_q, max_q)
    avg_diff = (int_b - int_a) / (max_q - min_q)
    return (10.0 ** avg_diff - 1.0) * 100.0


def bd_psnr(rate_a, psnr_a, rate_b, psnr_b):
    """Bjontegaard Delta PSNR. Positive means test (b) is better."""
    if len(rate_a) < 4 or len(rate_b) < 4:
        return None

    log_rate_a = [math.log10(max(r, 1)) for r in rate_a]
    log_rate_b = [math.log10(max(r, 1)) for r in rate_b]

    pairs_a = sorted(zip(log_rate_a, psnr_a))
    pairs_b = sorted(zip(log_rate_b, psnr_b))
    lra = [p[0] for p in pairs_a]
    pa = [p[1] for p in pairs_a]
    lrb = [p[0] for p in pairs_b]
    pb = [p[1] for p in pairs_b]

    min_lr = max(min(lra), min(lrb))
    max_lr = min(max(lra), max(lrb))
    if max_lr <= min_lr:
        return None

    int_a = _cubic_interp_integral(lra, pa, min_lr, max_lr)
    int_b = _cubic_interp_integral(lrb, pb, min_lr, max_lr)
    return (int_b - int_a) / (max_lr - min_lr)


def _cubic_interp_integral(x, y, x_min, x_max):
    """Piecewise cubic polynomial fit and integration (VCEG-M33 method)."""
    M = [[xi ** 3, xi ** 2, xi, 1.0] for xi in x]
    aug = [row + [yi] for row, yi in zip(M, y)]
    nn = len(aug)
    for col in range(nn):
        max_row = col
        for row in range(col + 1, nn):
            if abs(aug[row][col]) > abs(aug[max_row][col]):
                max_row = row
        aug[col], aug[max_row] = aug[max_row], aug[col]
        if abs(aug[col][col]) < 1e-12:
            continue
        for row in range(col + 1, nn):
            factor = aug[row][col] / aug[col][col]
            for j in range(col, nn + 1):
                aug[row][j] -= factor * aug[col][j]
    coeffs = [0.0] * nn
    for i in range(nn - 1, -1, -1):
        coeffs[i] = aug[i][nn]
        for j in range(i + 1, nn):
            coeffs[i] -= aug[i][j] * coeffs[j]
        if abs(aug[i][i]) > 1e-12:
            coeffs[i] /= aug[i][i]
    a, b, c, d = coeffs

    def antideriv(t):
        return a * t ** 4 / 4.0 + b * t ** 3 / 3.0 + c * t ** 2 / 2.0 + d * t

    return antideriv(x_max) - antideriv(x_min)


def ssim_to_db(s):
    if s is None:
        return None
    if s >= 1.0:
        return 100.0
    if s <= 0.0:
        return 0.0
    return -10.0 * math.log10(1.0 - s)


def fmt_pct(v):
    return f"{v:+.2f}%" if v is not None else "N/A"


def fmt_db(v):
    return f"{v:+.3f}dB" if v is not None else "N/A"


def safe_avg(lst):
    return sum(lst) / len(lst) if lst else None


def run_cq_analysis(args, seq_list, has_vmaf, out):
    """Run constant-quality analysis across all sequences, speeds, AQ modes."""
    all_results = {}

    for cpu in args.cpu_used:
        out(f"\n{'='*80}")
        out(f"  cpu-used={cpu}  |  threads={args.threads}  |  limit={args.limit}")
        out(f"{'='*80}")

        results = {}
        total = len(seq_list) * len(args.qp_points) * len(AQ_MODES)
        count = 0

        for seq_path in seq_list:
            seq_name = seq_path.stem
            w, h, fps = detect_y4m_resolution(seq_path)
            results[seq_name] = {"meta": (w, h, fps)}

            for aq_mode, aq_name in AQ_MODES.items():
                rd_points = []
                for qp in args.qp_points:
                    count += 1
                    tag = f"{seq_name}_cpu{cpu}_aq{aq_mode}_qp{qp}"
                    ivf = WORK_DIR / f"{tag}.ivf"
                    dec_y4m = WORK_DIR / f"{tag}_dec.y4m"

                    print(f"  [{count}/{total}] {tag}...", end=" ", flush=True)

                    fsize, enc_ms = encode_cq(
                        seq_path, ivf, qp, aq_mode, cpu, args.threads, args.limit)

                    nframes = min(args.limit, 9999)
                    bitrate_kbps = (fsize * 8.0) / (nframes / fps) / 1000.0

                    decode_to_y4m(ivf, dec_y4m)
                    psnr, ssim = measure_psnr_ssim(seq_path, dec_y4m)

                    vmaf = None
                    if has_vmaf:
                        vmaf = measure_vmaf(seq_path, dec_y4m)

                    ms_per_frame = enc_ms / nframes if nframes > 0 else 0
                    if psnr is None:
                        psnr = 0.0
                    if ssim is None:
                        ssim = 0.0
                    print(f"rate={bitrate_kbps:.1f}k PSNR={psnr:.2f} "
                          f"SSIM={ssim:.4f}"
                          f"{f' VMAF={vmaf:.2f}' if vmaf else ''}"
                          f" {ms_per_frame:.1f}ms/f")

                    rd_points.append({
                        "rate": bitrate_kbps, "psnr": psnr,
                        "ssim": ssim, "vmaf": vmaf,
                        "ms_per_frame": ms_per_frame,
                        "file_size": fsize,
                    })

                    os.remove(dec_y4m)
                    os.remove(ivf)

                results[seq_name][aq_mode] = rd_points

        all_results[cpu] = results
        _print_cq_tables(results, args.qp_points, has_vmaf, cpu, args.threads, out)

    return all_results


def _print_cq_tables(results, qp_points, has_vmaf, cpu, threads, out):
    """Print raw RD data and BD-rate tables for one speed preset."""
    out(f"\n{'-'*80}")
    out(f"RAW RD DATA  (cpu-used={cpu}, threads={threads})")
    out(f"{'-'*80}")
    vmaf_col = "  {'VMAF':>7}" if has_vmaf else ""
    for seq_name, data in results.items():
        if "meta" not in data:
            continue
        w, h, fps = data["meta"]
        out(f"\n  Sequence: {seq_name} ({w}x{h} @ {fps:.0f}fps)")
        hdr = f"  {'AQ Mode':<12} {'QP':>4}  {'Rate(kbps)':>12}  {'PSNR(dB)':>10}  {'SSIM':>8}"
        if has_vmaf:
            hdr += f"  {'VMAF':>7}"
        hdr += f"  {'ms/f':>7}"
        out(hdr)
        for aq_mode, aq_name in AQ_MODES.items():
            if aq_mode not in data:
                continue
            for i, qp in enumerate(qp_points):
                pt = data[aq_mode][i]
                label = f"{aq_name}({aq_mode})" if i == 0 else ""
                line = (f"  {label:<12} {qp:>4}  {pt['rate']:>12.2f}  "
                        f"{pt['psnr']:>10.2f}  {pt['ssim']:>8.4f}")
                if has_vmaf and pt["vmaf"] is not None:
                    line += f"  {pt['vmaf']:>7.2f}"
                elif has_vmaf:
                    line += f"  {'N/A':>7}"
                line += f"  {pt['ms_per_frame']:>7.1f}"
                out(line)

    metrics = [("PSNR", lambda pt: pt["psnr"]),
               ("SSIM", lambda pt: ssim_to_db(pt["ssim"]))]
    if has_vmaf:
        metrics.append(("VMAF", lambda pt: pt["vmaf"]))

    for metric_name, get_qual in metrics:
        out(f"\n{'-'*80}")
        out(f"BD-RATE ({metric_name}-based)  cpu-used={cpu}")
        out(f"{'-'*80}")
        out(f"  {'Sequence':<20} {'Edge vs None':>14} {'Edge vs Var':>14} {'Var vs None':>14}")
        out(f"  {'':<20} {'BD-rate(%)':>14} {'BD-rate(%)':>14} {'BD-rate(%)':>14}")
        out(f"  {'-'*20} {'-'*14} {'-'*14} {'-'*14}")

        sums = {"en": [], "ev": [], "vn": []}
        for seq_name, data in results.items():
            if 0 not in data or 4 not in data:
                continue
            rates_n = [pt["rate"] for pt in data[0]]
            qual_n = [get_qual(pt) for pt in data[0]]
            rates_v = [pt["rate"] for pt in data[1]] if 1 in data else None
            qual_v = [get_qual(pt) for pt in data[1]] if 1 in data else None
            rates_e = [pt["rate"] for pt in data[4]]
            qual_e = [get_qual(pt) for pt in data[4]]

            if any(q is None for q in qual_n + qual_e):
                out(f"  {seq_name:<20} {'N/A':>14} {'N/A':>14} {'N/A':>14}")
                continue

            bdr_en = bd_rate(rates_n, qual_n, rates_e, qual_e)
            bdr_ev = bd_rate(rates_v, qual_v, rates_e, qual_e) if rates_v and all(
                q is not None for q in qual_v) else None
            bdr_vn = bd_rate(rates_n, qual_n, rates_v, qual_v) if rates_v and all(
                q is not None for q in qual_v) else None

            out(f"  {seq_name:<20} {fmt_pct(bdr_en):>14} {fmt_pct(bdr_ev):>14} {fmt_pct(bdr_vn):>14}")
            if bdr_en is not None: sums["en"].append(bdr_en)
            if bdr_ev is not None: sums["ev"].append(bdr_ev)
            if bdr_vn is not None: sums["vn"].append(bdr_vn)

        out(f"  {'-'*20} {'-'*14} {'-'*14} {'-'*14}")
        out(f"  {'AVERAGE':<20} {fmt_pct(safe_avg(sums['en'])):>14} "
            f"{fmt_pct(safe_avg(sums['ev'])):>14} {fmt_pct(safe_avg(sums['vn'])):>14}")

    out(f"\n{'-'*80}")
    out(f"BD-PSNR  cpu-used={cpu}")
    out(f"{'-'*80}")
    out(f"  {'Sequence':<20} {'Edge vs None':>14} {'Edge vs Var':>14} {'Var vs None':>14}")
    out(f"  {'':<20} {'BD-PSNR(dB)':>14} {'BD-PSNR(dB)':>14} {'BD-PSNR(dB)':>14}")
    out(f"  {'-'*20} {'-'*14} {'-'*14} {'-'*14}")

    sums = {"en": [], "ev": [], "vn": []}
    for seq_name, data in results.items():
        if 0 not in data or 4 not in data:
            continue
        rates_n = [pt["rate"] for pt in data[0]]
        psnr_n = [pt["psnr"] for pt in data[0]]
        rates_v = [pt["rate"] for pt in data[1]] if 1 in data else None
        psnr_v = [pt["psnr"] for pt in data[1]] if 1 in data else None
        rates_e = [pt["rate"] for pt in data[4]]
        psnr_e = [pt["psnr"] for pt in data[4]]

        bdp_en = bd_psnr(rates_n, psnr_n, rates_e, psnr_e)
        bdp_ev = bd_psnr(rates_v, psnr_v, rates_e, psnr_e) if rates_v else None
        bdp_vn = bd_psnr(rates_n, psnr_n, rates_v, psnr_v) if rates_v else None

        out(f"  {seq_name:<20} {fmt_db(bdp_en):>14} {fmt_db(bdp_ev):>14} {fmt_db(bdp_vn):>14}")
        if bdp_en is not None: sums["en"].append(bdp_en)
        if bdp_ev is not None: sums["ev"].append(bdp_ev)
        if bdp_vn is not None: sums["vn"].append(bdp_vn)

    out(f"  {'-'*20} {'-'*14} {'-'*14} {'-'*14}")
    out(f"  {'AVERAGE':<20} {fmt_db(safe_avg(sums['en'])):>14} "
        f"{fmt_db(safe_avg(sums['ev'])):>14} {fmt_db(safe_avg(sums['vn'])):>14}")

    out(f"\n{'-'*80}")
    out(f"ENCODE SPEED  cpu-used={cpu}")
    out(f"{'-'*80}")
    out(f"  {'Sequence':<20} {'none(0)':>10} {'var(1)':>10} {'edge(4)':>10}")
    out(f"  {'':<20} {'ms/f':>10} {'ms/f':>10} {'ms/f':>10}")
    out(f"  {'-'*20} {'-'*10} {'-'*10} {'-'*10}")
    for seq_name, data in results.items():
        if 0 not in data or 4 not in data:
            continue
        avg_n = safe_avg([pt["ms_per_frame"] for pt in data[0]])
        avg_v = safe_avg([pt["ms_per_frame"] for pt in data[1]]) if 1 in data else None
        avg_e = safe_avg([pt["ms_per_frame"] for pt in data[4]])
        v_str = f"{avg_v:.1f}" if avg_v is not None else "N/A"
        out(f"  {seq_name:<20} {avg_n:>10.1f} {v_str:>10} {avg_e:>10.1f}")


def run_rate_controlled(args, seq_list, has_vmaf, mode, out):
    """Run VBR or CBR analysis."""
    out(f"\n{'='*80}")
    out(f"  RATE-CONTROLLED ANALYSIS: {mode.upper()}")
    out(f"{'='*80}")

    cpu = args.cpu_used[0] if args.cpu_used else 6

    for seq_path in seq_list:
        seq_name = seq_path.stem
        w, h, fps = detect_y4m_resolution(seq_path)
        out(f"\n  Sequence: {seq_name} ({w}x{h} @ {fps:.0f}fps)  cpu-used={cpu}")
        hdr = f"  {'AQ Mode':<12} {'Target(k)':>10}  {'Actual(k)':>10}  {'Ratio':>7}  {'PSNR':>8}  {'SSIM':>8}"
        if has_vmaf:
            hdr += f"  {'VMAF':>7}"
        out(hdr)

        for aq_mode, aq_name in AQ_MODES.items():
            for i, target in enumerate(args.vbr_targets):
                tag = f"{seq_name}_{mode}_cpu{cpu}_aq{aq_mode}_t{target}"
                ivf = WORK_DIR / f"{tag}.ivf"
                dec_y4m = WORK_DIR / f"{tag}_dec.y4m"

                print(f"  {mode.upper()} {tag}...", end=" ", flush=True)

                try:
                    fsize, enc_ms = encode_rate_controlled(
                        seq_path, ivf, target, aq_mode, cpu, args.threads,
                        args.limit, mode)
                except RuntimeError:
                    print("FAILED")
                    continue

                nframes = min(args.limit, 9999)
                actual_kbps = (fsize * 8.0) / (nframes / fps) / 1000.0
                ratio = actual_kbps / target if target > 0 else 0

                decode_to_y4m(ivf, dec_y4m)
                psnr, ssim = measure_psnr_ssim(seq_path, dec_y4m)
                vmaf = measure_vmaf(seq_path, dec_y4m) if has_vmaf else None

                print(f"actual={actual_kbps:.1f}k ratio={ratio:.3f} "
                      f"PSNR={psnr:.2f}")

                label = f"{aq_name}({aq_mode})" if i == 0 else ""
                line = (f"  {label:<12} {target:>10}  {actual_kbps:>10.1f}  "
                        f"{ratio:>7.3f}  {psnr:>8.2f}  {ssim:>8.4f}")
                if has_vmaf and vmaf is not None:
                    line += f"  {vmaf:>7.2f}"
                elif has_vmaf:
                    line += f"  {'N/A':>7}"
                out(line)

                os.remove(dec_y4m)
                os.remove(ivf)


def main():
    args = parse_args()

    WORK_DIR.mkdir(exist_ok=True)

    seq_dir = args.sequences_dir
    if not seq_dir.exists():
        print(f"ERROR: Sequences directory not found: {seq_dir}")
        print(f"Run: bash download_ctc_sequences.sh --dir {seq_dir}")
        sys.exit(1)

    if args.sequences:
        seq_list = [seq_dir / f"{s}.y4m" for s in args.sequences]
    else:
        seq_list = sorted(seq_dir.glob("*.y4m"))

    seq_list = [s for s in seq_list if s.exists()]
    if not seq_list:
        print(f"ERROR: No .y4m sequences found in {seq_dir}")
        sys.exit(1)

    has_vmaf = not args.no_vmaf and check_vmaf_available()
    if not args.no_vmaf and not has_vmaf:
        print("NOTE: VMAF not available (ffmpeg lacks libvmaf). Skipping VMAF.")

    output_lines = []

    def out(line=""):
        print(line)
        output_lines.append(line)

    out("=" * 80)
    out("BD-RATE ANALYSIS: EDGE-AQ (--aq-mode=4) for libaom/AV1")
    out("=" * 80)
    out()
    out("Configuration:")
    out(f"  Encoder:     aomenc (custom libaom with EDGE-AQ)")
    out(f"  Sequences:   {seq_dir} ({len(seq_list)} files)")
    out(f"  QP points:   {args.qp_points}")
    out(f"  cpu-used:    {args.cpu_used}")
    out(f"  Threads:     {args.threads}")
    out(f"  Frame limit: {args.limit}")
    out(f"  Rate modes:  {args.rate_modes}")
    out(f"  VMAF:        {'enabled' if has_vmaf else 'disabled'}")
    out(f"  Sequences:   {[s.stem for s in seq_list]}")
    out()

    if "cq" in args.rate_modes:
        run_cq_analysis(args, seq_list, has_vmaf, out)

    for mode in args.rate_modes:
        if mode in ("vbr", "cbr"):
            run_rate_controlled(args, seq_list, has_vmaf, mode, out)

    out()
    out("=" * 80)
    out("INTERPRETATION:")
    out("  BD-rate: negative % = test uses fewer bits at same quality (BETTER)")
    out("  BD-rate: positive % = test uses more bits at same quality (WORSE)")
    out("  BD-PSNR: positive dB = test has higher quality at same bitrate (BETTER)")
    out("  Anchor is always the first mode (e.g., 'None' in 'Edge vs None')")
    out("=" * 80)

    results_file = args.output or (BASE_DIR / f"bdrate_results.txt")
    with open(results_file, "w") as f:
        f.write("\n".join(output_lines) + "\n")
    print(f"\nResults saved to {results_file}")


if __name__ == "__main__":
    main()
