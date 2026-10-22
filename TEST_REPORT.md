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

## Host verification — mission 1b, progression, sentries, piranhas (build 4b15f54, APK sha256 6ffa2ae0…, delivered and checked)
- `gvr_tests` pass. New `testRescue`: grab with the reach-down assist, carry to the pad, saved; dropped in the water →
  splash → back on the islet; out of reach → nothing grabbed.
- Harness:
  - `GVR_AUTO_RESCUE=1 GVR_MISSION=1b_1_catchsmarties`: 8/8 saved → complete → outro → 1c_1 loads.
  - First-person grab (`--grip-window`, `--hand-path`): picked up with the right hand.
  - 1c_2: the two Reaper sentries stand in the guard towers' crow's nests.
  - 1b water: piranha bites (health 100 → 16 in 4 s).
- **Not yet device-tested.** Device test 18 now also covers: 1b carrying (reach, grip, let go over the pad), the pad
  counting on the wrist pips, piranhas, the mission chain 1a → 1b → 1c, and free-camera X (next mission).

## Device test 18 — 2026-10-22, user's Quest 2 (build 4b15f54)
- **DEVICE VERIFIED:** mission progression.
- Reported issues, all addressed in a899d44 (see ENGINEERING_LOG):
  - cutscene voices and effects missing; misplaced bodies; the hatch didn't open; the flag was missing; Tel wasn't
    hanging;
  - the game felt stuck after cutscenes;
  - water and piranhas; Ripper nests; bridges sliding;
  - 1b Smarties standing instead of hanging;
  - the Meccaryn's missing face; the jetpack showing on his back in 1a.

## Host verification — build a899d44 (APK sha256 5cf1d305…, delivered and checked)
- `gvr_tests` pass (rescue test extended: hanging, slipping, letting go, dying, re-hanging, grabbing off the hang).
- Harness:
  - hatch opening frames;
  - visor face close-up;
  - both bridges crossed;
  - swimming at −1.35 m with the piranha attack (health 100 → 76);
  - nests hatch and are destroyed by gunfire;
  - the shadow term (debug view) and the shadowed render;
  - the briefing screen "Tel's Dilemma";
  - 8 Smarties hung on 9 hang points.
- **Not yet device-tested.**

## Device test 19 — 2026-10-22, user's Quest 2 (build a899d44)
- **DEVICE VERIFIED:**
  - frame rate unchanged with shadows;
  - cutscene audio;
  - loading screen;
  - visor face;
  - jetpack model swap;
  - bridges;
  - floating in water.
- **Reported (addressed in f5c0f47):**
  - shadows not sharp enough; colours washed out; water should reflect;
  - Timmy floating in cutscenes; subtitles should follow the head;
  - briefing on a button; Timmy on Baz's back;
  - no piranhas for Smarties in the water; no wading effects or sound; footsteps too quiet;
  - enemies notice too late; no gore or corpses; no death scene.

## Host verification — build f5c0f47 (APK sha256 8b0d9f76…)
- `gvr_tests`: all pass. AI tests were updated, and there is a new gore/Cleaner test.
- Harness renders:
  - reflections of pillars and hills (debug view of the reflection texture too);
  - clear shallows;
  - the shadow term;
  - Timmy on the back;
  - the briefing overlay;
  - a Reaper killed (pool), shot apart (chunks, splats);
  - Cleaners called, eating, the body eaten;
  - player death shot (lying in mc_die) and blast (chunks, pool), respawn after 5.5 s.
- Draw calls (1a start): eyes 350/305 plus the reflection 203, every other frame. On Quest the adaptive
  fallback logs `perf: ... quality down`.
- **Not device-tested.**

## Device test 20 — 2026-10-22, user's Quest 2 (build f5c0f47)
- **DEVICE VERIFIED:**
  - no frame drops with reflections and 2048 shadows;
  - better graphics;
  - blood particles on bodies; bodies eaten;
  - player death; held briefing.
- **Reported:** Timmy sideways and disappearing; too few Cleaners.
- **Fixed in acb8c9c:** harness views of Timmy from three angles, and a Cleaner swarm around a body. Tests pass. Not
  device-tested.

## Device test 21 — 2026-10-22, user's Quest 2 (build acb8c9c)
- **DEVICE VERIFIED** ("Works"): Timmy rides facing forward without flicker; Cleaner swarms.

## Host verification — build 36320d3 (APK sha256 418cdb07…)
- `gvr_tests`: all pass. New: destruction, jetpack rework, HUD gauge flood fill, player action (injection).
- Harness renders and logs:
  - guard tower collapse: shake, bend, dust, ruin swap; "The sentry falls from his tower", fall death on landing;
  - house cave-in; palm topple;
  - launcher held at its grip;
  - jetpack flight pose (mc_hover_turbo_back);
  - HUD in first person (forearm) and third person (lower left), weapon strip after a switch;
  - hit marker after `--hurt 10 70 90`;
  - FP injection with `--x-window` / `--lhand-path`: health 30 → 80, injections 2 → 1;
  - TP injection (mc_health_pup) heals; 1c pickup collected ("Picked up a health injection (1)").
- **Not device-tested:** everything above. Need from the next device test: the jetpack feel (thrust 22, air speed 10,
  fuel 5 s), fall damage levels, HUD size and legibility at the wrist, the push-into-chest threshold, and frame rate
  during a tower collapse.

## Device test 22 — 2026-10-22, user's Quest 2 (build 36320d3)
- **DEVICE VERIFIED:** health injections ("work flawlessly"); no frame drops (destruction, particles, HUD).
- **Reported:** no gradual burn on damaged trees/buildings; launcher not held by its handle; no rocket self-damage;
  Ripper hitboxes; blue spit too weak; forward flight pose reversed; landing clip on small drops, glitching;
  not enough fuel for 1b's broken bridges.

## Host verification — build 845cb14 (APK sha256 16d11cff…)
- `gvr_tests`: all pass (new: landing thresholds and one-shot clip, Ripper hit box, burn textures).
- Harness:
  - a Smartie house after 0/1/2 rocket blasts: char spreading; the palm next to it glowing with embers;
  - the launcher held at its wooden grip (FP);
  - forward flight lean;
  - self-damage at 1/2/5 m: 26/14/0;
  - a Ripper hit box of 1.3 × 1.46 × 1.9 m logged.
- **Not device-tested.**

## Device test 23 — 2026-10-22, user's Quest 2 (build 845cb14)
- **DEVICE VERIFIED:** the burning/char damage look; the flight animation.
- **Reported:** too much fuel; Timmy faces the wrong way, disappears in flight, wrong/buggy riding clip.
- **Fixed in d07dbb9** (APK sha256 639e313b…): fuel 8 s; Timmy facing backwards on sm_fly_idle (+ yells in flight),
  mount kept in clips without reference nodes. Tests pass; harness flight frames show him throughout. Not device-tested.

## Device test 24 — 2026-10-22, user's Quest 2 (build d07dbb9)
- **DEVICE VERIFIED:** Timmy on the back facing backwards, no longer disappearing ("better", "fine").
- **Reported:** Timmy doesn't lean with Baz.

## Host verification — build f811216 (APK sha256 80d24f79…)
- `gvr_tests`: all pass.
- Harness:
  - Timmy lean trace (0° idle, 72° in forward flight) and renders;
  - Timmy lines "I've hopped on!" and "Do that jetpack thing again!" with the subtitle panel;
  - "Oh no, they are attacking us!" after `--hurt`;
  - terrain before/after at the 1a and 1b starts and from the air (beach band, earth patches);
  - debug renders of the rock/sand/patch weights.
- **Not device-tested;** terrain frame cost unmeasured.
