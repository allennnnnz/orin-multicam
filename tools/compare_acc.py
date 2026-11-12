#!/usr/bin/env python3
"""Per-frame agreement of detection counts between the GPU camera and others
(run with --same-scene so every camera sees identical frames)."""
import csv
import sys

for path in sys.argv[1:]:
    by = {}
    for r in csv.DictReader(open(path)):
        by.setdefault(r["stream"], []).append(int(r["ndet"]))
    ref_name = next(k for k in by if k.endswith("/gpu"))
    ref = by[ref_name]
    for k, v in by.items():
        if k == ref_name:
            continue
        same = sum(a == b for a, b in zip(ref, v)) / len(ref)
        mad = sum(abs(a - b) for a, b in zip(ref, v)) / len(ref)
        print(f"{path}: {k} vs {ref_name}: same count {100*same:.1f}% of frames, "
              f"mean |diff| {mad:.2f}, totals {sum(v)} vs {sum(ref)}")
