"""Frame pacing of an F6 capture, from its real presents (perfil_<n>_telas.txt: log time, emulated field, image sequence,
game frames so far).

Reports the present-to-present intervals, how many presents showed a new image vs the previous one again, how long each
new image stayed on screen (1, 2, 3+ presents), and how far emulated time and the game advanced between new images.

usage: python tools/present_pacing.py build/port/bin/perfil_<n> [--from s] [--to s]   (seconds into the capture)
"""
import argparse
import statistics
from collections import Counter


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--from", dest="start", type=float, default=0.0)
    ap.add_argument("--to", dest="end", type=float, default=1e9)
    a = ap.parse_args()
    rows = []
    for line in open(a.capture + "_telas.txt", encoding="utf-8", errors="replace"):
        p = line.split()
        if len(p) == 4:
            rows.append((float(p[0]), int(p[1]), int(p[2]), int(p[3])))
    if not rows:
        print("no presents")
        return
    t0 = rows[0][0]
    rows = [r for r in rows if a.start <= r[0] - t0 <= a.end]
    span = rows[-1][0] - rows[0][0]
    intervals = [(rows[i + 1][0] - rows[i][0]) * 1000 for i in range(len(rows) - 1)]
    new = [i for i in range(1, len(rows)) if rows[i][2] != rows[i - 1][2]]
    print(f"{len(rows)} presents in {span:.1f} s ({len(rows) / span:.1f}/s); new images: {len(new)} ({len(new) / span:.1f}/s)")
    iv = sorted(intervals)
    print(f"present-to-present: median {statistics.median(iv):.1f} ms, p99 {iv[int(0.99 * (len(iv) - 1))]:.1f} ms, max {iv[-1]:.1f} ms")
    buckets = Counter(min(4, max(1, round(x / 16.683))) for x in intervals)
    print("present intervals in refreshes: " + ", ".join(f"{k}{'+' if k == 4 else ''}: {v}" for k, v in sorted(buckets.items())))
    # How long each new image stayed: presents until the next new image (including itself).
    stays = Counter()
    fieldSteps = Counter()
    frameSteps = Counter()
    holdMs = []
    for j in range(len(new) - 1):
        i, k = new[j], new[j + 1]
        stays[min(4, k - i)] += 1
        holdMs.append((rows[k][0] - rows[i][0]) * 1000)
        fieldSteps[rows[k][1] - rows[i][1]] += 1
        frameSteps[rows[k][3] - rows[i][3]] += 1
    total = sum(stays.values()) or 1
    print("each new image stayed on screen for: " + ", ".join(
        f"{k}{'+' if k == 4 else ''} present(s): {v} ({100 * v / total:.0f}%)" for k, v in sorted(stays.items())))
    hm = sorted(holdMs)
    if hm:
        print(f"time on screen: median {statistics.median(hm):.1f} ms, p90 {hm[int(0.9 * (len(hm) - 1))]:.1f} ms, max {hm[-1]:.1f} ms")
    print("emulated fields between new images: " + ", ".join(f"{k}: {v}" for k, v in sorted(fieldSteps.items())[:8]))
    print("game frames between new images: " + ", ".join(f"{k}: {v}" for k, v in sorted(frameSteps.items())[:8]))


if __name__ == "__main__":
    main()
