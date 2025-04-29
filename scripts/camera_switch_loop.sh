#!/bin/bash

cam_id=0  # Start with CAMERA_NONE

while true; do
    echo "Switching to camera $cam_id"
    ros2 topic pub /camera_switch rocx_interfaces/msg/CameraSwitch "{camera_id: {value: $cam_id}}" --once
    sleep 0.5  # Wait 2 seconds before switching again

    # Cycle through 0 → 1 → back to 0
    cam_id=$(( (cam_id + 1) % 2 ))
done
