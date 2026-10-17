"""Per-frame times of a run, and what the game thread did in the slowest frames.

Frames are the per-frame calls to 0x128dc0 in urban.out (URBAN_CALL_TRACE=128dc0); samples come from
URBAN_NATIVE_PROFILE_TIMELINE (log time + folded stack of the game thread, 1 ms apart).

usage: python tools/frame_times.py <run dir> [--timeline file] [--from s] [--to s] [--worst 8]
       python tools/frame_times.py --capture build/port/bin/perfil_<n>   (an F6 capture: _quadros.txt + _linha.txt)
"""
import argparse
import re
import statistics
from collections import Counter
from pathlib import Path

# Coarse buckets for a stack (root first). The first match from the leaf side wins.
BUCKETS = [
    ("pacing: esperando o tempo real (vblank)", ("processDueDeadlines",)),
    ("EE esperando evento (thread ociosa)", ("EeScheduler::waitForEvent",)),
    ("sync com VU/GIF (asyncvif::sync)", ("asyncvif::sync",)),
    ("VU1 (microprograma)", ("fastBlock", "VU1Interpreter::run")),
    ("VIF1 (desempacotar/enviar ao VU1)", ("processVIF1Data",)),
    ("VU0 (microprograma)", ("executeVU0Microprogram",)),
    ("GS front end (pacotes GIF, vertexKick)", ("GS::processGIFPacket", "GifArbiter")),
    ("GS: espera/envio à thread de vídeo", ("GSThreadedBackend::",)),
    ("download de VRAM / readback", ("readback", "Readback", "LocalToHost", "localToHost", "Download")),
    ("shader (compilação/espera)", ("Shader", "shader")),
    ("IOP (rodar ciclos / esperar a thread)", ("IopSubsystem",)),
    ("CD/arquivo", ("sceCd", "Cdvd", "ReadFile", "fread")),
    ("DMA (memmove/cópias)", ("memmove", "memcpy")),
    ("locks", ("SRWLock", "Mtx_lock", "Mtx_unlock")),
    ("decodificador de animação", ("sub_00292670",)),
    ("contabilidade de ciclos/checkpoints", ("accountCycles", "checkpointDue")),
]


def bucket(stack: str) -> str:
    frames = stack.split(";")
    for name, keys in BUCKETS:
        for f in reversed(frames):
            if any(k in f for k in keys):
                return name
    leaf = frames[-1]
    if leaf.startswith("sub_"):
        return "código do jogo (EE recompilado)"
    return "outros"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run", nargs="?")
    ap.add_argument("--capture", help="F6 capture prefix, e.g. build/port/bin/perfil_3")
    ap.add_argument("--timeline")
    ap.add_argument("--from", dest="start", type=float, default=0.0)
    ap.add_argument("--to", dest="end", type=float, default=1e9)
    ap.add_argument("--worst", type=int, default=8)
    a = ap.parse_args()
    times = []
    if a.capture:
        for line in open(a.capture + "_quadros.txt", encoding="utf-8", errors="replace"):
            if line.startswith("#"):
                print(line.rstrip())
            elif line.strip():
                times.append(float(line))
        a.timeline = a.timeline or a.capture + "_linha.txt"
    else:
        for line in open(Path(a.run) / "urban.out", encoding="utf-8", errors="replace"):
            if "call 0x128dc0" in line:
                m = re.match(r"\[\s*([0-9.]+)\]", line)
                if m:
                    times.append(float(m.group(1)))
    times = [t for t in times if a.start <= t <= a.end]
    frames = [(times[i], times[i + 1] - times[i]) for i in range(len(times) - 1)]
    if not frames:
        print("no frames")
        return
    ms = sorted(d * 1000 for _, d in frames)
    p99 = ms[int(0.99 * (len(ms) - 1))]
    worst1 = ms[int(0.99 * len(ms)):] or ms[-1:]
    print(f"{len(frames)} frames from {frames[0][0]:.1f}s to {frames[-1][0]:.1f}s: {len(frames) / (frames[-1][0] - frames[0][0]):.1f} FPS")
    print(f"frame time: median {statistics.median(ms):.1f} ms, p99 {p99:.1f} ms, worst 1% average {statistics.mean(worst1):.1f} ms, worst {ms[-1]:.1f} ms")
    fields = Counter(min(4, max(1, round(d * 1000 / 16.683))) for _, d in frames)
    print("frames by fields: " + ", ".join(f"{k} field{'s' if k > 1 else ''}: {v}" for k, v in sorted(fields.items())))

    samples = []
    if a.timeline:
        for line in open(a.timeline, encoding="utf-8", errors="replace"):
            t, _, stack = line.rstrip("\n").partition(" ")
            samples.append((float(t), stack))
        samples.sort()

    def describe(start, end):
        inside = [s for t, s in samples if start <= t < end]
        if not inside:
            return "   (sem amostras nesse quadro)"
        c = Counter(bucket(s) for s in inside)
        out = [f"   {len(inside)} amostras:"]
        for name, n in c.most_common(6):
            out.append(f"     {100 * n / len(inside):5.1f}%  {name}")
        leaves = Counter(s.split(";")[-1] for s in inside)
        out.append("     folhas: " + ", ".join(f"{k} {v}" for k, v in leaves.most_common(5)))
        return "\n".join(out)

    if samples:
        lo, hi = samples[0][0], samples[-1][0]
        typical = [(t, d) for t, d in frames if lo <= t and t + d <= hi]
        if typical:
            med = statistics.median(d for _, d in typical)
            normal = [f for f in typical if abs(f[1] - med) < 0.002]
            if normal:
                t, d = normal[len(normal) // 2]
                print(f"\nquadro típico em {t:.3f}s ({d * 1000:.1f} ms):")
                print(describe(t, t + d))
        worst = sorted((f for f in frames if lo <= f[0] and f[0] + f[1] <= hi), key=lambda f: -f[1])[: a.worst]
        for t, d in worst:
            print(f"\nquadro lento em {t:.3f}s: {d * 1000:.1f} ms")
            print(describe(t, t + d))


if __name__ == "__main__":
    main()
