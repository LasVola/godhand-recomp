"""frame_stats.py <frames.csv> [--from SECONDS] [--window SECONDS]

Summarizes a PS2X_FRAME_TIMES capture (run with PS2X_FRAME_HASH=1 so image changes are known):
  fields/s   - emulated VSync fields per second of wall time (60 = full speed NTSC)
  visibleFPS - presented images that differ from the previous one, per second (what the player sees)
  presents/s - host presents per second
"""
import argparse
import csv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--from', dest='start', type=float, default=0.0)
    ap.add_argument('--window', type=float, default=5.0)
    a = ap.parse_args()
    rows = [r for r in csv.DictReader(open(a.csv)) if float(r['seconds']) >= a.start]
    if not rows:
        print('no rows')
        return
    t = float(rows[0]['seconds'])
    end = float(rows[-1]['seconds'])
    totals = []
    while t + a.window <= end:
        seg = [r for r in rows if t <= float(r['seconds']) < t + a.window]
        if len(seg) > 1:
            fields = (int(seg[-1]['field']) - int(seg[0]['field'])) / a.window
            fps = sum(1 for r in seg if r['image_changed'] == '1') / a.window
            totals.append((fields, fps))
            print(f'{t:7.1f}s fields/s={fields:5.1f} visibleFPS={fps:5.1f} presents/s={len(seg) / a.window:5.1f}')
        t += a.window
    if totals:
        print(f'average: fields/s={sum(f for f, _ in totals) / len(totals):5.1f} '
              f'visibleFPS={sum(v for _, v in totals) / len(totals):5.1f}')


if __name__ == '__main__':
    main()
