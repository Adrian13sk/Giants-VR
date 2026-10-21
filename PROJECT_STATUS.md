# Project status — GiantsVR (unofficial Quest 2 port)

Last updated: 2026-10-21 (session 1)

**Summary:** A native Quest APK (OpenXR + GLES 3, arm64) builds and is signed. It loads the
user's original Giants archives, decodes terrain, models and textures, and renders original
worlds (terrain, sky, sea, fog) with original character models in stereo. That is verified
only in the headless desktop harness. **Nothing has run on a Quest yet.** There is no
gameplay: no animation, AI, weapons, missions, audio or object placement.

| # | Milestone | Status | Evidence |
|---|---|---|---|
| M0 | Environment & repo inspection | DONE | ENGINEERING_LOG 2026-10-21 |
| M1 | Source / asset investigation | DONE | ARCHITECTURE.md strategy table; docs/formats/* |
| M2 | Architecture + dependencies | DONE | ARCHITECTURE.md, scripts/fetch_deps.sh (sha256 pinned) |
| M3 | Native Android/OpenXR project builds | BUILD VERIFIED | `scripts/build_apk.sh` → signed APK (v3 signature + zipalign verified by apksig) |
| M4 | Stereo rendering + head tracking | IMPLEMENTED (headless stereo verified) | Per-eye swapchains/views; harness renders correct parallax. Needs a device. |
| M5 | Quest controller input | IMPLEMENTED | Touch bindings, poses, haptics, controller visualiser. Needs a device. |
| M6 | Asset pipeline | BUILD VERIFIED (host) | gzptool validate: 5291 entries, 1561/1561 GBS, 1001 TGA, 32/32 GTI parsed |
| M7 | Original assets render correctly | IN PROGRESS | Terrain/sky/sea/fog + static character meshes render. Lighting rules, scale and texture tiling are provisional. No object placement or skinning yet. |
| M8 | Original engine/gameplay integrated | NOT STARTED | Needs object-type defs, SKN/ANM, simulation loop |
| M9 | Meaningful original gameplay | NOT STARTED | |
| M10 | VR cameras & controls integrated | IN PROGRESS | Smooth move, snap/smooth turn, left-handed swap, terrain following |
| M11 | Performance & stability | NOT STARTED | No on-device profiling possible yet |
| M12 | APK packaged & validated | BUILD VERIFIED | Manifest/badging, arm64 ELF, exported `ANativeActivity_onCreate`, NEEDED libs checked |
| M13 | Quest 2 hardware verification | BLOCKED | No headset reachable from the dev environment (see BLOCKERS.md B1) |

## Next three priorities
1. **Device smoke test (needs the user):** install the APK, copy `Bin/`, send `adb logcat -s GiantsVR` output.
2. **Object placement:** decode the object-type definition stream (`xx_*.bin` section 3:
   `typeId, 01, modelGroup, ops…`, plus mesh-id → name tables) and world/mission placements
   (op 42 records, `.abx` sets, `wm_*.bin` op 0x03). Then show buildings, trees and characters in place.
3. **Skinned characters:** SKN/ANM decode + CPU or GPU skinning, idle/walk animation for Smartie/Reaper/Kabuto.

Then: world opcode table fixes (24 of 33 worlds still stop early), Kabuto-scale camera
experiments, audio (WAV in VO_Sfx gzp), fixed-step simulation, Vulkan/multiview evaluation.

## Resume instructions
1. `scripts/fetch_deps.sh` (or reuse `../toolchains`), then build host + APK (see CLAUDE.md).
2. Original data: stage the user's `C:\GOG Games\Giants - Citizen Kabuto\Bin` (all 899 files,
   ~440 MB) into the workspace. Keep it outside the repo.
3. Check `git log` and ENGINEERING_LOG.md before changing anything.
