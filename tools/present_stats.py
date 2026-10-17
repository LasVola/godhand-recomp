"""What the monitor really showed, from PS2X_PRESENT_STATS (DXGI frame statistics after each present).

usage: python tools/present_stats.py <stats.csv> [from_s] [to_s]

Each row says which present was on screen and since which monitor refresh. With one game image per refresh, every
present should stay exactly one refresh: a present shown for 2 refreshes is a repeat (the motion stops for a frame),
a present never shown is a skip (it jumps). Both read as judder even when the game delivers 60 images per second.
"""
import csv
import ctypes
import sys

path = sys.argv[1]
lo = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
hi = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9

freq = ctypes.c_int64()
ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(freq))
rows = [r for r in csv.DictReader(open(path)) if r.get("sync_qpc")]  # the last line may be cut
if not rows:
    sys.exit("no rows")
t0 = int(rows[0]["qpc"])
shown = {}  # present id -> refresh it was first shown at
for r in rows:
    t = (int(r["qpc"]) - t0) / freq.value
    if not lo <= t <= hi:
        continue
    pid, refresh = int(r["shown_present"]), int(r["shown_refresh"])
    if pid and refresh:
        shown.setdefault(pid, refresh)

ids = sorted(shown)
if len(ids) < 2:
    sys.exit("no frame statistics (not a flip-model swap chain, or the window was hidden?)")
perfect = repeats = skips = 0
pattern = []
for a, b in zip(ids, ids[1:]):
    d_id, d_ref = b - a, shown[b] - shown[a]
    if d_id == d_ref:
        perfect += d_id
        pattern.append("." * min(d_id, 3))
    elif d_ref > d_id:
        repeats += d_ref - d_id
        pattern.append("R")
    else:
        skips += d_id - d_ref
        pattern.append("S")
refreshes = shown[ids[-1]] - shown[ids[0]]
presents = ids[-1] - ids[0]
seconds = refreshes / 60.0
print(f"{refreshes} monitor refreshes, {presents} presents in the window")
print(f"repeated refreshes (an image held 2+ refreshes): {repeats}  ({repeats / max(seconds, 1e-9):.2f}/s)")
print(f"skipped presents (never shown):                  {skips}  ({skips / max(seconds, 1e-9):.2f}/s)")
print(f"smooth: {100.0 * perfect / max(presents, 1):.2f}% of presents shown exactly once")
print("pattern (. = one refresh each, R = repeat, S = skip):")
text = "".join(pattern)
for i in range(0, min(len(text), 600), 120):
    print("  " + text[i:i + 120])
