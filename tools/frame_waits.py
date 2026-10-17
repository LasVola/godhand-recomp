"""Per-frame waits of the game thread from an MTVU trace: W..w (runtime sync on VU/GIF), P..p (pacing sleep),
X..x of 0x10bc38 (GS handler), 128290/146af8 (vblank handlers), chain span (first K to last H 4). usage: python tools/frame_waits.py build/port/bin/mtvu_trace.txt (run with PS2X_MTVU_TRACE=<start s>)"""
import sys, statistics
ev = []
for line in open(sys.argv[1]):
    p = line.split()
    if p[3] == "e":
        ev.append((int(p[0]), p[1], p[2]))
frames = [i for i, e in enumerate(ev) if e[1] == "F"]
rows = []
for a, b in zip(frames, frames[1:]):
    seg = ev[a:b + 1]
    span = seg[-1][0] - seg[0][0]
    acc = {"sync W..w": 0, "pacing P..p": 0, "GS handler": 0, "vblank handlers": 0}
    open_ = {}
    stack = []
    for t, c, arg in seg:
        if c == "W": open_["W"] = t
        elif c == "w" and "W" in open_: acc["sync W..w"] += t - open_.pop("W")
        elif c == "P": open_["P"] = t
        elif c == "p" and "P" in open_: acc["pacing P..p"] += t - open_.pop("P")
        elif c == "X": stack.append((t, arg))
        elif c == "x" and stack:
            t0, pc = stack.pop()
            if pc == "10bc38": acc["GS handler"] += t - t0
            elif pc in ("128290", "146af8"): acc["vblank handlers"] += t - t0
    rows.append((span, acc))
spans = [r[0] for r in rows]
print(f"{len(rows)} frames, frame median {statistics.median(spans)/1000:.2f} ms")
for k in rows[0][1]:
    vals = [r[1][k] for r in rows]
    print(f"  {k:16s} median {statistics.median(vals)/1000:.2f} ms/frame, mean {statistics.mean(vals)/1000:.2f}")
