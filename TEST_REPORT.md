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

## Host verification — object placement (2026-10-21 14:40, commit 91b80bf)
- `ninja -C build/host && build/host/gvr_tests`: all tests passed.
- `headless_render --data <Bin> --world Story1`:
  - Log: "Object registry: 629 types, 626 meshes", "Mission 1a_1_timmy: 170 placements",
    "Level objects: 135 placed, 115 helpers skipped, 0 without a model".
  - Whole host run 0.56 s.
- Culling A/B: the village ground and top views are pixel-identical with culling on and off. Ground-view draw calls
  dropped from 286 to 185.
- Story4 placed 2 objects, Rstory1 36, Story2 0 (world stream stops early, B5).
- APK: `scripts/build_apk.sh` produced a signed APK, 2,859,507 bytes, sha256 84835f3c…97d2. Copied to the user's
  Documents\GiantsQuestVR. **Not yet device-tested.**

## Device test 3 — 2026-10-21 15:13, user's Quest 2 (build f817084)
User report: "that worked, now it is as it is supposed". The village buildings (skinned walls and houses in their default pose), the raft, the canopy and the fly/noclip mode are confirmed in the headset.

## Device tests 4–7 — 2026-10-21 17:11–17:37, user's Quest 2
- Test 4 (2e9f917, third-person Meccaryn): "it is fine, the movement is kinda slow". The user asked for first person as the primary mode, with body and hands.
- Test 5 (52d77cd, first-person body + IK arms): "torso is fine". Issues: turning snaps, the torso sinks after a recentre, arms barely visible, hands too big. Fixed in ad9823f.
- Test 6 (ad9823f): "way better". The user wants the arms to stretch further; my first attempt (556548f) misread this as limiting the stretch.
- Test 7 (13fdb40, dynamic reach stretch): **"Now it's perfect"**. First-person body, arm IK, hands and turning are DEVICE VERIFIED.
- Test 8 (ca0d903, level collision): **"Everything works"**. Wall collision, sliding, landing on tops and walk-through plants are DEVICE VERIFIED.

## Device tests 9–14 — 2026-10-21 17:45–18:43, user's Quest 2
- FP gun on the controller (b2c…): "works flawlessly in FP". Third-person aim and gun size were then reworked (crosshair, scale 0.75).
- Reapers (49fdcaf → 6b7c586): "works fine". Feedback drove: running and stand-to-shoot, bodies on the ground, the 50° walk slope limit, the mc_skid slide, and the cliff rule for enemies. All accepted on device.
- Audio (3afbc82, sha256 fab0d844…): **"it works just fine"**. AAudio output, positional mixing and the inferred sound mapping are DEVICE VERIFIED. The user didn't flag any wrong sound.

## Host verification — missions and creatures (2026-10-21 22:40 local, build 1dc7f45…)
- `gvr_tests`: all pass. New tests: mission stream (objects, mounts, tilts, rejects truncation/unknown ops/trailing data);
  definition section (meshes, groups, LOD, anim slots, fx events, type class and scale, corrupt op rejected); Ripper AI
  (alert, chase, bite, out-of-sight idle, kill thresholds by size, raycast, can't climb a cliff).
- `tools/py/mission.py`: 56/56 wm_*.bin files walk exactly to EOF. `tools/py/objsect.py`: every .bin parses (807 types).
- `headless_render --world Story1`:
  - Log: "Object registry: 807 types, 799 groups, 1570 meshes", "Mission 1a_1_timmy: 344 objects (1 mounted)",
    "Level objects: 300 placed, 9 characters", "Mission characters: 6 Rippers, 0 Reapers, 3 ambient".
  - Combat at Timmy's tower:
    - 600 frames without firing: health 28.
    - 300 frames with `--trigger 1 --auto-aim`: the 3 nearby Rippers die, health 100.
- APK: signed, 3,047,923 bytes, sha256 1dc7f456…2416. **Not yet device-tested.**

## Device test 15 — 2026-10-21 22:25 local, user's Quest 2 (build 4a1d8c2)
- Frame rate smooth with the mission-populated Story1 (300 objects + 9 characters): **DEVICE VERIFIED**.
- Rippers "feel alright"; navigation weird; missing the original ranged spit and burst deaths → fixed in the next build.

## Host verification — Ripper spit, bursts, steering (build 220c5ae6…)
- `gvr_tests` pass. New checks:
  - a medium Ripper spits and the ball bursts at the player;
  - small Rippers never spit;
  - a Ripper walks around a wall to reach the player;
  - the hit-point thresholds come from the type.
- Headless runs as logged in ENGINEERING_LOG. **Not yet device-tested.**

## Device test 16 — 2026-10-21 22:38 local, user's Quest 2 (build 74debe0)
- User: "It's fine now." Ripper steering, spit balls (orange/blue), burst/collapse deaths and data hit points are **DEVICE VERIFIED**.

## Host verification — mission objectives and music (build 50c05f59…)
- `gvr_tests` pass. New `testMusic`:
  - set parsing and set membership;
  - transitions into a variant song via its bridge segment;
  - chaining into a loop song;
  - gapless mixer queue;
  - raw decoding.
- Harness: `Music: 429 songs, 932 transitions`, 4 objectives, completion, soundtrack, jetpack (see log). **Not yet device-tested.**
- On the headset, music and soundtracks need the `Music` and `Stream` folders copied next to `Bin` (INSTALL.md).

## Device test 17 — 2026-10-21 23:07 local, user's Quest 2 (build 04c8f08)
- User: "End soundtrack plays and the jetpack works." Mission 1a completion, the outro soundtrack and the story jetpack
  are **DEVICE VERIFIED**.
- The user chose the cutscene presentation: a fixed viewpoint per shot (free head look), fades between shots, and
  subtitles from GTextEnglish.

## Host verification — cutscenes (build fad5912, APK sha256 826ac6b1…, delivered to the PC and checked)
- `gvr_tests` pass. New checks: GText parsing (incl. truncated input rejected), `Mat4::affineInverse`.
- Harness, contact sheets of every shot:
  - `story1_start_1…12` at mission start (62 s): lander, Baz and Gordon, flak, flags, subtitles.
  - `GVR_CUTSCENE=story1_jetpack_1` (39 s): Timmy, Baz, Rippers, props on bones.
- **Not yet device-tested.** To check on the headset (device test 18):
  - the opening cutscene starts when Story1 loads; the jetpack cutscene plays when 1a is complete;
  - comfort of the travelling viewpoint (the view moves with the shot camera, never rotates by itself);
  - subtitle readability; skipping with either trigger (release it first after the cutscene starts);
  - the soundtrack (needs `Stream` next to `Bin`) stays in sync with the pictures.
