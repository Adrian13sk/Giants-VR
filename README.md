# GiantsVR — unofficial standalone Quest 2 VR port of Giants: Citizen Kabuto (in development)

An independent engineering project. It is **not** affiliated with or endorsed by Planet Moon
Studios, Interplay, GOG or Meta, and it contains **no game data**: you need your own copy of the
game.

## What works today (honest status)
- A native Quest APK (OpenXR + OpenGL ES 3, arm64). It builds and is signed, but has **not yet
  been run on a headset**.
- It reads the original `Bin/*.gzp` archives: models (GBS), textures (TGA), terrain (GTI) and
  world definitions.
- It renders original worlds (terrain, sky, sea, fog) and original character models in stereo
  VR. This has been verified in a headless desktop harness only.
- Touch controller input, smooth locomotion, snap turning and terrain following.

**Not yet:** animation, gameplay, AI, weapons, missions, audio, menus, object/building placement.
See [PROJECT_STATUS.md](PROJECT_STATUS.md) and [BLOCKERS.md](BLOCKERS.md).

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
