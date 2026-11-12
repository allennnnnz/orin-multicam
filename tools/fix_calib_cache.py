#!/usr/bin/env python3
"""Override INT8 ranges in a TensorRT calibration cache.

The entropy calibrator chooses each tensor's clipping threshold by minimising
KL divergence of its histogram. ResNet10's coverage map (sigmoid output) is
almost all zeros, so the "optimal" threshold came out at 0.36: every
confidence above 0.36 saturates and a 0.4 detection threshold never fires.
On the GPU this tensor stays FP32, on DLA the whole graph is INT8, so only
the DLA engine lost all detections.

A sigmoid is bounded to [0, 1] analytically, so set its range to 1.0.

usage: fix_calib_cache.py IN OUT tensor=max [tensor=max ...]
"""
import struct
import sys


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    src, dst, overrides = sys.argv[1], sys.argv[2], dict(
        (kv.split("=")[0], float(kv.split("=")[1])) for kv in sys.argv[3:])
    lines = open(src).read().splitlines()
    out = [lines[0]]
    for line in lines[1:]:
        if ":" not in line:
            continue
        name, hexval = (s.strip() for s in line.split(":"))
        if name in overrides:
            old = struct.unpack(">f", bytes.fromhex(hexval))[0] * 127
            hexval = struct.pack(">f", overrides[name] / 127.0).hex()
            print(f"{name}: range {old:.3f} -> {overrides[name]:.3f}")
        out.append(f"{name}: {hexval}")
    open(dst, "w").write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
