import gi
import time
gi.require_version('Gst', '1.0')
from gi.repository import Gst, GLib

Gst.init(None)

def on_new_sample(sink):
    sample = sink.emit("pull-sample")
    if not sample:
        return Gst.FlowReturn.ERROR

    buffer = sample.get_buffer()
    pts = buffer.pts
    duration = buffer.duration

    element = sink.get_parent()
    clock = element.get_pipeline_clock()
    base_time = element.get_base_time()

    # current absolute pipeline time
    abs_time = clock.get_time() if clock else Gst.CLOCK_TIME_NONE

    # timestamp in absolute time
    if pts != Gst.CLOCK_TIME_NONE:
        # ts_abs = base_time + pts
        ts_abs = pts
        now_ns = time.monotonic_ns()
        now_gst_ns = int(now_ns)  # both are in ns
        delta_ms = (now_gst_ns - ts_abs) / 1e6
        print(f"PTS: {pts / 1e6:.3f} ms, Δ vs monotonic: {delta_ms:.3f} ms")
    else:
        print("PTS not available. you’re driving blindfolded again.")

    return Gst.FlowReturn.OK

# pipeline is unchanged
pipeline_desc = (
    "v4l2src device=/dev/video0 ! "
    "videoconvert ! "
    "video/x-raw,format=I420,width=640,height=480,framerate=30/1 ! "
    "appsink name=mysink emit-signals=true sync=false max-buffers=1 drop=true"
)

pipeline = Gst.parse_launch(pipeline_desc)
appsink = pipeline.get_by_name("mysink")
appsink.connect("new-sample", on_new_sample)

pipeline.set_state(Gst.State.PLAYING)

loop = GLib.MainLoop()
try:
    loop.run()
except KeyboardInterrupt:
    print("finally stopped your timestamp torture")
finally:
    pipeline.set_state(Gst.State.NULL)
