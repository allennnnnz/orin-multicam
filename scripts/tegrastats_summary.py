#!/usr/bin/env python3
"""Average engine utilisation / clocks / power from a tegrastats log.

usage: tegrastats_summary.py LOG [--json OUT]
"""
import json
import re
import sys

PATTERNS = {
    "gpu_util_pct": r"GR3D_FREQ (\d+)%",
    "gpu_mhz": r"GR3D_FREQ \d+%@\[(\d+)",
    "vic_util_pct": r"VIC_FREQ (\d+)%",
    "vic_mhz": r"VIC_FREQ \d+%@(\d+)",
    "emc_util_pct": r"EMC_FREQ (\d+)%",
    "emc_mhz": r"EMC_FREQ \d+%@(\d+)",
    "nvdec_mhz": r"NVDEC (\d+)",
    "nvenc_mhz": r"NVENC (\d+)",
    "p_gpu_soc_mw": r"VDD_GPU_SOC (\d+)mW",
    "p_cpu_cv_mw": r"VDD_CPU_CV (\d+)mW",
    "p_sys5v_mw": r"VIN_SYS_5V0 (\d+)mW",
}


def summarize(path):
    vals = {k: [] for k in PATTERNS}
    cpu = []
    for line in open(path):
        for k, p in PATTERNS.items():
            m = re.search(p, line)
            if m:
                vals[k].append(float(m.group(1)))
        m = re.search(r"CPU \[([^\]]+)\]", line)
        if m:
            loads = [int(c.split("%")[0]) for c in m.group(1).split(",") if "%" in c]
            if loads:
                cpu.append(sum(loads) / len(loads))
    out = {k: round(sum(v) / len(v), 1) for k, v in vals.items() if v}
    if cpu:
        out["cpu_util_pct"] = round(sum(cpu) / len(cpu), 1)
    out["samples"] = len(cpu)
    return out


if __name__ == "__main__":
    s = summarize(sys.argv[1])
    if "--json" in sys.argv:
        json.dump(s, open(sys.argv[sys.argv.index("--json") + 1], "w"), indent=1)
    print(json.dumps(s))
