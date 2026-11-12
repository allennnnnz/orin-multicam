#!/usr/bin/env python3
"""Remux a raw H.264 elementary stream into MP4 without re-encoding.

A raw .h264 file carries no timestamps, and qtmux refuses buffers without PTS.
A pad probe after h264parse stamps each access unit with n / fps.

usage: h264_to_mp4.py IN.h264 OUT.mp4 [fps]
"""
import sys

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402


def main():
    src, dst = sys.argv[1], sys.argv[2]
    fps = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    Gst.init(None)
    pipe = Gst.parse_launch(
        f"filesrc location={src} ! h264parse name=parse ! "
        f"video/x-h264,stream-format=avc,alignment=au ! qtmux ! filesink location={dst}")
    dur = Gst.SECOND // fps
    count = [0]

    def stamp(pad, info):
        buf = info.get_buffer()
        buf.pts = buf.dts = count[0] * dur
        buf.duration = dur
        count[0] += 1
        return Gst.PadProbeReturn.OK

    pipe.get_by_name("parse").get_static_pad("src").add_probe(Gst.PadProbeType.BUFFER, stamp)
    pipe.set_state(Gst.State.PLAYING)
    msg = pipe.get_bus().timed_pop_filtered(Gst.CLOCK_TIME_NONE,
                                            Gst.MessageType.EOS | Gst.MessageType.ERROR)
    pipe.set_state(Gst.State.NULL)
    if msg.type == Gst.MessageType.ERROR:
        print("error:", msg.parse_error()[0].message)
        return 1
    print(f"{dst}: {count[0]} frames @ {fps} fps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
