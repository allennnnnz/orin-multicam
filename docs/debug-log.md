# Bring-up & debug log

Real issues hit while bringing this pipeline up on Jetson AGX Orin (JetPack 5.1.2),
written as *symptom → hypothesis → evidence → root cause → fix → result*.

---

## 1. `v4l2_mmap()` on the decoder fails with `ENODEV`

**Symptom.** Mapping NVDEC's MMAP OUTPUT-plane buffers failed:
`mmap output buf len=872448 off=0: No such device`.

**Hypothesis.** The buffer is valid (QUERYBUF succeeded, sane length/offset), so
the problem is *how* it is mapped, not the buffer itself.

**Evidence.** In the MMAPI C++ classes, `NvV4l2ElementPlane::setupPlane()` calls
`VIDIOC_EXPBUF` for every plane and `NvBuffer::map()` then calls plain `mmap()`
on the *exported dmabuf fd*, never on the device fd.

**Root cause.** `libnvv4l2` is not a real V4L2 kernel driver; it is a user-space
shim over NvMM. It does not implement `v4l2_mmap()` on the device fd.

**Fix.** `map_mmap_plane()` in [`src/v4l2_util.h`](../src/v4l2_util.h): `EXPBUF` → `mmap(dmabuf_fd)`.

---

## 2. `VIDIOC_DQBUF` returns `EAGAIN` on a *blocking* fd

**Symptom.** Right after start-up, both `DQBUF(OUTPUT)` and `DQBUF(CAPTURE)`
returned `EAGAIN` although the device was opened without `O_NONBLOCK`.

**Root cause.** NVDEC cannot consume bitstream until the CAPTURE plane exists
(which happens only after `V4L2_EVENT_RESOLUTION_CHANGE`), and libnvv4l2's
internal wait times out with `EAGAIN` instead of sleeping forever. The MMAPI
samples silently retry.

**Fix.** `dqbuf_retry()`: retry on `EAGAIN/EINTR`, treat `EPIPE` or
`V4L2_BUF_FLAG_LAST` as end-of-stream, and fail after a deadline instead of
hanging - a hung pipeline is harder to debug than an explicit timeout.

---

## 3. 134 ms spent "in the decoder" for a 3 ms decode

**Symptom.** At 4 × 30 fps the *feed → decoded* stage had a median of
**134 ms** (≈ 4 frame periods), while NVDEC throughput was > 300 fps.

**Hypothesis.** Throughput is fine, latency is not ⇒ frames are being *held*,
not processed slowly. In H.264 the decoder holds frames for display reordering.

**Evidence.** Parsing the slice headers of the sample bitstream:
`profile_idc=100 (High)`, slice types `{I: 3, P: 168, B: 420}`. Without
`max_num_reorder_frames` in the VUI the decoder must assume the worst-case DPB
delay before emitting a frame.

**Root cause.** Test input is a *movie-style* stream; automotive cameras emit
low-latency IPPP streams (or raw frames).

**Fix.** Make the input look like a camera: re-encode with NVENC as Baseline,
no B-frames, `poc-type=2` (output order = decode order), IDR every 30 frames
(`scripts/setup.sh`). Disabling the DPB instead would have corrupted the B-frame stream.

**Result.** Decode stage 134 ms → 41 ms.

---

## 4. Still one frame (≈ 33 ms) stuck in the decoder

**Symptom.** After fix 3 the decode stage was still ~40 ms ≈ 1.2 frame periods.

**Hypothesis.** "Exactly one frame late" smells like an end-of-frame detection
problem: the parser can only know a picture is complete when it sees the *next*
start code.

**Evidence.** The decoder was configured with
`V4L2_CID_MPEG_VIDEO_DISABLE_COMPLETE_FRAME_INPUT = 1` (byte-stream mode, the
MMAPI sample's default). The app's own indexer (`src/h264_index.c`) already
splits the stream into complete access units.

**Fix.** Feed exactly one access unit per buffer and set the control to `0`.
Kept as `--chunked-input` for A/B.

**Result.** Decode stage 40 ms → **6.3 ms**; GPU camera feed→detections
50 ms → **16.8 ms** (p50).

---

## 5. DLA inference 26.7 ms vs 1.3 ms on GPU

**Symptom.** The DLA engine was 20× slower than the GPU engine in isolation
(`trtexec`), and DLA cameras detected noticeably fewer people.

**Evidence.** The TensorRT build log:
`Missing scale and zero-point for tensor Layer3_block_0_Y ... fall back to non-int8`
for 13 tensors. The calibration table shipped with the sample is from
**TensorRT 7** and the **Caffe** graph: it has scales for `*_relu_Y` but not
for the conv outputs that exist as separate tensors in the ONNX graph.

**Root cause.** The DLA ran a patchwork of INT8 and FP16 layers with
re-formatting between them.

**Fix.** Recalibrate (`tools/build_engine.cpp`, `IInt8EntropyCalibrator2`) on
119 frames of the target scene, preprocessed exactly like the runtime kernel.

**Result.** DLA: **26.7 ms → 1.0 ms** in `trtexec`. Engine size 2.0 MB → 0.9 MB.

---

## 6. ...and then the fast DLA engine detected *nothing*

**Symptom.** After fix 5 the DLA cameras reported 0 detections, the GPU engine
built from the same calibration cache was fine. Too fast to be true.

**Evidence.** Same input frame through both engines (`trtexec --loadInputs`):

| engine | `Layer7_cov` max | cells > 0.4 |
|---|---|---|
| GPU, recalibrated | 0.924 | 24 |
| DLA, recalibrated | **0.360** | **0** |
| DLA, old table | 0.923 | 21 |

Decoding the cache: `Layer7_cov scale=0.002832 → clip = 0.360`.

**Root cause.** The coverage map is a sigmoid output that is ~0 almost
everywhere. The entropy calibrator minimises KL divergence over that histogram
and chooses a clipping range of 0.36, so every confidence above 0.36
saturates. On GPU that output stays in FP32; on DLA the whole graph is INT8.

**Fix.** A sigmoid is bounded to [0, 1] analytically, so do not let the
calibrator guess: `tools/fix_calib_cache.py ... Layer7_cov=1.0`.

**Result.** With all cameras seeing identical frames (`--same-scene`):

| DLA engine | infer (ms) | frames with same #detections as GPU |
|---|---|---|
| TRT7 table (mixed INT8/FP16) | 19.4 | 78.0 % |
| recalibrated | 1.9 | 0.0 % |
| recalibrated + range fix | **1.9** | **86.7 %** |

Lesson: always validate accuracy *on the target accelerator*, not just on GPU.

---

## 7. Boxes randomly missing from the encoded mosaic (CUDA → VIC coherence)

**Symptom.** Detections were correct (1 of 1800 frames had none), but in the
encoded mosaic about **35 % of frames had no boxes** on one or more tiles.
It appeared right after a refactor that moved the mosaic work *after*
inference, to stop it from inflating the measured inference time.

**Measurement.** `tools/count_boxes.py` decodes the output and counts
box-coloured pixels per frame, which turns "looks flaky" into a number.

**Hypotheses, tested one at a time (300 frames each):**

| variant | frames missing boxes |
|---|---|
| before refactor (VIC tile written ~5 ms before CUDA draws) | 0 % |
| after refactor (VIC tile, then CUDA draw immediately) | 34–37 % |
| H1: VIC not finished → `NvBufSurfTransformAsync` + wait on its fence | 34 % ✗ |
| H2: memory ordering → `__threadfence_system()` in the draw kernel | 36 % ✗ |
| H3: GPU writes not published → register/unregister EGLImage per frame | **0 %** ✓ |

Cross-check for the opposite direction (VIC writes, CUDA reads the inference
input through a long-lived registration): two GPU cameras fed identical frames
produced identical detection counts on 100 % of frames, so that path is fine.

**Root cause.** The mosaic tile was registered with CUDA once at start-up and
reused. `cudaStreamSynchronize()` guarantees the kernel finished, but not that
its writes through that EGL mapping are visible to *other hardware engines*.
Unregistering the resource is the synchronisation point (it is what the MMAPI
samples do every frame). The old ordering only worked by timing luck.

**Fix.** `cuda_register()` / `cuda_unregister()` around each overlay in
[`src/cuda_map.c`](../src/cuda_map.c). EGLImage mapping stays long-lived; only
the CUDA registration is per frame. Cost: +1.4 ms on the output path, nothing on
the detection path.

---

## 8. Clocks and contention (not bugs, but customer questions)

* **DVFS.** With default governors the GPU idled at its minimum 306 MHz because
  each frame is short, bursty work, so every inference pays for the clock-up time.
  `jetson_clocks` (all engines at max for the nvpmodel) cut GPU inference p50 from
  2.6 ms to 1.6 ms and pipeline p99 from 31.2 ms to 27.9 ms, at +0.3 W GPU+SOC.
* **Shared DRAM + CPU load.** A memcpy storm on every CPU core (~51 GB/s) left the
  median unchanged (26.5 ms) but pushed the worst frame to 71.5 ms and p99 to 35.6 ms.
  SCHED_FIFO + per-camera core pinning restored max 35.7 ms / p99 31.5 ms.
  In other words, the tail is a CPU scheduling problem, not a bandwidth problem.
* **Where the capacity limit is.** Free-running, all placements top out at
  ~480 frames/s total, so the ceiling is the NVDEC/VIC path, not inference. Moving
  cameras to the DLAs does not add throughput, but it cuts GPU utilisation from
  59 % to 32 % and GPU+SOC power from 7.4 W to 6.1 W at the same frame rate.

---

## Smaller notes

* **NvBufSurfTransform session parameters are thread-local.** Each worker
  thread must call `NvBufSurfTransformSetSessionParams()` or it silently falls
  back to the default compute device.
* **`char` signedness.** The MMAPI sample preprocess kernel reads pixels through
  `char*`. That is unsigned on AArch64 but signed on x86, so the same code built
  for an x86 dGPU turns every value > 127 negative. This project uses `uint8_t`.
* **Coded vs visible size.** 1080p H.264 is coded as 1088 lines; the visible
  size comes from `VIDIOC_G_CROP`, not `G_FMT`.
