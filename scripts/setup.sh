#!/usr/bin/env bash
# One-time setup: build binaries, create the camera-like input, calibrate and
# build TensorRT engines. Everything comes from JetPack itself (no downloads).
set -euo pipefail
cd "$(dirname "$0")/.."

MMAPI=/usr/src/jetson_multimedia_api
SAMPLE=$MMAPI/data/Video/sample_outdoor_car_1080p_10fps.h264
TRTEXEC=/usr/src/tensorrt/bin/trtexec
mkdir -p models data/calib build

echo "== build"
make -j"$(nproc)"

echo "== model (ResNet10 detector shipped with the Multimedia API)"
cp -n $MMAPI/data/Model/resnet10/resnet10_dynamic_batch.onnx $MMAPI/data/Model/resnet10/labels.txt models/

echo "== camera-like input: re-encode the sample as IPPP (no B-frames, POC type 2, IDR every 30)"
[[ -f data/camera_ipp_1080p.h264 ]] || gst-launch-1.0 -q filesrc location=$SAMPLE ! h264parse ! nvv4l2decoder ! \
    nvv4l2h264enc profile=0 num-B-Frames=0 poc-type=2 iframeinterval=30 idrinterval=30 \
    insert-sps-pps=true bitrate=6000000 ! 'video/x-h264,stream-format=byte-stream' ! \
    filesink location=data/camera_ipp_1080p.h264

echo "== calibration frames: every 5th frame as 640x368 RGBA"
[[ -f data/calib/frames_640x368_rgba.raw ]] || gst-launch-1.0 -q filesrc location=$SAMPLE ! h264parse ! \
    nvv4l2decoder ! nvvidconv ! 'video/x-raw,format=RGBA,width=640,height=368' ! fdsink fd=1 | \
    python3 -c "
import sys
fs = 640 * 368 * 4; i = 0
out = open('data/calib/frames_640x368_rgba.raw', 'wb')
while True:
    b = sys.stdin.buffer.read(fs)
    if len(b) < fs: break
    if i % 5 == 0: out.write(b)
    i += 1"

echo "== engines: baseline (TRT7-era calibration table from the sample, header patched for TRT 8.5)"
sed '1s/.*/TRT-8502-EntropyCalibration2/' $MMAPI/data/Model/resnet10/CalibrationTable_ONNX > models/calib_trt85.cache
S="--minShapes=data:1x3x368x640 --optShapes=data:1x3x368x640 --maxShapes=data:1x3x368x640"
$TRTEXEC --onnx=models/resnet10_dynamic_batch.onnx $S --int8 --fp16 --calib=models/calib_trt85.cache \
    --saveEngine=models/resnet10_gpu_int8.engine > /dev/null
$TRTEXEC --onnx=models/resnet10_dynamic_batch.onnx $S --int8 --fp16 --calib=models/calib_trt85.cache \
    --useDLACore=0 --allowGPUFallback --saveEngine=models/resnet10_dla_int8.engine > /dev/null

echo "== engines: calibrated on scene frames, sigmoid output range fixed"
rm -f models/calib_frames.cache
build/build_engine --onnx models/resnet10_dynamic_batch.onnx --out models/resnet10_gpu_int8_cal.engine \
    --int8 data/calib/frames_640x368_rgba.raw --cache models/calib_frames.cache
python3 tools/fix_calib_cache.py models/calib_frames.cache models/calib_fixed.cache Layer7_cov=1.0
for t in gpu dla; do
    extra=""; [[ $t == dla ]] && extra="--dla 0"
    build/build_engine --onnx models/resnet10_dynamic_batch.onnx --out models/resnet10_${t}_int8_fix.engine \
        --int8 data/calib/frames_640x368_rgba.raw --cache models/calib_fixed.cache $extra
done
echo "setup done"
