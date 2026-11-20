#!/usr/bin/env bash
# Measures how a Flutter Linux app's frames reach the screen on Wayland. Runs
# a list that scrolls on every frame for SECONDS (default 20) after a 5 s
# warm-up, logs each wl_surface.commit with its wp_presentation feedback, and
# prints the presented frames per second, the refreshes between frames and
# the commit-to-screen latency.
#
#   ./measure.sh                                         # stock engine
#   ./measure.sh my-label path/to/libflutter_linux_gtk.so  # a patched release engine
#
# Run it in a Wayland session on the monitor to test; the result also goes to
# results/<label>.json.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
label=${1:-stock}
lib=${2:-}
seconds=${SECONDS_TO_MEASURE:-20}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

[[ -n ${WAYLAND_DISPLAY:-} ]] || { echo "needs a Wayland session" >&2; exit 2; }

"$here/pacing/build.sh"
(cd "$here" && flutter build linux --release > "$work/build.log" 2>&1) || { cat "$work/build.log"; exit 1; }

bundle="$here/build/linux/x64/release/bundle"

install -m 0755 "${lib:-$here/linux/flutter/ephemeral/libflutter_linux_gtk.so}" "$bundle/lib/libflutter_linux_gtk.so"

monotonic() { python3 -c 'import time; print(time.clock_gettime_ns(time.CLOCK_MONOTONIC))'; }

GDK_BACKEND=wayland LD_PRELOAD="$here/pacing/libpacing.so" PACING_LOG="$work/pacing.log" \
    "$bundle/repro_196776" > "$work/app.log" 2>&1 &
app=$!

sleep 5
t0=$(monotonic)
sleep "$seconds"
t1=$(monotonic)
sleep 0.6
kill "$app"
wait "$app" 2> /dev/null || true

mkdir -p "$here/results"

python3 -I - "$here/pacing" "$work/pacing.log" "$t0" "$t1" "$label" "$here/results/$label.json" <<'EOF'
import json
import sys

sys.path.insert(0, sys.argv[1])

from pacing import analyse

r = analyse(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
r.pop("surfaces", None)
r.pop("video", None)
r["label"] = sys.argv[5]

with open(sys.argv[6], "w") as f:
    json.dump(r, f, indent=1)

steps = ", ".join(f"{k}: {v}" for k, v in r["refresh_steps"].items())
print(f"{r['label']}: display {r['refresh_hz']:.2f} Hz, presented {r['presented_per_s']:.1f}/s")
print(f"  frame interval p50 {r['interval_ms']['p50']:.2f} ms, p99 {r['interval_ms']['p99']:.2f} ms")
print(f"  refreshes between frames {{{steps}}}")
print(f"  commit-to-screen latency p50 {r['latency_ms']['p50']:.2f} ms, p99 {r['latency_ms']['p99']:.2f} ms")
EOF
