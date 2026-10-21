# Test report

Environment: Ubuntu 24.04 x86_64 cloud container; Mesa 25.2.8 llvmpipe (GLES 3.2) for headless
rendering; NDK r27c (clang 18) for arm64. No Android device or emulator available.

## Session 1 — 2026-10-21

| Test | Command | Result |
|---|---|---|
| Unit tests (synthetic data: LZSS, GZP, GBS, GTI, world bin, math, VR rig) | `build/host/gvr_tests` | **PASS** ("all tests passed") |
| Original archive extraction | `build/host/gzptool validate <Bin>` (89 staged *.gzp + world bins) | **PASS** 5291/5291 entries decompressed to declared sizes |
| GBS model parsing | same | **PASS** 1561/1561 (strict: all indices validated, zero trailing bytes) |
| TGA decoding | same | **PASS** 1001/1001 |
| GTI terrain parsing | same | **PASS** 32/32 (stream consumed to ≤4 bytes from EOF for 27; 5 leave 164–796 trailing bytes, which are unexplained) |
| World definition streams | same | **PARTIAL** 9/33 complete, 24 stop early at an unknown/mis-sized opcode |
| Model winding after Z-up→Y-up conversion | same (signed volume) | 1463 positive / 98 negative (open meshes), so no flip |
| Headless stereo render, test scene + models | `headless_render --data <Bin> --out out/stereo_assets.png` | **PASS** (visual check: textured Smartie/Reaper, correct stereo parallax) |
| Headless render, original worlds | `headless_render --data <Bin> --world {Story1,Kstory1,Rstory1,M_Reaper_L1}` | **PASS** (visual check: terrain, sky, sea, fog render; lighting provisional) |
| SKN skin parsing | `gzptool validate <Bin>` | **PASS** 782/782 |
| ANM animation parsing | same | **PASS** 1318/1318. 3 tracks use key types that aren't supported yet |
| Rigid skinning correctness | `gzptool skincheck <Bin> sm_L0 sm_stand.anm` | **PASS** 89/706 vertices reproduced exactly: every bone whose stand pose equals the export pose (root, spine base, right leg, tail). Other bones differ only because the pose differs. |
| Animated characters in world | `headless_render --world Story1 --frames 500 --move 0 -1` | **PASS** (visual: Smartie, Meccaryn, Reaper and Kabuto animate and are lit; Kabuto ≈16 m) |
| Android native build | `scripts/build_apk.sh` | **PASS** (warnings only from stb_image) |
| APK packaging / signing | same (apksig verify) | **PASS** zipalign verified, signature verified (v3) |
| APK metadata | `aapt2 dump badging`, `llvm-readelf -d`, `llvm-nm -D` | **PASS** package io.github.giantsvr.dev, min 29 / target 32, headtracking feature, arm64-v8a libgiantsvr.so + libopenxr_loader.so, `ANativeActivity_onCreate` exported |

## Device test 1 — 2026-10-21, user's Quest 2 (build cb8236e)
Log: `adb logcat -d -s GiantsVR OpenXR` captured by the user from 13:23:25 to 13:25:21, about 2 minutes.
The start-up lines were not in the capture.

| Check | Result | Evidence |
|---|---|---|
| APK installs and launches | **PASS** | process 9006 logging from the render thread |
| OpenXR session running, frames submitted | **PASS** | continuous frame loop; the display refresh request was applied |
| Frame rate | **PASS** 71.8 fps at 72 Hz, stable for about 2 min | `perf:` lines every 5 s |
| Touch controllers bound | **PASS** | `/interaction_profiles/oculus/touch_controller` on both hands. The profile events repeat on controller sleep/wake and system menu use. |
| Scene content | probable | 218–246 draw calls per frame (both eyes), consistent with the Story1 world plus animated characters, not the test scene. Needs the user's visual confirmation. |
| Start-up path (loader, runtime name, data dir, world load) | not captured | the start-up lines were not in the log |

## NOT verified (requires hardware)
- APK install and launch on Quest 2; OpenXR runtime negotiation (loader broker, extensions)
- Session lifecycle (READY/FOCUSED/STOPPING), suspend/resume, Oculus-button recentre
- Swapchain format selection on the device, sRGB correctness in the headset
- Head-tracking latency, frame timing (72 Hz target), CPU/GPU load, thermals
- Controller bindings, poses, haptics; the interaction-profile change event
- Original-data loading from `/sdcard/Android/data/.../files/Bin` (path, permissions, load time)
- Load-time hitch: the world is currently loaded synchronously at start-up
