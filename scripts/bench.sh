#!/usr/bin/env bash
# Benchmark matrix for orin-multicam. Each run writes:
#   results/bench/<name>.json   pipeline latency summary (from omc)
#   results/bench/<name>.csv    per-frame stage timestamps
#   results/bench/<name>.ts.*   tegrastats log + averaged utilisation/power
#
# Requires passwordless sudo for tegrastats, jetson_clocks and --rt runs.
# jetson_clocks state is stored before and restored after the DVFS run.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=results/bench
FRAMES=${FRAMES:-450}
mkdir -p "$OUT"
OMC=build/omc

run() {   # run NAME [sudo] -- omc args...
    local name=$1; shift
    local pre=""
    if [[ $1 == sudo ]]; then pre="sudo -n"; shift; fi
    [[ $1 == -- ]] && shift
    echo "=== $name: $*"
    sudo -n tegrastats --interval 200 --logfile "$OUT/$name.ts.log" &
    sleep 0.3
    $pre $OMC --frames "$FRAMES" --json "$OUT/$name.json" --csv "$OUT/$name.csv" "$@" \
        2> >(grep -E "^cam|^stream|ERROR" >&2) || echo "!!! $name failed"
    sudo -n tegrastats --stop || true
    wait 2>/dev/null || true
    sudo -n chown "$(id -u):$(id -g)" "$OUT/$name".* 2>/dev/null || true
    python3 scripts/tegrastats_summary.py "$OUT/$name.ts.log" --json "$OUT/$name.ts.json" >/dev/null
}

# 0. Optimisation journey (4 cams GPU/DLA0/DLA1/GPU @30 fps, no mosaic)
SAMPLE=/usr/src/jetson_multimedia_api/data/Video/sample_outdoor_car_1080p_10fps.h264
OLD="--gpu-engine models/resnet10_gpu_int8.engine --dla-engine models/resnet10_dla_int8.engine"
run j0_baseline    -- --video "$SAMPLE" --chunked-input $OLD
run j1_ipp_input   -- --chunked-input $OLD
run j2_full_au     -- $OLD
run j3_recalib     --

# 1. Workload placement at 4 x 30 fps, full pipeline incl. mosaic + NVENC
run place_all_gpu  -- --engines gpu,gpu,gpu,gpu       --output "$OUT/mosaic_all_gpu.h264"
run place_hetero   -- --engines gpu,dla0,dla1,gpu     --output "$OUT/mosaic_hetero.h264"
run place_all_dla  -- --engines dla0,dla1,dla0,dla1   --output "$OUT/mosaic_all_dla.h264"

# 2. Capacity: free-running decode (no pacing, no mosaic) -> max frames/s
run cap_all_gpu    -- --engines gpu,gpu,gpu,gpu     --fps 0
run cap_hetero     -- --engines gpu,dla0,dla1,gpu   --fps 0
run cap_all_dla    -- --engines dla0,dla1,dla0,dla1 --fps 0

# 3. DVFS: default governors vs clocks locked at max for this nvpmodel
sudo -n rm -f "$OUT/jetson_clocks.conf"   # --store refuses to overwrite
sudo -n jetson_clocks --store "$OUT/jetson_clocks.conf"
sudo -n jetson_clocks
run dvfs_locked    -- --engines gpu,dla0,dla1,gpu --output "$OUT/mosaic_locked.h264"
sudo -n jetson_clocks --restore "$OUT/jetson_clocks.conf"

# 4. Contention: memcpy storm on every CPU core (DRAM bandwidth + CPU time)
hog() { build/membw_hog "$(nproc)" $((FRAMES / 30 + 3)) 64 > "$OUT/$1.hog.txt" & }
hog load_default;  run load_default      -- --engines gpu,dla0,dla1,gpu --output "$OUT/mosaic_load.h264"
wait
hog load_rt_pin;   run load_rt_pin sudo  -- --engines gpu,dla0,dla1,gpu --output "$OUT/mosaic_load_rt.h264" --rt --pin
wait

python3 scripts/report.py "$OUT"
