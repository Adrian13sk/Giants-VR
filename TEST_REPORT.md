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

## Host verification — build 5b56bea (APK sha256 d6a9cb99…)
- `gvr_tests`: all pass (new: villagers).
- Harness:
  - 1d completed at the hatch → outro → Story2 loaded, 2a at its start marker;
  - Story2/3/5 missions found by file name;
  - 1a's Dactyls circling and banking;
  - 1a village Smarties (8) and the hatch village (6).
- **Not device-tested.**

## Host verification — build 0a8b1c0 (APK sha256 5d85a67c…)
- `gvr_tests`: all pass (new: save game and settings round trips).
- Harness:
  - checkpoint written at start;
  - resumed from hand-written 1c_2 and Story2/2b_1 checkpoints (injections and launcher restored);
  - options menu opened with `--menu-at` and navigated with the stick.
- **Not device-tested.**

## Host verification — build 8626e3c (APK sha256 7a584013…)
- `gvr_tests`: all pass.
- Harness: holstered gun and launcher (TP front and back views); the frame-rate counter and large HUD from
  settings.txt. Rumble is not observable on the host. **Not device-tested.**

## Host verification — build 2d57afb (APK sha256 7feac221…)
- `gvr_tests`: all pass.
- Harness: day, dusk, night (stars, moonlight) and dawn renders of 1a's start. **Not device-tested.**

## Host verification — build 93a8eed (APK sha256 0da55f65…)
- `gvr_tests`: all pass.
- Harness: Delphi in Rstory1 (TP run, jump trace, swipe; FP sword); the options menu starting the Reaper story from
  Story1. **Not device-tested.**

## Host verification — build 866dfdc (APK sha256 a5e4426a…)
- `gvr_tests`: all pass.
- **Harness, Kstory1 as Kabuto:**
  - FP and TP views: HUD and controllers at normal size, Kabuto 17 m tall over the village.
  - Walk: 8.7 m/s measured.
  - Grab-and-eat of a Smartie: "Kabuto ate something".
  - TP punch: destroyed a tree.
  - Stomp: quake plus destroyed tree.
  - FP ground pound: 11.7 m/s hand, quake.
  - Trampling a Smartie house.
  - Death: kb_die.
  - Audio: 163/163 sounds.
- **Regression:** Story1 Mecc FP and Rstory1 Delphi TP renders unchanged.
- **Not device-tested.**

## Host verification — build 42ac61a (APK sha256 219ffef8…)
- `gvr_tests`: all pass. Story1 start render byte-identical to the previous build.
- **Harness `--main-menu`:**
  - Title scene with logo, menu and art card.
  - CONTINUE into Story1 / 1b_1_catchsmarties (syringes 2 restored, checkpoint rewritten).
  - CONTINUE into Kstory1, playing as Kabuto after the loading screen.
  - NEW GAME → Rstory1 after the 1.5 s hold (checkpoint replaced).
  - Loading screens: Mecc art with "The Wax Smarties" briefing, Delphi art with "Yan, the Samurai Smartie".
- **Not verified:** menu music (segments not in the cloud copy).
- **Not device-tested.**

## Host verification — world streams (B5)
- Python walker: 33/33 w_*.bin streams end on 0xFF with the new sizes.
- `gvr_tests`: all pass, including a new world-bin case with ops 50 / 35 / 44 / 110 followed by an object.
- **Harness:**
  - Every story world loads with `complete=1`.
  - Chapter end Story5 → rstory1 (Delphi).
  - Story1 render byte-identical.
- **Not device-tested.**

## Host verification — Vimps and Mecc objectives
- `gvr_tests`: all pass (new: Vimp herds).
- **Harness:**
  - Objective count printed for all 22 Mecc missions: none at 0.
  - 2a → 2b by meat delivery.
  - story3_prison_7 → Story 4 by destroying the prison.
  - 5a complete by killing the Charger.
  - Vimp herd render (grazing) and blast (3 dead, lying).
  - Story1 render byte-identical.
- **Not device-tested.**

## Host verification — Reaper/Kabuto targets and the Reaperski (builds 77eef3a, 049ffc4)
- `gvr_tests`: all pass (new: race and jetski handling).
- **Harness:**
  - All four Reaperski races won by the autopilot with no gate missed; race 1 chains on to race 2.
  - kstory5_4: the player starts at his own Kabuto, the rival appears at marker 3 and approaches.
  - kstory5_5: Meccaryn vs Kabuto (2500 hp).
  - rstory2_jetski_2 / _3: moored ski placed.
  - Story1 render byte-identical.
- **Not device-tested.**

## Host verification — bosses, Sonaks, SAMs (build 6e3a8cb)
- `gvr_tests`: all pass.
- **Harness:**
  - Offspring, Charger, rival Kabuto and Sonak attack a standing player.
  - A SAM missile hits a hovering player.
  - Story1 render byte-identical.
- **Not device-tested.**

## Host verification — the 16:45 list (build 8515055)
- `gvr_tests`: all pass (new: burrowing - buried can't be hit, tunnels and emerges biting within 10 s, large tunnels
  slower, stays put far away, stops at the shore, small ones latch and jump off; dry jetpack held lands soft, released
  lands hard).
- **Harness (Story1):**
  - 1a: buried Rippers tunnel (dust plume) and burst out 2.6 m from the player; holes and orange spit drops left.
  - 1a nests: mini Rippers hatch, latch on (t 11.4 s) and jump off (15.4 s).
  - 1a: Timmy animates on the tower deck; tags show over a looked-at Ripper.
  - 1b: wading spray, shot splash with ring, piranha frenzy; the jetpack plasma and ground light in both views.
  - 1d: entering the shop opens the counter; standing in the pool heals 40 → 100.
  - VR map opens with Y (fogged and all-explored renders).
  - Smoke runs Story1, Rstory1, Kstory1, Story2 (300 frames, firing, Y): no errors.
- **APK:** build 8515055, 3854835 bytes, sha256 e8e9dcf6d3ae3f47….
- **Not device-tested.**

## Host verification — flares, bush pack, Kabuto growth (builds 41f2a89, 14386c1, 3886e95)
- `gvr_tests`: all pass.
- **Harness:**
  - 2b, hovering by a SAM: missile bursts 2.7 m from the player (health 96); with a flare thrown it bursts 5.9 m away
    (health 100).
  - 2b, Reaper at 10 m: no bush, shot (health 94); in the bush standing still, not noticed in 10 s (health 100); walking
    past in the bush, the Reaper goes to poke it and finds nothing.
  - Kstory1 (k1a): Kabuto starts at 60 %; render at 60 % and 100 %.
- **Not device-tested.**

## Host verification — first-person bodies, mission sweep, CPU pass 2 (builds 07bbbc8, 55ef4a7, this build)
- `gvr_tests`: all pass.
- Delphi and Kabuto render in first person with their bodies; both arms reach the controllers (Rstory1, Kstory1); the
  Meccaryn's first person is unchanged.
- `scripts/mission_sweep.sh`: all 56 missions run 500 frames (fire, jetpack, map, injection, flare) with exit 0 and no
  errors. Mean update time over the missions 0.34 → 0.27 ms after Reaper animation LOD and throttled line-of-sight
  (2b_2 1.96 → 0.66 ms, kstory4_1 1.20 → 0.66 ms; host x86, relative).
- Mesa's host GL reports GL_OVR_multiview2, so a one-pass stereo renderer could be checked on the host.
- **Not device-tested.**
- Delphi's spells (build d6ef359): rstory2_battle_1 - a tornado destroys the army barracks (objective done, mission
  complete); fire wall renders and burns; r1_smartiegrab - the bubble catches the Smartie 20 m away and sets it down by
  Delphi (third person; the harness's first-person controller pose points elsewhere).

## Host verification — Kabuto's powers and magnifying glass (builds 31f0064, this build)
- `gvr_tests`: all pass.
- kstory3_3: 20 rocket blasts on mag_crystal destroy it; Kabuto walking forward takes the glass. 40 blasts on
  giantwall4 destroy nothing; with the glass (GVR_GLASS=1) the beam destroys it from the west, east and north in about
  3 s and the mission completes. Rendered first and third person (light on the gate, scorch smoke).
- kstory4_1: the crystal and the gate are found; same code path.
- Push-out fix: Kabuto spawned against kstory3's gate stays within 1 m over 60 frames (was flung 217 m).
- k1a gates and kstory4_1 Offspring: see ENGINEERING_LOG.
- **Not device-tested.**
- Sweep (this build): all 56 missions exit 0, no errors; mean update 0.29 ms (run alongside an APK build).

## Host verification — Kabuto's throws (this build)
- `gvr_tests`: all pass.
- kstory3_2: the pillar of volcanic rock (k_rock_gen) at game (-1217, -819) carries a k_rock_gen_top boulder; a blast at
  its top knocks it off; X held picks it up, release throws it (third person, auto-aimed). GVR_BOULDER=1 by the army
  barracks: the boulder bursts on it at 77 m, 600 → 300 hp.
- kstory4_1 (GVR_REAPER_NEAR=1): X held 0.7 s then released throws Reaper 2 into Reaper 3, both die (18 → 16 alive);
  first person with a 0.67 m scripted left-hand swing does the same; a 4-frame X tap eats the Reaper.
- Rendered: the boulder seated on the pillar, carried in Kabuto's first-person hand, in third-person flight.
- **Not device-tested.**
- Sweep: all 56 missions exit 0, no errors (mean update 0.32 ms, partly run alongside an APK build).

## Host verification — on-device test mode (this build)
- `gvr_tests`: all pass, including the new test-script parser test (timeline, hand paths, events, eye poses, errors).
- Harness `--script` dry runs of the five device tests: exit 0, no errors; perf_1d_tower destroys guard_tower2;
  GVR_GOD keeps the perf runs from ending in a respawn.
- The APK contains the autotest code (log strings present in libgiantsvr.so). devtest.ps1 not executed (no
  PowerShell here); **nothing device-tested**.

## Automated device run 1 — 2026-10-24, user's Quest 2, build da6a2a4, run by devtest.ps1
First run of the on-device test mode: the user ran `devtest.ps1`; nobody wore the headset. **DEVICE VERIFIED:** the
test mode itself (scripted input and eye poses, the session runs unattended with the proximity override, screenshots
upright and correct, results pulled). 5/5 tests done, 0 errors logged, no crashes.

| Test (perf window 4 s - end) | fps | frame ms p50 / p95 / p99 | slow frames | update ms | render CPU ms | draws |
|---|---|---|---|---|---|---|
| 1a village, turning | 68.7 | 13.9 / 15.0 / 26.2 | 27 | 0.61 | 3.99 | 1167 |
| 1d guard tower collapse | 62.4 | 14.0 / 25.9 / 27.9 | 137 | 0.71 | 2.83 | 720 |
| 1c jetpack flight | 66.4 | 14.1 / 20.1 / 22.7 | 52 | 0.52 | 2.41 | 624 |
| Kabuto k1a, walking | 65.6 | 14.0 / 22.5 / 23.2 | 90 | 0.74 | 2.19 | 497 |
| Delphi r1, walking | 71.1 | 14.0 / 14.1 / 14.3 | 4 | 0.37 | 0.90 | 167 |

- These include the screenshots' read-back stalls (~170 ms each, 2-3 per test); taken out from the next build on.
- Per second: the village holds 71-72 once loaded; the tower scene runs ~45-55 fps from 4 s to 12 s (Rippers close to
  the camera, then the collapse); flight drops to ~45-60 fps at height; Kabuto walking ~54 fps, standing 72.
- The system's VrApi line in the village (last seconds): FPS 70-71/72, App GPU 11.8-12.3 ms of the 13.9 ms frame,
  GPU load 0.91-0.93, GPU at its top clock level (4), CPU level 2. **The game is GPU-bound; CPU is far from the limit**
  (update < 1 ms, render submission 1-4 ms).
- Memory: 223-237 MB resident (peak 255 MB).
- The app's GPU timer queries returned garbage on the Quest's driver (now filtered); the runner reads VrApi instead.
- The phone log buffer had been overwritten by system chatter (only the last seconds kept); the runner now sets 16 MB.

## Automated device run 2 (GPU experiments) — 2026-10-24, user's Quest 2, build 07e197c, `devtest.ps1 -Set experiments`
15/15 done, 0 errors. fps and frame time from the app (perf window 4-22 s, screenshot stalls left out); App GPU time,
GPU load and GPU clock level from the system's VrApi lines (18-19 per run). FFR = fixed foveated rendering.

| Scene / setting | fps | frame p95 ms | slow frames | App GPU ms | GPU load |
|---|---|---|---|---|---|
| Kabuto k1a walk, FFR off | 67.7 | 22.4 | 72 | 12.88 | 0.93 |
| ... FFR low | 71.8 | 14.2 | 0 | 11.85 | 0.94 |
| ... FFR medium | 71.8 | 14.2 | 0 | 11.05 | 0.87 |
| ... FFR high | 71.8 | 14.2 | 0 | 10.32 | 0.82 |
| ... shadows off | 69.7 | 15.4 | 8 | 11.49 | 0.89 |
| ... shadow map 1024 | 67.8 | 22.5 | 69 | 12.68 | 0.93 |
| ... reflections off | 69.4 | 15.7 | 4 | 12.59 | 0.95 |
| 1a village, FFR off | 70.8 | 15.0 | 9 | 12.14 | 0.95 |
| ... FFR medium | 71.8 | 14.2 | 0 | 10.72 | 0.83 |
| ... FFR high | 71.8 | 14.2 | 0 | 9.86 | 0.80 |
| ... shadows off | 71.8 | 14.2 | 0 | 10.30 | 0.83 |
| ... reflections off | 71.0 | 14.5 | 3 | 11.99 | 0.92 |
| 1c flight, FFR off | 68.5 | 16.1 | 46 | 12.82 | 0.91 |
| ... FFR medium | 71.7 | 14.1 | 1 | 10.58 | 0.83 |
| ... reflections off | 69.7 | 15.5 | 7 | 12.22 | 0.92 |

- **Conclusions:** foveation is the big win (medium: -1.4 to -2.2 ms GPU, 72 fps everywhere measured). The shadow pass
  costs 1.4-1.8 ms but not because of its resolution (1024 = 2048): it's the extra draw of the scene. Reflections cost
  ~0.2-0.6 ms. Decision: FFR medium by default, selectable in Options.
- Also seen: the first 3-4 s after a mission loads run 33-45 fps with App GPU up to 23 ms (warm-up: uploads / first
  use). Not yet investigated.
- With the plausibility filter the app's own GPU timer gives believable numbers on some frames (e.g. 8-13 ms), but it
  times only part of the frame; VrApi's App time is the reference.

## Host verification — warm-up, shadow LOD, Kabuto FP eye (builds 26301db-aa9d266 and this build)
- `gvr_tests`: all pass (settings round-trip now covers `foveation`; the test-script parser accepts `briefing`).
- Briefing frame renders the briefing (the warm-up view is cleared behind it); the warm-up runs once per frame.
- Village render with the 1-tap far shadows: shadows present, no visible change at the harness resolution.
- Kabuto first person with the right hand pushed 50 cm forward: eye offsets 0.2 / 1.2 / 2.0 m compared; 2.0 adopted.
  Delphi and the Meccaryn checked in the same pose: arms normal.
- Dry run of devtest/tests/warmup_k1a.txt: reload through the loading screen and briefing, play resumes.
- **Not device-tested.**

## Host verification — cutscene fades and show/hide, adaptive quality (builds aeb1645 and this build)
- `gvr_tests`: all pass, incl. the new flick show/hide test.
- Harness traces: story1_start_1 fades in from black over 2.0 s (op 0x1F); story1_start_12 holds to 12.67 s while it
  fades to black, and play fades in from there; story1_jetpack_6 renders one Timmy before frame 61 (standing) and one
  after (on Baz's back).
- Host frame pacing is fixed (1/72 s), so the adaptive-quality change shows no messages there (as intended).
- **Not device-tested.**
- Sweep after the cutscene / quality changes: 56/56 exit 0, no errors.

## Host verification — all cutscene shots, cutscene effects, carried props (build 97241e5, APK dcf9c896…)
- `gzptool flick` over all 545 flk shots: 545 parse with a director, 0 failures.
- `gvr_tests`: all pass, incl. the new op 0x27 parser test.
- Harness, Rstory1_fire_4a at 7.6 s: 4 effects fired at the weapons' ref_nozzle (log `cut-fx`), flame/smoke visible at
  the spear tips; spears and bazookas now in the guards' hands (before: floating upside down above their heads).
- Harness, story1_jetpack_6: Baz's gun still in his hand (skinned-prop path unchanged).
- Harness, every mission's opening cutscene (33 missions, 360 frames each): all exit 0, no `[E]` lines;
  kstory1b (k1b_portal) absent from the data and skipped.
- Mission sweep not re-run for this change (it plays with GVR_CUTSCENE=none, so cutscene code isn't exercised).
- **Not device-tested.**
