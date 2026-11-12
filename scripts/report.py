#!/usr/bin/env python3
"""Turn results/bench/* into summary.md and PNG charts.

usage: report.py results/bench
"""
import csv
import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

D = sys.argv[1] if len(sys.argv) > 1 else "results/bench"
STAGES = ["decode", "vic", "infer", "post"]
COLORS = {"decode": "#4C78A8", "vic": "#F58518", "infer": "#54A24B", "post": "#B279A2"}


def load(name):
    p = os.path.join(D, name + ".json")
    if not os.path.exists(p):
        return None
    r = json.load(open(p))
    tsp = os.path.join(D, name + ".ts.json")
    r["ts"] = json.load(open(tsp)) if os.path.exists(tsp) else {}
    return r


def worst(r, key, stat):
    return max(s[key][stat] for s in r["streams"])


def mean_of(r, key, stat):
    return sum(s[key][stat] for s in r["streams"]) / len(r["streams"])


def row(name, label, r, extra=""):
    ts = r["ts"]
    out = r["config"]["output"]
    e2e = "output_e2e" if out else "detect_e2e"
    fps = sum(s["fps"] for s in r["streams"])
    return (f"| {label} | {fps:.0f} | {mean_of(r, 'infer', 'p50'):.2f} | "
            f"{mean_of(r, e2e, 'p50'):.1f} | {worst(r, e2e, 'p99'):.1f} | {worst(r, e2e, 'max'):.1f} | "
            f"{ts.get('gpu_util_pct', 0):.0f}% @ {ts.get('gpu_mhz', 0):.0f} | "
            f"{ts.get('p_gpu_soc_mw', 0) / 1000:.1f} / {ts.get('p_sys5v_mw', 0) / 1000:.1f} |{extra}")


HDR = ("| run | total fps | infer p50 (ms) | e2e p50 (ms) | e2e p99 worst cam (ms) | "
       "e2e max (ms) | GPU util @ MHz | GPU+SOC / SYS5V (W) |\n|---|---|---|---|---|---|---|---|")

SECTIONS = [
    ("Optimisation journey (4 cams, GPU/DLA0/DLA1/GPU, 30 fps, e2e = camera feed -> detections)", [
        ("j0_baseline", "0. MMAPI sample bitstream (High profile, B-frames), byte-stream feed, TRT7 calib table"),
        ("j1_ipp_input", "1. camera-like IPPP input (no B-frames)"),
        ("j2_full_au", "2. one access unit per V4L2 buffer"),
        ("j3_recalib", "3. INT8 recalibrated on scene + sigmoid range fix"),
    ]),
    ("Workload placement (full pipeline incl. 2x2 mosaic + NVENC, e2e = camera feed -> encoder)", [
        ("place_all_gpu", "all 4 cameras on GPU"),
        ("place_hetero", "GPU, DLA0, DLA1, GPU"),
        ("place_all_dla", "DLA0, DLA1, DLA0, DLA1"),
    ]),
    ("Capacity (free-running, no mosaic; fps = total frames/s across 4 cameras)", [
        ("cap_all_gpu", "all GPU"), ("cap_hetero", "GPU+DLA"), ("cap_all_dla", "all DLA"),
    ]),
    ("DVFS and shared-DRAM contention (GPU/DLA0/DLA1/GPU + mosaic)", [
        ("place_hetero", "default governors"),
        ("dvfs_locked", "jetson_clocks (clocks locked at max)"),
        ("load_default", "+ memcpy storm on all CPU cores"),
        ("load_rt_pin", "+ memcpy storm, SCHED_FIFO + pinned threads"),
    ]),
]


def stage_chart():
    runs = [("j0_baseline", "baseline"), ("j1_ipp_input", "IPPP input"),
            ("j2_full_au", "1 AU/buffer"), ("j3_recalib", "recalib INT8")]
    runs = [(n, l, load(n)) for n, l in runs if load(n)]
    if not runs:
        return None
    cams = [s["name"] for s in runs[0][2]["streams"]]
    fig, axes = plt.subplots(1, len(cams), figsize=(4 * len(cams), 4), sharey=True)
    for ax, cam in zip(axes, cams):
        bottom = [0.0] * len(runs)
        for st in STAGES:
            vals = [next(s for s in r["streams"] if s["name"] == cam)[st]["p50"] for _, _, r in runs]
            ax.bar([l for _, l, _ in runs], vals, bottom=bottom, label=st, color=COLORS[st])
            bottom = [b + v for b, v in zip(bottom, vals)]
        ax.set_title(cam)
        ax.tick_params(axis="x", rotation=30)
    axes[0].set_ylabel("median latency per stage (ms)")
    axes[-1].legend()
    fig.suptitle("Where the latency went: camera feed -> detections (p50)")
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    path = os.path.join(D, "chart_journey.png")
    fig.savefig(path, dpi=110)
    return path


def cdf_chart():
    runs = [("place_hetero", "default"), ("dvfs_locked", "jetson_clocks"),
            ("load_default", "DRAM load"), ("load_rt_pin", "DRAM load + RT/pin")]
    fig, ax = plt.subplots(figsize=(7, 4))
    for name, label in runs:
        p = os.path.join(D, name + ".csv")
        if not os.path.exists(p):
            continue
        v = sorted(float(r["output_e2e_ms"]) for i, r in enumerate(csv.DictReader(open(p)))
                   if float(r["output_e2e_ms"]) > 0 and int(r["frame"]) >= 20)
        ax.plot(v, [i / len(v) for i in range(len(v))], label=label)
    ax.set_xlabel("camera feed -> NVENC latency (ms)")
    ax.set_ylabel("fraction of frames")
    ax.grid(alpha=0.3)
    ax.legend()
    ax.set_title("Latency distribution, 4 cameras @ 30 fps")
    fig.tight_layout()
    path = os.path.join(D, "chart_latency_cdf.png")
    fig.savefig(path, dpi=110)
    return path


def main():
    md = ["# Benchmark results", "",
          "Jetson AGX Orin 64GB, JetPack 5.1.2 (L4T R35.4.1), nvpmodel MODE_30W, "
          "ResNet10 detector 640x368, 4 x 1080p H.264 inputs.", ""]
    for title, runs in SECTIONS:
        md += [f"## {title}", "", HDR]
        for name, label in runs:
            r = load(name)
            if r:
                md.append(row(name, label, r))
        md.append("")
    for p in (stage_chart(), cdf_chart()):
        if p:
            md += [f"![{os.path.basename(p)}]({os.path.basename(p)})", ""]
    open(os.path.join(D, "summary.md"), "w").write("\n".join(md))
    print("\n".join(md))


if __name__ == "__main__":
    main()
