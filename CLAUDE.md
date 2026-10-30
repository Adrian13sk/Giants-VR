# GiantsVR — repository instructions

Unofficial standalone Meta Quest 2 VR port of *Giants: Citizen Kabuto* (2000). No original
source exists; this is a **native reimplementation that loads the user's original data files**
(GOG `Bin/` folder) and renders them through OpenXR + OpenGL ES 3 on Android arm64.

## Ground rules
- Never commit or package proprietary game data (`*.gzp`, extracted `*.gbs/*.tga/...`, renders of
  them). The APK contains only our code; users copy their own `Bin/` folder to the headset.
- Do not copy code from the decompilation repo (`YYoudi/vibe-giants-decomp`, no licence) or from
  GiantsTools (no licence) or GiantZiP (GPL-2.0). Use them only as *factual* format references and
  verify every claim against real files (record evidence in `docs/formats/`).
- Label status honestly: IMPLEMENTED / BUILD VERIFIED / DEVICE VERIFIED. Nothing is device
  verified until it has run on a Quest.

## Layout
- `src/core` math/log · `src/render` GLES3 renderer · `src/assets` file formats (GZP, GBS, GTI,
  world .bin, SKN/ANM, images) · `src/scene` terrain/model cache/worlds · `src/game` VR rig, input state,
  GameApp (platform-independent app core) · `src/xr` OpenXR · `src/platform/android` entry point.
- `tools/headless` Mesa/EGL harness running the same GameApp → stereo PNG. `tools/gzptool`
  archive/asset validator. `tools/py` Python reference decoders used for format RE.
- `tests/` unit tests on synthetic data (no proprietary data needed).

## Build
- Toolchain: `scripts/fetch_deps.sh` (GitHub-hosted, checksum pinned; Google hosts are blocked
  in the cloud dev environment). Default location `../toolchains`.
- Host: `cmake -S . -B build/host -G Ninja && ninja -C build/host && build/host/gvr_tests`
- Quest APK: `scripts/build_apk.sh` → `build/apk/GiantsVR-dev.apk`
- Asset validation: `build/host/gzptool validate <path to Bin>`
- Visual check: `build/host/headless_render --data <Bin> --world Story1 --out out/x.png`

## Conventions
- Runtime frame: right-handed, +Y up, -Z forward, metres. Giants data is Z-up:
  `runtime = (-x, z, y) * gGameUnitsToMeters` (0.2, provisional). Winding preserved.
- With every APK, deliver EVERY changed devtest script too (all of `devtest/<set>/`), and stage one back to
  compare: the 2026-10-27 whole test ran six stale `tests/` scripts that had been fixed but never copied.
- Delivering an APK to the user's PC: stage it under a UNIQUE name (e.g. `GiantsVR-dev-<commit>.apk`).
  Re-sending the same staged path can silently deliver stale content. Then stage the PC copy back
  and compare its sha256 against the build before telling the user to install.
- `scripts/build_apk.sh` keeps each build's unstripped library as `build/symbols/<commit>.so` (build from a clean
  commit before delivering); crash reports from the headset: `tools/py/symbolize_crash.py <report> build/symbols/<commit>.so`.
- ASan/UBSan runs that take a while (all devtests, the mission sweep): build them from a snapshot worktree
  (`git worktree add <dir> HEAD`, its own `build/asan` with `-fsanitize=address,undefined`), never from the working
  tree while editing - a compile that reads half-edited headers gives a binary with mismatched struct layouts.
- Keep docs current: PROJECT_STATUS.md, ENGINEERING_LOG.md, BLOCKERS.md, TEST_REPORT.md.
- GitHub: `origin` = github.com/Adrian13sk/Giants-VR (public). Its `main` is this repo's `rework` (push `rework:main`);
  `classic` = the classic version. From the cloud session tags and Releases are refused by policy; releases are made
  in the user's own Chrome (Claude in Chrome, signed in as them; upload the APK from a staged copy under
  /mnt/user-data/uploads). v1.0-rc2 = GiantsVR-FINAL-368d43d.apk (2026-10-30). Never push game data or renders of it.
