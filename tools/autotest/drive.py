"""Closed-loop test driver: starts urban.exe, watches the window and presses buttons through PS2X_PAD_FIFO until a
goal screen is reached (default: a fight), then keeps it running for --hold seconds.

Screens are recognized by comparing a small grayscale thumbnail of the window with reference captures in
bench/refs/<name>.png (game screenshots: they live under bench/, never in the repository). Missing
references only make the driver fall back to pressing START/✕.

Usage:
  python tools/autotest/drive.py [--goal fight] [--timeout 300] [--hold 20] [--tag run1] [--env KEY=VALUE ...]
Output: bench/runs/<tag>/ (captures of every new screen, driver.log, urban.out/err).
"""
import argparse
import os
import random
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "build" / "port" / "bin"
REFS = ROOT / "bench" / "refs"
CAPTURE = ROOT / "tools" / "win" / "capture_window.ps1"
THUMB = (64, 48)
MASH_BUTTONS = ["cross", "circle", "square", "triangle", "left", "right", "up", "down", "l1", "r1", "cross+square"]

# What to press on each recognized screen to get to a fight (Challenge mode, profile 1, defaults).
ACTIONS = {
    "memcard": [],
    "title": ["start"],
    "profile_new": ["right"],
    "profile_load": ["cross"],
    "load_list": ["cross"],
    "load_dialog": ["left", "cross"],
    "main_menu": ["wait", "down", "down", "cross"],
    "mission_select": ["triangle"],  # FREE mode by mistake: back to the main menu
    "story_load_list": ["triangle"],  # CONTINUE by mistake (the menu dropped the down presses): back
    "quit_dialog": ["left", "cross"],  # "Quit Free Mode?" defaults to No
    "challenge_setup": ["cross"],
    "fighter_select": ["cross"],
    "proceed_dialog": ["cross"],
    "story_proceed_dialog": ["cross"],  # same dialog over the blurred fighter select (VRAM download), any mode
    "challenge_proceed_dialog": ["cross"],
    "loading": [],
    "fight": [],
}

# --route story: Continue → load game data 1 → first highlighted mission → solo fighter.
STORY_ACTIONS = dict(ACTIONS, **{
    "main_menu": ["wait", "cross"],
    "story_load_list": ["cross"],
    "story_load_dialog": ["left", "cross"],
    "story_mission_select": ["cross"],
    "story_mission_info": ["cross"],
    "story_fighter_select": ["cross"],
    "story_proceed_dialog": ["cross"],  # "Proceed with these settings?" over the blurred fighter select (VRAM download)
})


def thumb(path: Path) -> Image.Image:
    return Image.open(path).convert("L").resize(THUMB, Image.BILINEAR)


def load_refs() -> dict:
    refs = {}
    for p in sorted(REFS.glob("*.png")):
        refs[p.stem] = thumb(p)
    return refs


def classify(img: Image.Image, refs: dict) -> tuple[str, float]:
    if ImageStat.Stat(img).mean[0] < 4:
        return "black", 0.0
    best, score = "unknown", 1e9
    for name, ref in refs.items():
        diff = ImageStat.Stat(ImageChops.difference(img, ref)).mean[0]
        if diff < score:
            best, score = name, diff
    return (best, score) if score < 18 else ("unknown", score)


def capture(out: Path) -> bool:
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(CAPTURE),
                        "-TitleEquals", "Urban Reign", "-OutFile", str(out)], capture_output=True)
    return r.returncode == 0 and out.exists() and out.stat().st_size > 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--goal", default="fight")
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--hold", type=float, default=20)
    ap.add_argument("--tag", default=time.strftime("run_%H%M%S"))
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--route", default="challenge", choices=["challenge", "story"])
    ap.add_argument("--mash", action="store_true", help="press random buttons while on the goal screen")
    ap.add_argument("--save-at", help="quick-save (states/quick.state) the first time this screen is reached")
    ap.add_argument("--load-state", action="store_true", help="quick-load as soon as the title screen shows")
    a = ap.parse_args()

    out = ROOT / "bench" / "runs" / a.tag
    out.mkdir(parents=True, exist_ok=True)
    fifo = out / "pad_fifo.txt"
    fifo.write_text("")
    log = open(out / "driver.log", "w", encoding="utf-8")

    def say(msg):
        line = f"[{time.time() - t0:7.1f}] {msg}"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    def press(button, hold=None, gap=1.2):
        # Menus ignore input while the cursor animates, so leave a gap after each press. Directions are short: a
        # 0.4 s hold triggers the menu's auto-repeat now that the game runs near full speed.
        if hold is None:
            hold = 0.2 if button in ("up", "down", "left", "right") else 0.4
        with open(fifo, "a") as f:
            f.write(f"{button}:{hold}\n")
        time.sleep(hold + gap)

    env = dict(os.environ, PS2X_PAD_FIFO=str(fifo))
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v
    refs = load_refs()
    t0 = time.time()
    game = subprocess.Popen([str(BIN / "urban.exe")], cwd=BIN, env=env,
                            stdout=open(out / "urban.out", "w"), stderr=open(out / "urban.err", "w"))
    say(f"started urban.exe pid {game.pid}, {len(refs)} references")
    last, seen_title, goal_at, last_press = None, False, None, 0.0
    seen_loading = False
    saved = loaded = False
    shot = out / "_now.png"
    try:
        while time.time() - t0 < a.timeout:
            if game.poll() is not None:
                say(f"game exited with code {game.returncode}")
                break
            time.sleep(1.0)
            if not capture(shot):
                continue
            name, score = classify(thumb(shot), refs)
            if name != last:
                say(f"screen: {name} (diff {score:.1f})")
                os.replace(shot, out / f"{time.time() - t0:06.1f}_{name}.png")
                last = name
                shot = out / "_now.png"
            if a.load_state and name == "title" and not loaded and score < 6:
                with open(fifo, "a") as f:
                    f.write("!load\n")
                say("quick-load requested")
                loaded, seen_title = True, True
                time.sleep(3.0)
                continue
            if a.save_at and name == a.save_at and not saved and score < 6:
                time.sleep(1.0)  # let the screen settle
                with open(fifo, "a") as f:
                    f.write("!save\n")
                say(f"quick-save requested on '{name}'")
                saved = True
                time.sleep(3.0)
            if name == "title":
                seen_title = True
            if name == "loading":
                seen_loading = True
            # Stages differ from the reference fight capture: after "Now Loading", an unknown screen is a fight too.
            in_goal = name == a.goal or (a.goal == "fight" and seen_loading and name == "unknown")
            if in_goal:
                if goal_at is None:
                    goal_at = time.time()
                    say(f"goal '{a.goal}' reached ({name})")
                if time.time() - goal_at >= a.hold:
                    break
                if a.mash:
                    # Stability test: random fighting input while the goal screen is up.
                    for _ in range(4):
                        b = random.choice(MASH_BUTTONS)
                        press(b, hold=random.choice((0.1, 0.2, 0.4)), gap=0.1)
                continue
            actions = (STORY_ACTIONS if a.route == "story" else ACTIONS).get(name)
            if actions and score >= 8:
                continue  # weak match (often a transition frame): wait for a clean capture before pressing
            if actions:
                for b in actions:
                    if b == "wait":
                        time.sleep(2.5)  # let the screen finish fading in (the menu ignores input until then)
                    else:
                        press(b)
                last_press = time.time()
            elif name in ("black", "unknown") and not seen_title and time.time() - last_press > 2:
                press("start", 0.3)  # skip the intro videos
                last_press = time.time()
        if capture(out / "final.png"):
            say(f"final screen: {classify(thumb(out / 'final.png'), refs)[0]}")
    finally:
        alive = game.poll() is None
        if alive:
            game.kill()
        time.sleep(0.5)
        problems = [line.strip() for line in open(out / "urban.err", encoding="utf-8", errors="replace")
                    if "missing-target" in line or "Error during program execution" in line]
        for line in problems[:5]:
            say("PROBLEM: " + line[:300])
        say("done" + (" (goal reached)" if goal_at else " (goal NOT reached)") +
            ("" if alive else " — the game had exited") + (f", {len(problems)} problem line(s)" if problems else ""))
    sys.exit(0 if goal_at else 1)


if __name__ == "__main__":
    main()
