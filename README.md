# v4l2

> 📷 fast, zero-copy V4L2 access wrapped in modern C++ and GStreamer plugin.  
> 🧠 uses mmap, sane enums, spans, and `pts` is in terms of monotonic microseconds.

---

## 🔧 features

- zero-copy V4L2 camera capture with `mmap`
- `v4l2::V4L2Camera` C++23 API: RAII, noexcept where it matters
- `pts` is in terms of monotonic microseconds
- Ability to set driver buffer count
- enum-safe FourCC, dimensions, and framerate handling
- gstreamer plugin `v4l2-src`: wraps `v4l2::V4L2Camera` for pipelines
- supports MJPEG and YUYV formats
- tested with NVIDIA Jetson and hardware decoders

---

## 📦 structure

| Component          | Description                          |
| ------------------ | ------------------------------------ |
| `v4l2::V4L2Camera` | low-level V4L2 device access wrapper |
| `v4l2-src`         | GStreamer push-source plugin         |
| `lib-v4l2`         | compiled static lib with headers     |

---

## 📸 example: pipeline usage

simple MJPEG capture:

```bash
GST_DEBUG=3 gst-launch-1.0 \
v4l2-src device=/dev/video0 pixel-format=MJPG ! \
    image/jpeg, width=1920, height=1080, framerate=30/1 ! \
    nvv4l2decoder mjpeg=1 ! \
    nvvidconv ! \
    video/x-raw, format=I420, width=1920, height=1080 ! \
    fakesink sync=false
```

multi-branch output and h265 stream:

```bash
gst-launch-1.0 -e \
  v4l2-src device=/dev/video0 \
    pixel-format="MJPG" resolution="FHD" fps=30 ! \
  queue ! nvv4l2decoder mjpeg=1 ! \
  nvvidconv flip-method=3 ! \
  video/x-raw,format=I420,width=1280,height=800 ! \
  nvvidconv ! nvv4l2h265enc name=enc ! \
  tee name=t \
    t. ! queue ! h265parse config-interval=1 ! \
          splitmuxsink muxer-factory=mpegtsmux \
                        location="segment_%04d.ts" \
                        max-size-bytes=104857600 \
                        async-finalize=true \
    t. ! queue ! mpegtsmux ! \
          udpsink host=10.0.0.10 port=1234 \
                  sync=false async=false

```

## 🏗️ building

### prereqs

- gstreamer 1.16+
- cmake ≥ 3.20
- colcon
- `fmt`, `exception-rt`, `cmake-library`

### colcon build (recommended)

```bash
colcon build --packages-select v4l2 cmake-library exception-rt
```

### manually (if you're a masochist)

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/your/install/prefix
make -j8
make install
```

---

## 📂 install layout

```
install/
├── v4l2/
│   ├── include/...
│   ├── lib/lib-v4l2.a
│   └── bin/v4l2-demo ...
└── lib/gstreamer-1.0/
    └── v4l2-src.so ← plugin lives here
```

plugin install is fixed via `CMakeLists.txt` to **avoid being nested** in `v4l2/lib`.

---

## 🤖 C++ usage

```cpp
#include "v4l2/v4l2.hpp"

v4l2::V4l2Config config;
config.device_path_ = "/dev/video0";
config.format_ = v4l2::PixelFormat::MJPG;
config.dimension_ = v4l2::PixelDimension::DIM_1080p;
config.fps_num_ = v4l2::FPS::FPS_30;

v4l2::V4L2Camera cam(config);
cam.open_device();
cam.configure();
cam.start_streaming();

auto frame = cam.capture_frame();
fmt::print("Captured frame size: {} bytes\n", frame.image.size_bytes());
cam.release_frame();

cam.stop_streaming();
```

---

## 🧪 testing

```bash
colcon test --packages-select v4l2
```

or run manually:

```bash
./install/v4l2/bin/v4l2-test
```

---

## 💥 known issues

- MJPEG decoding assumes hardware support (use `nvv4l2decoder`)
- invalid FourCC from V4L2 will hard-fail — as it should
- bad camera drivers will make you cry. use uvcvideo or get help.

---
## 🐛 Troubleshooting
Setting `GST_DEBUG=3` will help you debug GStreamer pipelines.
- `GST_DEBUG=3 gst-inspect-1.0 v4l2-src` to check plugin registration
- `GST_DEBUG=3 gst-launch-1.0 v4l2-src device=/dev/video0` to check if the device is accessible

Setting environment variable `GST_PLUGIN_PATH` to the path of the plugin can help if the plugin is not found.
- `export GST_PLUGIN_PATH=/path/to/your/plugin` before running your GStreamer pipeline

---
## 🧠 authors

- you (for now)
- future you, trying to debug timestamps at 3 a.m.

---

## 🐵 final words

this ain't no opencv toy wrapper.  
this is bare-bones, zero-bullshit v4l2 for real-time pipelines.

use it. abuse it.  
and stop writing `VIDIOC_*` by hand, this ain't 2008.
