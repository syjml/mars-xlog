#!/usr/bin/env bash
set -euo pipefail

smoke=${1:?Pass the absolute path to the iOS simulator smoke executable}
test -x "$smoke"

# Use a device type known to work with an installed iOS runtime on this image.
device_config=$(xcrun simctl list devices available -j | python3 -c '
import json
import re
import sys

candidates = []
for runtime, devices in json.load(sys.stdin)["devices"].items():
    match = re.fullmatch(r"com\.apple\.CoreSimulator\.SimRuntime\.iOS-([0-9-]+)", runtime)
    if not match:
        continue
    for device in devices:
        if device["isAvailable"] and device["name"].startswith("iPhone"):
            version = tuple(map(int, match[1].split("-")))
            candidates.append((version, runtime, device["deviceTypeIdentifier"]))
if not candidates:
    sys.exit("No available iPhone simulator runtime")
_, runtime, device_type = max(candidates)
print(runtime, device_type)
')
read -r runtime device_type <<< "$device_config"
device=$(xcrun simctl create "Mars xlog CI" "$device_type" "$runtime")
cleanup() {
    xcrun simctl shutdown "$device" >/dev/null 2>&1 || true
    xcrun simctl delete "$device" >/dev/null 2>&1 || true
}
trap cleanup EXIT

xcrun simctl boot "$device"
xcrun simctl bootstatus "$device" -b
xcrun simctl spawn "$device" "$smoke" /tmp
