"""Cadence of distinct images on screen, from a PS2X_FRAME_TIMES csv recorded with PS2X_FRAME_HASH=1 (a hash of the
displayed image per present). usage: python tools/image_cadence.py file.csv [from_s] [to_s]"""
import collections
import csv
import sys

path = sys.argv[1]
lo = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
hi = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
rows = [r for r in csv.DictReader(open(path)) if r.get("image_changed") not in (None, "") and lo <= float(r["seconds"]) <= hi and r["hash_valid"] == "1"]
changed = [int(r["image_changed"]) for r in rows]
span = float(rows[-1]["seconds"]) - float(rows[0]["seconds"])
runs = collections.Counter()
n = 0
for c in changed:
    if c:
        if n:
            runs[min(n, 4)] += 1
        n = 1
    else:
        n += 1
total = sum(runs.values()) or 1
print(f"{len(rows)} presents, {sum(changed)} new images in {span:.1f} s = {sum(changed) / span:.1f} distinct images/s")
print("each image stayed: " + ", ".join(f"{k}{'+' if k == 4 else ''} refresh(es) {v} ({100 * v / total:.0f}%)" for k, v in sorted(runs.items())))
print("pattern: " + "".join("N" if c else "." for c in changed[:120]))
