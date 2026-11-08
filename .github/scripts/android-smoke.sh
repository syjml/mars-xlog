#!/usr/bin/env bash
set -euo pipefail

smoke=${1:?Pass the path to the x86_64 Android smoke executable}
stl="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/x86_64-linux-android/libc++_shared.so"
test -f "$smoke"
test -f "$stl"
test -c /dev/kvm
sudo chown "$(id -u)" /dev/kvm

image='system-images;android-34;google_apis;x86_64'
sdkmanager --install emulator "$image"
printf 'no\n' | avdmanager create avd --force --name xlog-ci --package "$image"

emulator_log="$RUNNER_TEMP/android-emulator.log"
"$ANDROID_HOME/emulator/emulator" -avd xlog-ci -port 5554 -accel on -no-window -no-audio \
    -no-boot-anim -no-snapshot -gpu swiftshader > "$emulator_log" 2>&1 &
emulator_pid=$!
device=(adb -s emulator-5554)
cleanup() {
    if (( $? != 0 )); then
        tail -100 "$emulator_log"
    fi
    timeout 10s "${device[@]}" emu kill >/dev/null 2>&1 || true
    kill "$emulator_pid" 2>/dev/null || true
}
trap cleanup EXIT

timeout 180s "${device[@]}" wait-for-device
for ((attempt = 0; attempt < 90; ++attempt)); do
    if [[ $(timeout 5s "${device[@]}" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' || true) == 1 ]]; then
        break
    fi
    kill -0 "$emulator_pid"
    sleep 2
done
if (( attempt == 90 )); then
    echo "Android emulator did not finish booting" >&2
    exit 1
fi

dir=/data/local/tmp/mars-xlog-ci
"${device[@]}" shell "mkdir -p $dir"
"${device[@]}" push "$smoke" "$dir/xlog_smoke"
"${device[@]}" push "$stl" "$dir/libc++_shared.so"
timeout 60s "${device[@]}" shell "cd $dir && chmod +x xlog_smoke && LD_LIBRARY_PATH=. ./xlog_smoke ."
