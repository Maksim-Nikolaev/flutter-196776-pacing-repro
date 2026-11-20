# Frame pacing repro for flutter/flutter#196776 / #191245

A Flutter Linux app on Wayland that scrolls a list at a constant speed, plus a tool that shows how its frames reach the screen.

`pacing/libpacing.so` is an `LD_PRELOAD` library. It logs every `wl_surface.commit` of the app and asks the compositor for its `wp_presentation` feedback, so each frame gets the time it was committed and the time and refresh count it was shown at. `measure.sh` builds the app and the library, runs the app for 5 s + 20 s, and prints:

- the display refresh rate and the presented frames per second,
- the interval between presented frames and the number of display refreshes between them (all `1` = a new frame on every refresh),
- the latency from the commit to the display showing it.

```bash
./measure.sh                                            # stock engine
./measure.sh pr196776 path/to/libflutter_linux_gtk.so   # a patched release engine
```

Requirements: a Wayland session whose compositor supports `wp_presentation` (KWin, Mutter, Sway, …), `wayland-scanner`, `gcc`, `pkg-config`, `wayland-protocols`, Python 3. Change the monitor's refresh rate in the system settings to test other rates.

## Results

KDE Plasma (KWin 6.6.6, Wayland), AMD RX 9070, 2560x1440, Flutter 3.50.0 release build, 20 s per run, 2026-11-20.

- **stock**: the Flutter 3.50.0 engine.
- **#196776**: 3.50 + #195287 (desynchronized subsurface, merged on master) + #196776.
- **phase-locked**: 3.50 + #195287 + a local patch that locks the tick phase to the `wp_presentation` feedback.

| Display | Engine | Presented/s | Refreshes between frames | Latency p50 / p99 |
|---|---|---|---|---|
| 60 Hz | stock | 59.7 | 1 (4× 2) | 28.7 / 36.0 ms |
| 60 Hz | #196776 | 59.9 | all 1 | 18.0 / 19.0 ms |
| 60 Hz | phase-locked | 60.0 | all 1 | 8.9 / 9.2 ms |
| 144 Hz | stock | 60.0 | 2 and 3 | 7.7 / 13.4 ms |
| 144 Hz | #196776 | 144.0 | all 1 | 7.4 / 8.0 ms |
| 144 Hz | phase-locked | 144.0 | all 1 | 11.4 / 11.5 ms |
| 240 Hz | stock | 60.0 | 3, 4 and 5 | 7.8 / 11.2 ms |
| 240 Hz | #196776 | 239.3 | 1 (14× 2) | 6.5 / 7.4 ms |
| 240 Hz | phase-locked | 240.0 | all 1 | 7.3 / 7.4 ms |

A second run 25 minutes earlier gave the same rates. The 60 Hz latency of #196776 was 15.0 ms there and 18.0 ms here: its tick grid has an arbitrary phase relative to the display refresh. The phase-locked patch gave 9.0 and 8.9 ms.

The stock engine has no vsync callback, so it uses a 60 Hz timer on every display. At 144 and 240 Hz each frame stays on screen for 2–3 or 3–5 refreshes (judder). The 60 Hz latency of stock is higher because its subsurface is synchronized with the GTK parent surface (#195287 changes that).
