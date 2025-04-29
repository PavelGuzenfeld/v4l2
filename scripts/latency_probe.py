#!/usr/bin/env python3
import gi
gi.require_version('Gst', '1.0')
from gi.repository import Gst, GLib

Gst.init(None)

src = Gst.ElementFactory.make("v4l2src", "source")
identity = Gst.ElementFactory.make("identity", "tap")
sink = Gst.ElementFactory.make("fakesink", "sink")

pipeline = Gst.Pipeline()
for e in (src, identity, sink):
    pipeline.add(e)
src.link(identity)
identity.link(sink)

# timestamp tracking
def on_handoff(identity, buffer, pad):
    pts_ns = buffer.pts
    if pts_ns == Gst.CLOCK_TIME_NONE:
        # fallback in case v4l2src sucks
        pts_ns = Gst.util_get_timestamp()
    now_ns = GLib.get_monotonic_time() * 1000
    delta_us = (now_ns - pts_ns) // 1000
    print(f"SRC: v4l2src, DELTA={delta_us} us")


identity.connect("handoff", on_handoff)


src.set_property("num-buffers", 100)
sink.set_property("sync", True)
identity.set_property("signal-handoffs", True)
identity.set_property("silent", False)

pipeline.set_state(Gst.State.PLAYING)

bus = pipeline.get_bus()
bus.timed_pop_filtered(Gst.CLOCK_TIME_NONE, Gst.MessageType.EOS)

pipeline.set_state(Gst.State.NULL)
