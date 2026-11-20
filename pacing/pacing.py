#!/usr/bin/env python3
"""Reads a log of the pacing library and reports the time between presented frames for one run."""

import json
import statistics
import sys
from collections import Counter


def parse(path):
    surfaces = {}
    arrivals = {}
    pid = 0

    with open(path, errors="replace") as f:
        for line in f:
            parts = line.split()

            if not parts:
                continue

            kind = parts[0]

            if kind == "V" and len(parts) >= 4:
                pid = int(parts[3])
            elif kind == "A" and len(parts) == 3:
                arrivals[int(parts[2])] = int(parts[1])
            elif kind == "C" and len(parts) == 7:
                t, sid, no, attached, mark, tid = (int(x) for x in parts[1:])
                surfaces.setdefault(sid, {})[no] = {"no": no, "t": t, "attached": attached, "mark": mark, "tid": tid}
            elif kind == "P" and len(parts) == 7:
                sid, no, t, refresh, msc, flags = (int(x) for x in parts[1:])
                commit = surfaces.get(sid, {}).get(no)

                if commit is not None:
                    commit.update(presented=t, refresh=refresh, msc=msc, flags=flags)
            elif kind == "D" and len(parts) == 3:
                commit = surfaces.get(int(parts[1]), {}).get(int(parts[2]))

                if commit is not None:
                    commit["discarded"] = True

    return pid, surfaces, arrivals


def spread(values):
    if not values:
        return None

    ordered = sorted(values)

    return {
        "p50": round(statistics.median(ordered), 3),
        "p99": round(ordered[min(len(ordered) - 1, int(len(ordered) * 0.99))], 3),
        "max": round(ordered[-1], 3),
        "min": round(ordered[0], 3),
    }


def on_screen(commits):
    """Returns the commits that reached the screen, and the count of commits that share a presentation time with a later commit and so were never visible."""
    reported = sorted((c for c in commits if "presented" in c), key=lambda c: (c["presented"], c["no"]))
    shown = [c for c, later in zip(reported, reported[1:] + [None]) if later is None or later["presented"] != c["presented"]]

    return shown, len(reported) - len(shown)


def surface_stats(commits, pid, seconds):
    shown, superseded = on_screen(commits)
    refresh = statistics.median(c["refresh"] for c in shown) if shown else 0
    intervals = [(b["presented"] - a["presented"]) / 1e6 for a, b in zip(shown, shown[1:])]
    steps = Counter()

    for a, b in zip(shown, shown[1:]):
        if a["msc"] and b["msc"]:
            steps[b["msc"] - a["msc"]] += 1
        elif refresh:
            steps[round((b["presented"] - a["presented"]) / refresh)] += 1

    return {
        "commits": len(commits),
        "with_buffer": sum(1 for c in commits if c["attached"] == 1),
        "presented": len(shown),
        "discarded": sum(1 for c in commits if c.get("discarded")),
        "superseded": superseded,
        "unresolved": sum(1 for c in commits if "presented" not in c and not c.get("discarded")),
        "off_main_thread": sum(1 for c in commits if c["tid"] != pid),
        "presented_per_s": round(len(shown) / seconds, 2) if seconds else 0,
        "refresh_hz": round(1e9 / refresh, 3) if refresh else 0,
        "interval_ms": spread(intervals),
        "refresh_steps": {str(k): v for k, v in sorted(steps.items())},
        "latency_ms": spread([(c["presented"] - c["t"]) / 1e6 for c in shown]),
    }


def video_stats(commits, seconds, arrivals, t0_ns, t1_ns):
    shown, _ = on_screen(commits)
    refresh = statistics.median(c["refresh"] for c in shown) if shown else 0
    frames = []

    for c in shown:
        if c["mark"] and (not frames or c["mark"] != frames[-1]["mark"]):
            frames.append(c)

    if len(frames) < 2:
        return None

    intervals = [(b["presented"] - a["presented"]) / 1e6 for a, b in zip(frames, frames[1:])]
    period = statistics.median(intervals)
    holds = Counter(round(i * 1e6 / refresh) for i in intervals) if refresh else Counter()
    jumps = [b["mark"] - a["mark"] for a, b in zip(frames, frames[1:])]
    start = frames[0]["presented"]
    long_at = [round((a["presented"] - start) / 1e9, 2) for a, i in zip(frames, intervals) if i > period * 1.5]
    skipped_at = [round((a["presented"] - start) / 1e9, 2) for a, j in zip(frames, jumps) if j > 1]
    arrived = sorted(t for t in arrivals.values() if (t0_ns is None or t >= t0_ns) and (t1_ns is None or t <= t1_ns))
    waits = [(c["presented"] - arrivals[c["mark"]]) / 1e6 for c in frames if c["mark"] in arrivals]

    return {
        "frames": len(frames),
        "frames_per_s": round(len(frames) / seconds, 2) if seconds else 0,
        "interval_ms": spread(intervals),
        "hold_refreshes": {str(k): v for k, v in sorted(holds.items())},
        "skipped_frames": sum(j - 1 for j in jumps if j > 1),
        "backward_marks": sum(1 for j in jumps if j < 0),
        "long_holds": len(long_at),
        "long_holds_at_s": long_at[:12],
        "skipped_at_s": skipped_at[:12],
        "arrived": len(arrived),
        "arrival_interval_ms": spread([(b - a) / 1e6 for a, b in zip(arrived, arrived[1:])]),
        "arrival_to_screen_ms": spread(waits),
    }


def analyse(path, t0_ns=None, t1_ns=None):
    pid, surfaces, arrivals = parse(path)
    result = {"surfaces": {}}
    best = None

    for sid, by_no in surfaces.items():
        commits = [c for c in by_no.values() if (t0_ns is None or c["t"] >= t0_ns) and (t1_ns is None or c["t"] <= t1_ns)]

        if not commits:
            continue

        seconds = (t1_ns - t0_ns) / 1e9 if t0_ns is not None and t1_ns is not None else (max(c["t"] for c in commits) - min(c["t"] for c in commits)) / 1e9
        stats = surface_stats(commits, pid, seconds)
        result["surfaces"][str(sid)] = stats

        rank = (stats["presented"] + stats["discarded"] > 0, stats["off_main_thread"] > 0, stats["presented"], sid)

        if best is None or rank > best[0]:
            best = (rank, sid, commits, seconds)

    if best is None:
        return result

    _, sid, commits, seconds = best
    result["surface"] = sid
    result.update(result["surfaces"][str(sid)])
    result["video"] = video_stats(commits, seconds, arrivals, t0_ns, t1_ns)

    return result


if __name__ == "__main__":
    window = [int(x) for x in sys.argv[2:4]]

    print(json.dumps(analyse(sys.argv[1], *window), indent=1))
