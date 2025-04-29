#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import gi
import threading
import time
import signal
import sys

gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib

Gst.init(None)

# long-ass pipeline string from your setup
pipeline_description = """
  v4l2-src device=/dev/video0 pixel-format=MJPG resolution=FHD fps=30 name=vsrc0 !
    queue !
    nvv4l2decoder mjpeg=1 !
    nvvidconv flip-method=3 !
    video/x-raw, format=I420, width=1080, height=1920 !
    camera-input-selector.sink_0
  v4l2-src device=/dev/video2 pixel-format=MJPG resolution=FHD fps=30 name=vsrc2 !
    queue !
    nvv4l2decoder mjpeg=1 !
    nvvidconv flip-method=0 !
    video/x-raw, format=I420, width=1920, height=1080 !
    camera-input-selector.sink_1
  input-selector name=camera-input-selector sync-mode=1 cache-buffers=false sync-streams=false !
    tee name=t_prefix
    t_prefix. ! queue max-size-buffers=16 leaky=downstream !
      fakesink
    t_prefix. ! queue max-size-buffers=16 leaky=downstream !
      nvvidconv !
      video/x-raw\\(memory:NVMM\\),width=1280,height=800 !
      nvv4l2h265enc name=nvvenc bitrate=20000 peak-bitrate=25000 insert-sps-pps=1 maxperf-enable=true num-B-Frames=0 insert-vui=1 preset-level=1 idrinterval=30 !
      mpegtsmux alignment=7 name=mux !
      udpsink host=10.0.0.10 port=1234 name=udp_sink sync=false async=false
"""

loop = GLib.MainLoop()
index = 0
selector = None

def switcher():
    pad_0 = selector.get_static_pad("sink_0")
    pad_1 = selector.get_static_pad("sink_1")

    if not pad_0 or not pad_1:
        print("💀 failed to fetch static pads. something is deeply wrong.")
        return

    pads = [pad_0, pad_1]
    index = 0

    while True:
        time.sleep(3)
        active = pads[index]
        print(f"🎥 switching to sink_{index}")

        # flush so gstreamer doesn't act like it's in a coma
        selector.send_event(Gst.Event.new_flush_start())
        selector.send_event(Gst.Event.new_flush_stop(False))

        selector.set_property("active-pad", active)

        index = 1 - index

        caps = active.get_current_caps()
        print(f"🎯 active pad caps: {caps.to_string() if caps else 'n/a'}")





def sig_handler(sig, frame):
    print("🔻 received signal, quitting...")
    loop.quit()


def main():
    global selector

    signal.signal(signal.SIGINT, sig_handler)

    pipeline = Gst.parse_launch(pipeline_description)
    selector = pipeline.get_by_name("camera-input-selector")

    if selector is None:
        print("💀 failed to get input-selector")
        return

    pipeline.set_state(Gst.State.PLAYING)

    vsrc0 = pipeline.get_by_name("vsrc0")
    vsrc2 = pipeline.get_by_name("vsrc2")

    vsrc0.set_state(Gst.State.PLAYING)
    vsrc2.set_state(Gst.State.PLAYING)


    # background thread for switching
    threading.Thread(target=switcher, daemon=True).start()

    try:
        loop.run()
    finally:
        pipeline.set_state(Gst.State.NULL)


if __name__ == "__main__":
    main()
