# GiantsVR — unofficial standalone Quest 2 VR port of Giants: Citizen Kabuto (in development)

An independent engineering project. It is **not** affiliated with or endorsed by Planet Moon
Studios, Interplay, GOG or Meta, and it contains **no game data**: you need your own copy of the
game.

## What works today (honest status)
- A native Quest APK (OpenXR + OpenGL ES 3, arm64) that loads your own copy of the game's `Bin/` data. It has run on
  the developer's Quest 2: the whole automated test (247 scripted runs, the campaign to the end) finished with no
  crashes, median 71.8 fps (2026-10-27). Features added since then are checked in a headless desktop harness only
  until the next headset run - see PROJECT_STATUS.md for which.
- The original worlds, characters, animations, cutscenes, music and sounds; the three campaigns (Meccaryn, Sea
  Reaper, Kabuto) with their missions, objectives and bosses; weapons and spells from the game's own tables;
  base building; races; multiplayer maps against computer players (and LAN play between headsets, untested
  with two).
- VR play: first person with your hands as the hero's (punch, grab, throw, swim, climb, bows drawn by hand,
  spells by gesture, Kabuto eating what he grabs), smooth or teleport movement, seated mode, menus by pointing
  (or bare hands), a practice range on the main menu.
- Much of the game's behaviour lives in its native code, which this project doesn't have: AI, damage numbers,
  timings and some rules are our own reading (BLOCKERS.md lists where).

## Docs
- [docs/INSTALL.md](docs/INSTALL.md) — install on a Quest, copy your game data, controls
- [ARCHITECTURE.md](ARCHITECTURE.md) — design and strategy decision
- [docs/formats/FORMATS.md](docs/formats/FORMATS.md) — verified file-format notes
- [TEST_REPORT.md](TEST_REPORT.md) · [ENGINEERING_LOG.md](ENGINEERING_LOG.md) · [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)

## Build
```
scripts/fetch_deps.sh                       # toolchain (checksum-pinned) into ../toolchains
cmake -S . -B build/host -G Ninja && ninja -C build/host && build/host/gvr_tests
scripts/build_apk.sh                        # → build/apk/GiantsVR-dev.apk
build/host/gzptool validate /path/to/Bin    # check your game data
build/host/headless_render --data /path/to/Bin --world Story1 --out story1.png
```
