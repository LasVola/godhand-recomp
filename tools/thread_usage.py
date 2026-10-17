"""CPU use of each thread of a running process (e.g. PCSX2 during a scene), by thread name. Read-only.

usage: python tools/thread_usage.py [--process pcsx2-qt.exe] [--seconds 10] [--interval 1]
Prints, per interval and as an average, the % of one core each thread used (100% = one core fully busy).
"""
import argparse
import ctypes
import time
from ctypes import wintypes

import psutil

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
THREAD_QUERY_LIMITED_INFORMATION = 0x0800
kernel32.OpenThread.restype = wintypes.HANDLE
kernel32.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.GetThreadDescription.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
kernel32.LocalFree.argtypes = [ctypes.c_void_p]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


def thread_name(tid):
    handle = kernel32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, tid)
    if not handle:
        return ""
    try:
        name = ctypes.c_wchar_p()
        if kernel32.GetThreadDescription(handle, ctypes.byref(name)) >= 0 and name.value:
            value = name.value
            kernel32.LocalFree(ctypes.cast(name, ctypes.c_void_p))
            return value
        return ""
    finally:
        kernel32.CloseHandle(handle)


def snapshot(proc):
    return {t.id: t.user_time + t.system_time for t in proc.threads()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--process", default="pcsx2-qt.exe")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--min", type=float, default=2.0, help="hide threads below this %% on average")
    a = ap.parse_args()
    procs = [p for p in psutil.process_iter(["name"]) if (p.info["name"] or "").lower() == a.process.lower()]
    if not procs:
        raise SystemExit(f"{a.process} is not running")
    proc = procs[0]
    names = {}
    totals = {}
    start = snapshot(proc)
    prev, prev_t = start, time.perf_counter()
    t0 = prev_t
    print(f"{a.process} pid {proc.pid}, {len(start)} threads, sampling {a.seconds:.0f} s")
    while time.perf_counter() - t0 < a.seconds:
        time.sleep(a.interval)
        cur, cur_t = snapshot(proc), time.perf_counter()
        dt = cur_t - prev_t
        row = []
        for tid, cpu in cur.items():
            if tid not in names:
                names[tid] = thread_name(tid) or f"tid {tid}"
            used = 100.0 * (cpu - prev.get(tid, cpu)) / dt
            if used >= a.min:
                row.append((used, names[tid]))
        row.sort(reverse=True)
        print(f"[{cur_t - t0:5.1f}s] " + "  ".join(f"{n}={u:.0f}%" for u, n in row[:8]))
        prev, prev_t = cur, cur_t
    elapsed = prev_t - t0
    end = prev
    print("\naverage over the whole window (% of one core):")
    avg = sorted(((100.0 * (end[t] - start.get(t, end[t])) / elapsed, names.get(t, str(t))) for t in end), reverse=True)
    for used, name in avg:
        if used >= a.min:
            print(f"  {used:6.1f}%  {name}")
    print(f"  process total {sum(u for u, _ in avg):.0f}% of one core")


if __name__ == "__main__":
    main()
