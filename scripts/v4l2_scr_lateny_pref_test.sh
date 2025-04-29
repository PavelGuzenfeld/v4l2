#!/bin/bash
set -euo pipefail

NUM_BUFFERS=100
_LOG="_latency.log"
V4L2_LOG="v4l2_latency.log"

echo "Running v4l2src..."
gst-launch-1.0 -q v4l2-src num-buffers=$NUM_BUFFERS ! fakesink sync=true > "$_LOG" 2>&1 || true

echo "Running v4l2src via latency_probe.py..."
./latency_probe.py > "$V4L2_LOG" 2>&1 || true

echo "Parsing logs..."

# analyze latency logs from both
python3 - <<EOF
import re

def parse(path):
    with open(path) as f:
        return [
            int(m.group(1))
            for line in f
            if (m := re.search(r'DELTA=(\d+)\s+us', line))
        ]

_lat = parse("$_LOG")
v4l2_lat = parse("$V4L2_LOG")

def stats(name, latencies):
    print(f"\n{name}:")
    print(f"  frames: {len(latencies)}")
    print(f"  min: {min(latencies)} us")
    print(f"  max: {max(latencies)} us")
    print(f"  avg: {sum(latencies) // len(latencies)} us")

if not _lat or not v4l2_lat:
    print("❌ Failed to extract latencies.")
    exit(1)

stats("v4l2-src", _lat)
stats("v4l2src", v4l2_lat)
EOF
