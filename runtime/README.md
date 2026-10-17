# runtime/

The PS2 runtime, recompiler and offline tools of **PS2Recomp** (GPL-3.0, `LICENSE` here), kept in this repository
with the Urban Reign port's changes instead of as a git submodule.

- **Upstream:** [TaylorNAlbarnaz/PS2Recomp](https://github.com/TaylorNAlbarnaz/PS2Recomp), branch `sotc-port`
  (the fork used by the [Shadow of the Colossus port](https://github.com/LightVelox/sotc-vibe-pc), from [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp)), commit
  `ac9efa0` (2026-10-01). Its original README is `README.upstream.md`.
- **Changes since then:** everything in `docs/PROGRESS.md` that touches `runtime/` (GS threading, MTVU timing, VU1
  AOT, IOP/SIF, PCSX2 GS back end, IPU, pnach, savestates...). Until 2026-10-12 they lived as uncommitted changes in
  the submodule, exported to `docs/runtime-urban-port.patch`; that file is gone now that the code lives here.
- **Kept:** `ps2xRuntime` (EE/VU/GS/IOP host runtime), `ps2xIOP` (IOP emulation), `ps2xRecomp` (the recompiler; the
  prebuilt `tools/ps2_recomp.exe` comes from it, `-DURBAN_BUILD_RECOMP=ON` builds it), `ps2xTest/gs_reference` and
  `ps2xTest/tools` (vu1_recomp, gs_replay, vu0_audit; `-DURBAN_BUILD_TOOLS=ON`).
- **Left out:** the analyzer, the studio, the unit tests and the Android/Vita targets. `CMakeLists.txt` here and in
  `ps2xTest/` replace upstream's for that.
- `ps2xRuntime/src/lib/ipu/` comes from the [Ace Combat 5 static recompilation](https://github.com/sal063/Ace-Combat-5-Static-recompilation) (Apache-2.0,
  `docs/licenses/AC5-Recomp-APACHE-2.0.txt`).
