"""Summary of a PS2X_FRAME_TIMES csv (with PS2X_FRAME_HASH=1): present rate, interval median/p99/max, game speed from
emulated fields, distinct images/s and how long each distinct image stayed on screen (ms): median/p99/max.
usage: python tools/pacing_summary.py <PS2X_FRAME_TIMES csv, recorded with PS2X_FRAME_HASH=1> [from_s] [to_s]"""
import csv, statistics, sys
path = sys.argv[1]
lo = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
hi = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
rows = [r for r in csv.DictReader(open(path)) if r.get("image_changed") not in (None, "") and lo <= float(r["seconds"]) <= hi]
t = [float(r["seconds"]) for r in rows]
span = t[-1] - t[0]
iv = sorted(float(r["interval_ms"]) for r in rows[1:])
fields = int(rows[-1]["field"]) - int(rows[0]["field"])
changes = [float(r["seconds"]) for r in rows if r.get("image_changed") == "1"]
hold = sorted((changes[i + 1] - changes[i]) * 1000 for i in range(len(changes) - 1))
p = lambda a, q: a[min(len(a) - 1, int(q * (len(a) - 1)))]
print(f"presents {len(rows) / span:.1f}/s  interval median {statistics.median(iv):.1f} p99 {p(iv, .99):.1f} max {iv[-1]:.1f} ms  "
      f"speed {100 * fields / span / 59.94:.0f}%  distinct {len(changes) / span:.1f}/s  "
      f"image on screen median {statistics.median(hold):.1f} p99 {p(hold, .99):.1f} max {hold[-1]:.1f} ms")
