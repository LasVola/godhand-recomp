# Changelog

Newest first. Each fix is commented in the code where it was made.

## October 19, 2026

- **"Heat wave" ripples in cutscenes** (stage 3): the game's depth-of-field blur on the far background was drawn
  wrong (wavy bands at 1x, sharp with ripples at 3x). It now matches the PS2: an ordered path for the game's
  in-place depth shuffle, and the blur passes keep their original resolution (`gs_gpu_backend.cpp`).
- **Freezes up to about a second** when the game read from the disc: the port reopened the whole disc image for
  every read. It now stays open, and the image is loaded into memory in the background at start
  (`Kernel/Stubs/Helpers/Support.h`, `src/main.cpp`; `GODHAND_ISO_PREFETCH=0` turns the background load off).

## October 18, 2026 (evening)

- **Sound cutting out** (tiny gaps in music and voices, worst in stage 2 and the God Reel): the port told the game
  that every data transfer to the sound processor was still busy for one frame too long, which slowly starved the
  streamed music. Fixed in the SIF DMA status (`runtime/ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp`).
- **Grid of lines over the sepia flashback cutscenes** at 3x: some 1:1 copies were filtered slightly differently from
  the PS2. They now read pixels exactly like the PS2 does (`gs_gpu_backend.cpp`, `gs_sprite_rules.h`).
- **Thin line under movies** at 3x: the movie picture was filtered past its own edge. Fixed the same way.
- **The game could close while saving** after about 30 minutes of play: a small memory area for game threads was
  never given back. It is now reused (`EeScheduler.cpp`).
- **Button prompts show your bound keys** (for example "F" instead of the PS2 circle), or Xbox-style names with a
  controller (`src/godhand/button_prompts.cpp`).
- **Controls:**
  - Start/Select no longer reach the game while you press Back + Start for the Controls screen.
  - Alt+Tab and similar shortcuts are no longer recorded as a key while rebinding.
  - Keys bound to two buttons are flagged on the F1 screen.
  - The Windows Sticky Keys popup no longer appears while playing.
- **Rare missing 3D pieces** in busy scenes: long-running VU1 programs are now finished instead of cut off.
- **The ending credits cannot hang** if a movie ever stalls (a 20-second safety check).
- Diagnostics for developers: `GODHAND_STALL_DUMP=<ms>` prints every thread's stack when the game stalls.

## October 18, 2026 (morning)

- **Boss "CHECK" freeze** (stage 1), second cause: a "stop sound" request could interrupt the sound mixer. Such
  requests now wait for the sound driver, as on a PS2.
- **Slow cutscenes:** missing ahead-of-time VU1 programs are now logged and saved to `logs\vu1_missing\`.
- Caps Lock / Num Lock bindings follow the key, not the keyboard light.
- The launcher keeps the previous session's log.

## October 17, 2026

- First public release: playable through stage 1. Fixes for boot, pad input, graphics speckle, sound effects,
  doubled audio, the floor rendering, the boss "CHECK" freeze (first cause), and the in-game Controls screen.
