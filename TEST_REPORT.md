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
| Scene content | probable, start-up log confirms the world loaded | 218–246 draw calls per frame (both eyes), consistent with the Story1 world plus animated characters, not the test scene. Needs the user's visual confirmation. |
| Start-up path | **PASS** (device test 1b, 14:15) | See below |

### Device test 1b — start-up log (2026-10-21 14:15)
- GL: Adreno 650, OpenGL ES 3.2. Runtime **Oculus 85.200.0**, system "Oculus Quest2".
- Views 1440×1584 per eye. Swapchain format 0x8C43 (GL_SRGB8_ALPHA8), 3 images per eye.
- Reference space: LOCAL_FLOOR_EXT (1000426000).
- Session went IDLE → READY → SYNCHRONIZED → VISIBLE → FOCUSED with no errors.
- Supported refresh rates: 60/72/80/90/96/100/120 Hz. 72 Hz requested and applied (the system briefly reported 90 first).
- Data directory `/storage/emulated/0/Android/data/io.github.giantsvr.dev/files/Bin`: **129 archives mounted**, the full user `Bin` folder.
- Story1 world, terrain, 4 skinned characters and 2 props loaded, all 100 % bound.
- Start-up timing: process start to world loaded in 0.53 s; to FOCUSED in 0.76 s.
- Frame rate: 65.5 fps in the first 5 s window (start-up), then a steady 72.0 fps.
- Draw calls: 174 per frame, 218–246 when the controllers are visible.
- No warnings or errors from GiantsVR.

## Device test 2 — 2026-10-21 14:33, user's Quest 2 (build c57e48b)
User report: "works fine". The fixes are confirmed in the headset: Kabuto walks on both legs,
the animations look natural, and the brightness is acceptable.

## NOT verified (requires hardware)
- APK install and launch on Quest 2; OpenXR runtime negotiation (loader broker, extensions)
- Session lifecycle (READY/FOCUSED/STOPPING), suspend/resume, Oculus-button recentre
- Swapchain format selection on the device, sRGB correctness in the headset
- Head-tracking latency, frame timing (72 Hz target), CPU/GPU load, thermals
- Controller bindings, poses, haptics; the interaction-profile change event
- Original-data loading from `/sdcard/Android/data/.../files/Bin` (path, permissions, load time)
- Load-time hitch: the world is currently loaded synchronously at start-up
