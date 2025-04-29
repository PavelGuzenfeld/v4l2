#!/usr/bin/env bash
set -euo pipefail

echo "=== Encoding captured JPEGs → H.265 ==="
gst-launch-1.0 -v \
  multifilesrc \
    location="capture_%03d.jpg" index=1 \
    caps="image/jpeg,framerate=30/1" \
  ! jpegparse \
  ! nvv4l2decoder mjpeg=1 \
  ! nvvidconv \
  ! 'video/x-raw(memory:NVMM), format=NV12, width=1280, height=800' \
  ! nvv4l2h265enc \
      iframeinterval=1 \
      insert-sps-pps=1 \
      maxperf-enable=1 \
  ! h265parse \
  ! multifilesink \
      location="frame_%04d.h265" \
      next-file=key-frame \
  2>&1 | tee encode_from_jpegs.log

echo
echo "=== Files on disk ==="
ls -lh frame_*.h265 || echo "No H265 files produced"
