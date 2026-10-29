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

## Host verification — base building v1 (5c_securebase)
- `gvr_tests` pass. Harness 5c with `GVR_BASE_ORDER=all GVR_GOD=1`: 14 sites from Mecc_base_template_1; Pub 28% and
  Workshop 25% after 5 s (two crews); "WORKSHOP - COMPLETED!" on screen at ~20 s; aerial view shows the gate and walls
  rising on the basin's rim. Plain 5c, 4a and r4a runs: exit 0, no errors.
- The hand-touch menu is untested (the harness has no hand path to the signs yet). **Not device-tested.**
- v1.1 (harness): hand path to the WORKSHOP sign orders exactly one building; collision boxes enclose their sites;
  50 s run reaches "Base: complete" and opens the Giftshop's counter. Not device-tested.

## Host verification — pop-up bomb
- story3_prison_7: bomb set by the gates -> prison destroyed, mission complete. 40 rocket blasts -> prison intact.
- Shop lists the extra bomb (4 items). 5c: base completion gives the bomb. story3_bunker_3: no errors.
- Holding/placing with real controllers untested. **Not device-tested.**

## Host verification — Delphi's base (r4a)
- r4a, everything ordered: 10 sites from RP_base_template_1, Persuader/Spell School/Spellshop/Party House solid and
  completed, "Base: complete"; render shows the Reaper buildings round a flat clearing. Vimp herds spawn on Rstory4.
- r4a, 5c, rstory2_battle_1, k1a plain runs: exit 0, no errors. Tornado gating by the base: logic only, untested.
  **Not device-tested.**

## Host verification — base raids
- 5c and r4a with everything ordered and GVR_RAID_SOON: raiders spawn, march, wreck a building (destroyed), the next
  raid follows; Smartie lines shown. Rebuilding a wrecked building: logic only, untested. **Not device-tested.**

## Host verification — base upgrades
- 5c and r4a with GVR_BASE_UPGRADE=1: every upgradable kind rebuilt as its second-level model and completed (log);
  no errors. Hand-touch of an UPGRADE sign: same code path as ordering, untested by hand path. **Not device-tested.**

## Regression sweep after base building, pop-up bomb, raids, upgrades (build ce5af02)
- `scripts/mission_sweep.sh`: 56/56 exit 0, 0 errors, mean update 0.27 ms (host).
- Raids along army paths (5c, 14000 frames): raiders follow the paths, perimeter wall destroyed, raids continue.

## Host verification — base workforce
- 5c: crew of 3 created, hatch opened by walking up (3 Smarties out), workers walk to and leave the Pub site, sites
  without workers show AWAITING MORE SMARTIES (logic). Not device-tested.

## Host verification — base leader / kidnapping
- 5c: leader placed at the template's borjoyzee node; snatcher kidnaps him and carries him back along its army path.
- Loss -> restart (22000-frame harness run): kidnapped -> carried off -> mission failed -> restarting -> Loading
  5c_securebase -> loaded. Freeing him by killing the carrier: logic only. **Not device-tested.**

## Host verification — outros and HUD
- Outro cutscenes: 20/20 play (harness, 300 frames each), no errors; 36 missions name none.
- Pop-up bomb strip cell: rendered with icon and count (story3_prison_7). Not device-tested.

## Host verification — Reaper battle bases
- rstory2_battle_1 / rstory5_battle_2: base, crew (3 + 5 hatches), Yan, army paths (7 / 12 lead to the base), raids
  spawn; renders checked. 5c and r4a placements unchanged. Not device-tested.

## Host verification — Meccaryn arsenal (weapondefs.bin)
- Unit tests: weapondefs parser on a synthetic file (records, flags, 4-value entries, strings, item runs, bad version);
  save game round trip with guns and ammo. `build/host/gvr_tests`: all passed.
- `gzptool weapons <Bin>`: 53 weapons, 73 items parsed from the user's file.
- Harness, story3_bunker_3 with a Reaper spawned ahead (`--spawn-reaper`, `--auto-aim`, `--trigger-pulse`):
  sniper at 40 m - 1 hit, dead; shotgun at 15 m - 1 direct shell, dead; machine gun at 15 m - dead after 8 hits;
  handgun - 5 hits. 4a: mortar bomb lobbed at a Reaper 20 m ahead - dead.
- 1c_1: shotgun granted from item 3; the three mc_pickup_shells are ammo pickups ("Picked up 2 rounds for the shotgun
  (27)"), no longer health pickups. First-person renders of the shotgun, machine gun and sniper gun in hand checked.
- Sweeps (`scripts/mission_sweep.sh`): plain 56/56 exit 0, 0 errors; with every gun and the machine gun firing
  (GVR_ARSENAL=1 GVR_WEAPON=5) 56/56 exit 0, 0 errors.
- Mortar (4a, a Reaper 20 m ahead, auto-aim): dead from the first bomb. Sweep firing the mortar everywhere
  (GVR_ARSENAL=1 GVR_WEAPON=7): 56/56 exit 0, 0 errors; bombs lobbed in the 25 Meccaryn missions.
- Grenades (5a): hand swing (`--hand-path` 0.85 m in 10 frames while the trigger is held) throws at 9.8 m/s; lobbed
  grenade bursts 0.6 m from a Reaper 12 m ahead and kills it. Held grenade visible in the first-person hand.
- Sniper zoom (story3_3sonaks_4, GVR_SCOPE=1 forcing it on): the side screen shows the magnified horizon upright
  with the red cross; the pass draws 57 calls; from the hip (no hook) it never renders.
- Sweeps: grenades thrown everywhere (GVR_ARSENAL=1 GVR_WEAPON=8) 56/56 exit 0, 0 errors; plain sweep after the
  grenade change 56/56 exit 0, 0 errors.
- Sniper sweep with the zoom forced on (GVR_ARSENAL=1 GVR_WEAPON=6 GVR_SCOPE=1): 56/56 exit 0, 0 errors.
- Shop ammo (1d): the counter lays out 4 goods (launcher, injection, flares, AMMO); taking the AMMO box was not
  reached by the harness's walk (logic only).
- **Not device-tested.** Sound names (ME_RSGSH, ME_SPRSH, ME_MCH*, ME_MTR*, ME_GRN*) are guesses from the file names.

## Host verification — Delphi's bows
- Unit test: save round trip with bows and arrows (unlimited = -1). `build/host/gvr_tests`: all passed.
- r4a, trigger draw with auto-aim at a spawned Reaper: bow 2/2 hits (killed), RPG bow killed at 25 m with one arrow,
  sniper bow killed at 25 m with one hit. Hand draw (new harness input `--lgrip-window`): the left hand moved to the
  string (0.09 m), gripped, the bow hand pushed 0.5 m forward -> "Arrow from the BOW at 76% draw", shot from the hand
  through the riser. Sweep with every bow and the RPG bow drawn (GVR_BOWS=1 GVR_DELPHI_WEAPON=1): 56/56 exit 0,
  0 errors, arrows loosed in the 11 Delphi missions. **Not device-tested.**
- Plain sweeps: on a317ed1 (before the bows) and with the bows: 56/56 exit 0, 0 errors each.

## Sanitizer sweep (AddressSanitizer + UBSan host build, `build/asan`)
- `gvr_tests` under ASan/UBSan: all passed.
- All 56 missions, 500 frames each, with every gun and bow carried (GVR_ARSENAL=1 GVR_BOWS=1), firing the shotgun as
  Baz and the sniper bow as Delphi: 56/56 exit 0, no AddressSanitizer or UBSan reports.
- Sniper zoom pass under ASan (story3_3sonaks_4, 200 frames, firing): clean.

## Host verification — review fixes and fx.bin playback
- After the review fixes: 1c shell pickup (+2), sniper / machine gun / shotgun kills at 40 / 15 / 15 m, sniper bow kill
  at 25 m - unchanged results.
- fx.bin playback (GVR_DATA_FX=1): cutscene Rstory1_fire_4a's effects resolve to fx.bin entries ("Effect
  fx_flick_millenium_spear (fx.bin)"); a rocket explosion renders flash, glow and debris; ASan/UBSan run in 4a with
  the mortar and a rocket explosion every 3 s: clean. **Looks unverified against the original.**
- Sweeps on fb0b21d: with the original effects forced on (GVR_DATA_FX=1, a grenade explosion every 3 s in front of
  the hero, rockets fired): 56/56 exit 0, 0 errors (effects played in 52); plain: 56/56 exit 0, 0 errors.

## Host verification — launcher rockets, mines, homing / power-up bows
- Save round trip extended (rockets, mines, bows 3-4): passed.
- Launcher with 12 rockets fired 31 times: exactly 12 rockets left the tube. Mine: set, armed, set off by a Reaper
  3 m ahead (dead); rendered at the hero's feet with its red light. Homing bow 12 degrees off a Reaper 20 m away:
  locked, struck, dead. Power-up bow at 12 m: dead.
- Sweeps: plain 56/56, every weapon carried with mines set everywhere (GVR_WEAPON=9) 56/56 (mines set in the 25
  Meccaryn missions), launcher + homing bow 56/56 - all exit 0, 0 errors. AddressSanitizer/UBSan runs of the mine
  and homing-bow tests: clean. **Not device-tested.**

## Build d295fa1 (APK) — headset-free verification, 2026-10-24
- APK: `scripts/build_apk.sh`, 0 compiler warnings; zipalign and APK signature v3 verified; 4.04 MiB,
  sha256 d47154085fd2b84d9ad44aa1ed3df7c6a2d8dc2505cf6396e0c4244ff64ea881. libgiantsvr.so: ELF64 AArch64, exports
  ANativeActivity_onCreate, needs only system libraries + libopenxr_loader; both .so stored and 4 KiB-aligned;
  manifest io.github.giantsvr.dev, minSdk 29 / target 32, OpenXR permissions.
- Delivered to Documents\GiantsQuestVR (GiantsVR-dev.apk and GiantsVR-dev-d295fa1.apk, giants-vr.gitbundle,
  INSTALL.md, PLAYTEST.md, three new devtest scripts); all read back from the PC with matching sha256.
- Unit tests (host and ASan/UBSan builds): all passed.
- Campaign soak in one process (every gun and bow carried, firing, a mission completed every 2000 frames): 1a -> end,
  54 missions, 17 world switches, 0 errors; resident memory 183 -> 454 MB (B15, as before).
- Sweeps of all 56 missions: plain, mines everywhere, launcher + homing bow - 56/56 each (code of a7f8998; d295fa1
  only renames a log line). ASan/UBSan sweep with every weapon, mines, the homing bow and the original effects on:
  56/56, no sanitizer reports.
- New device tests (devtest\tests): weapons_mecc (sniper zoom frame cost, mortar, grenade, mine), delphi_bows
  (homing, power-up), fx_original (frame cost of fx.bin explosions). Dry-run in the harness: all ran clean, every
  scripted shot / throw / switch happened. **They need the Quest on USB and devtest.ps1 run on the PC.**
- Nothing in this build has run on the headset yet.

## Branch `next` (after d295fa1) — host verification, 2026-10-24 evening
Not built into an APK; nothing here is device verified.
- `build/host/gvr_tests`: all pass, incl. the new FxPlayer attach test (synthetic fx.bin: attach, child riding,
  streak, detach, auto-end after 0.5 s unmoved).
- `tools/py/review_results.py` on the two real PC runs (20261024-121735, 20261024-123302) and a synthetic later run:
  tables, worst frames, both cost tables, history comparison and the "did not finish" flag all produced.
- Targeted harness set (`asan_targeted.sh`, plain build): all 16 Kabuto missions first person (grab / eat, a fist
  swing, Y held for an egg, a snap turn, a stomp) and third person; kstory5_4 -> kstory5_5 chain; the four races and
  two Reaperski missions; trails on launcher, shotgun and Millenium mortar shots and on Delphi's RPG / homing /
  power-up arrows; Delphi's mine. 46 runs: all exit 0, 0 errors. Trails logged for kinds 0, 3, 4, 5, 7, 8.
- Snap turn with still hands: no fist blow. A 0.9 m swing in 3 frames: one blow at 21.6 m/s.
- Device-test scripts dry-run in the harness: fx_trails (4 rockets with trails), delphi_mine (set, screenshots).
- Same 46-run set under AddressSanitizer + UBSan (`build/asan`): all exit 0, 0 errors, 0 sanitizer reports; ASan
  unit tests pass. (A first attempt crashed in `initShop` on every mission with a shop: a stale object file from an
  interrupted build had the old `GameApp` layout. After touching `game_app.h` and rebuilding, clean. Not a code bug.)
- Whole-campaign sweep with the branch build (56 missions, 500 frames each with firing, jetpack, map, injection,
  flare): all exit 0, 0 errors; 4 warnings, all the known harness one ("w_Rstory3.bin not found": the sweep starts
  the Rstory3 race missions without their race world).

## Multiplayer (branch `next`) — host verification, 2026-10-25
Not built into a delivered APK; nothing here is device verified. Online play is verified only between two copies on
one machine (one Quest available).
- Unit tests (host and ASan/UBSan): the skirmish rules (spawns from markers, capture, drop on death, friendly fire
  off, a played-out game reaching a winner) and the network (host + joiner over loopback: join, mirror positions,
  hits applied once, damage reaching the joiner, its death counted as a kill and heard by it). All pass.
- All 12 multiplayer maps, 200 s each against computer players (harness): exit 0, 0 errors; kills on every map,
  flags taken on most; captures on M_3WAY_L1 (the Kabuto bot twice). Update 0.4-0.7 ms average.
- Under ASan/UBSan: the 12 maps (90 s, firing), the 3-way map as the Sea Reaper and as Kabuto: all exit 0, 0 errors,
  0 sanitizer reports. Host + joiner (two processes, real-time pacing) under ASan: joined, 0 reports on either side.
- Two harnesses paced in real time (GVR_REALTIME=1): positions agree within 1 m; the host's bot killed the joiner
  and both logged it; joining through the main menu (found by probe, map loaded); a joiner quitting times out.
- Whole-campaign sweep with the multiplayer branch (56 missions): 56/56 exit 0, 0 errors.
- Android build compiles (no warnings); not delivered.
- Device-test script `devtest/tests/skirmish_3way.txt` dry-run in the harness.

## Branch `next` fixes — host verification, 2026-10-25 night
- Unit tests (host): all pass, including new `testSkNav` (water exit, turned building footprint, blocked goal,
  learnt cells) and the extended `testSkNet` (an event backlog of 30 arrives complete and in order; a joiner leaves,
  its player is gone and not respawned, the next joiner takes its slot).
- Bot matches, 12 maps x 10 minutes (`GVR_SKIRMISH=0,3`, harness player idle): before 0 captures / 13 flags taken /
  166 kills; after (three runs) 11-19 captures / 61-70 taken / 201-210 kills, 0 errors.
- LAN on loopback (two harnesses, GVR_REALTIME=1): joined, the host's bot killed the joiner and both logged it; a host
  stopped and restarted 40 s later: the joiner logged "lost the host", knocked again and rejoined.
- Reaper ammo: GVR_AMMO_DROP_AHEAD=4 with GVR_ARSENAL=half in Story3: 4 drops rendered with their own models, walked
  through: rockets x2, sniper rounds, shells taken.
- ASan / UBSan build (`build/asan`): unit tests 0 failures, 0 reports; all 12 maps 90 s with firing (and 3-way L1 as
  teams 2 and 3): exit 0, 0 errors, 0 sanitizer reports; host + joiner under ASan: joined, 0 reports.
- Campaign sweep (plain build, 56 missions x 500 frames): 56/56 exit 0, 0 errors.
- APK e4c2818: `scripts/build_apk.sh` exit 0, 0 warnings; zipalign + v3 signature verified by the build.
- **Not verified:** anything on a headset; play between two headsets.
## Device run 20261025-043504 — APK e5a4c27 (d295fa1 game code + launch fix), Quest 2
- Started from `devtest.ps1` with nobody wearing the headset and no trigger press: all 9 tests `done`, 0 app errors
  logged, no system dialog reported. **The launch fix is DEVICE VERIFIED** (before it, the "Controllers required"
  dialog held every launch: LaunchCheckControllerRequiredDialogActivity in the 2026-10-24 logs).
- fps (frame p95 ms, GPU ms mean): delphi_bows 71.8 (14.2, 9.2); delphi_r1 71.8 (14.1, 8.0); fx_original 71.7 (14.2,
  12.2); kabuto_k1a 71.6 (14.3, 13.7); perf_1a_village 71.5 (14.3, 12.1); perf_1c_flight 71.6 (14.3, 13.2);
  perf_1d_tower 69.3 (16.3, 14.4); warmup_k1a 71.3 (14.3, 13.5); weapons_mecc 71.7 (14.2, 8.3). Memory 216-237 MB.
- Against 20261024-121735 (an older APK): kabuto_k1a 65.6 -> 71.6, perf_1c_flight 66.4 -> 71.6, perf_1d_tower
  62.4 -> 69.3, perf_1a_village 68.7 -> 71.5, delphi_r1 71.1 -> 71.8.
- perf_1d_tower: GPU 8-9 ms before the blasts, 15-17 ms from 7 s to the end with the same draw count. The "after"
  screenshot shows why: two Rippers right in front of the (god-mode, standing) player fill the view - the test
  measures Rippers at arm's length more than the collapse. Rippers that close cost ~6 ms of GPU.
- Original effects: +2.9 ms GPU in the 1.5 s after each blast (71.6 fps). Sniper zoom: its pass rendered (44 draws);
  the comparison window (mortar) was busier, so its cost isn't separated; no fps loss (71.7).

## Device run 20261025-045902 — APK 521cea6 (branch `next`: multiplayer, bot fixes, ammo drops, trails, Delphi's mine, launch fix), Quest 2
- 12/12 tests `done` with nobody wearing the headset; 0 app errors; one warning (RP_command_coll.gbs absent: the
  model's own triangles are used - expected). Nothing flagged by review_results.py.
- fps: delphi_bows 71.6, delphi_mine 71.7, delphi_r1 71.8, fx_original 71.7, fx_trails 71.6, kabuto_k1a 71.7,
  perf_1a_village 71.5, perf_1c_flight 71.6, perf_1d_tower 70.5, skirmish_3way 71.8, warmup_k1a 71.1,
  weapons_mecc 71.7. Against 043504 (d295fa1 code): equal or better everywhere; memory 205-238 MB.
- **skirmish_3way (DEVICE VERIFIED, bots only):** M_3WAY_L1 with 6 computer players and both standing bases:
  71.8 fps, frame p95 14.2 ms, GPU 8.8 ms (VrApi), update 0.84 ms mean / 5.1 ms max, render CPU 2.7 ms; the Reaper
  flag placed 18 m from its pad; Kabuto's bot took the Green flag within 30 s; bots routing (traces).
- **fx_trails:** the rockets carry FX_rocket from fx.bin (logged per rocket), 71.6 fps. **delphi_mine:** the mine in
  her hand and set down ("Mine set ... 4 left"), 71.7 fps. Screenshots not yet looked at by a person.
- Still unverified on a headset: play between two headsets; Reaper ammo drops (no test kills Reapers yet).

## Device run 20261025-103335 — APK 1634bb8, the two new tests, Quest 2
- **reaper_ammo: Reapers dropping ammo DEVICE VERIFIED.** Six Reapers spawned 8 m ahead, blasts at 5 s: 4 ammo drops
  (rockets x2, machine-gun clips, sniper rounds), walked over: an injection and 3 sniper rounds picked up. 0 errors.
  68.9 fps: the slow frames are at the start (load) and at 18-19 s, where the GPU took 21-22 ms - the walk ends in the
  sea after the blasts; the harness replay of that moment shows a red full-screen tint and large translucent sheets
  right at the camera (overdraw). Not yet traced.
- **perf_1d_tower** with the Rippers passive: 71.4 fps (was 70.5 with them in the face), GPU 11.8 ms mean; 7.5-8 ms
  before the blasts, ~12.5 ms from 7 s on.

## Device sweep 20261025-110318 — APK 1634bb8, 13 tests + all 56 missions (20 s each), Quest 2
- 69 runs `done`, **0 app errors in all 69**; no crashes, no timeouts. Every mission loads and plays on the headset.
- delphi_bows (the first run) drew at 1.6 fps: the headset was going to sleep as it launched (SLEEP transition);
  the launch check passed. devtest.ps1 now wakes the headset before each test and reruns one that hardly drew.
- Below 69 fps (GPU-bound, CPU 2-3 ms): kstory3_1 44.9, rstory2_jetski_3 56.6, rstory2_jetski_2 57.3,
  rstory2_battle_1 59.2, kstory3_2 64.6, kstory3_3 66.0, kstory3_4 66.0, kstory5_4 68.3, 2b_2 69.3. All other missions
  69-72. The system's App GPU time in kstory3_1: ~14 ms standing, 17-18 ms after moving (frames then halve to 36 fps);
  rstory2_jetski_2: 12.5-13.5 ms, 17-21 ms after moving. The adaptive fallback reached level 2 (no reflections).
- The app's own GPU timer read ~6 ms higher after each mid-run screenshot (t = 5 s in every sweep script; 7 s in the
  tower test); the system's App GPU didn't jump there. Its numbers after a screenshot are not trusted until explained.
- Next: GPU experiments x_k3_* / x_js_* (APK d90c8b1: the fallback also raises foveation one step).

## GPU experiments 20261025-114712 — APK d90c8b1, quality pinned, Quest 2 (system App GPU ms / fps)
- kstory3_1: base 16.5 / 39; FFR 3 13.5 / 64.5; no reflections 13.7 / 64.0; no shadows 13.6 / 63.3; quality 2 10.8 / 71.8.
- rstory2_jetski_2: base 12-14 standing, 19-23 racing / 56.4; FFR 3 61.1; no reflections 60.9; no shadows 61.4;
  quality 2 15-16 racing / 65.7. The racing frames: blood (piranha bites) and splash crowns at the eye.
- Fixes in 8b7371d (near-eye fade for blood and splashes; 2 s fallback windows); device check: `-Set slow`.

## Device run 20261025-115756 — APK 8b7371d, the eight slow missions (`-Set slow`), Quest 2
| Mission | before (1634bb8) | now | notes |
|---|---|---|---|
| kstory3_1 | 44.9 | 57.6 | 37.5 fps for the first 4 s while the fallback stepped 1-2-3; 71.7 fps from 15 s (App GPU 10.5 ms) |
| kstory3_2 / _3 / _4 | 64.6 / 66.0 / 66.0 | 69.2 / 70.5 / 70.6 | level 1 |
| kstory5_4 | 68.3 | 68.1 | never stepped down (68 fps is above the 65 fps trigger) |
| rstory2_battle_1 | 59.2 | 67.9 | level 2 |
| rstory2_jetski_2 / _3 | 57.3 / 56.6 | 66.0 / 66.3 | level 2; App GPU 13-15 ms, 10.7-11.9 at the end |
0 errors in all eight.

## Device run 20261025-123156 — APK 9ed3505, the eight slow missions, Quest 2
kstory3_1 71.4 (was 57.6; its first try hit a dozing headset and was rerun by devtest.ps1 automatically - the retry
works), kstory3_2/3/4 71.8, kstory5_4 70.6 (stepped down to level 1 at 69 fps), rstory2_battle_1 70.1,
rstory2_jetski_2 69.8, rstory2_jetski_3 69.9 (all three Rstory2 runs stepped from 1 to 2 when the action started:
Rstory2 now starts at 2). 0 errors. Kabuto 3 started at 2 and stepped back up to 1 without see-sawing.

## Harness, rework branch, 2026-10-25 evening (Mesa; nothing here is device-verified)
- Mission sweep (56 missions, 500 frames, firing/jetpack/map/injection/flare): 56/56 exit 0, 0 errors (after the
  grass; again at c31ae3d with grab, parry, sparks and the cutscene changes).
- Kabuto gates, scripted (`$S/kab_chain*.sh`: Kabuto at full size ~12 m from the gate, third person, punching;
  GVR_KB_ADREN=1 + both grips for giantwall3, GVR_GLASS=1 + trigger held for giantwall4): objective done in k1a,
  k1b (1/2: the gate), k2a, kstory3_1, kstory3_2, kstory3_3, kstory4_1, kstory4_2, kstory5_1, kstory5_2, kstory5_3.
  kstory5_4 (GVR_KB_FACE_BOSS=1): the boss killed in 17 hits, objective done. kstory5_5 (the Meccaryn against
  Kabuto; --auto-aim at his belly, handgun): killed after 34 hits, objective done.
- Kabuto hand grab (kstory4_1, GVR_REAPER_NEAR=1, scripted right hand): grabbed Reaper 2; brought to the mouth: eaten;
  swung and released: thrown at 55 m/s. Device tests kabuto_grab_eat / kabuto_grab_throw.
- Delphi parry (rstory2_battle_1, two Reapers, sword swung side to side): 3 bolts deflected, one returned and hit
  Reaper 0 at 8.9 m (GVR_HIT_TRACE). Device test delphi_parry.
- Cutscenes: contact sheets of all 30 opening cutscenes; rstory3_pre_race_1 (10 s black) and story3_prison_6's
  second shot (10 s black) now show their scenes.
- Unit tests: all passed.
- After Kabuto's climbing change (1c49eac): sweep 56/56 clean; the seven special gates (adrenaline / glass) all done again.
- Reach objectives (GVR_REACH_CHECK, every mission): all reachable from the start except 1a_1_timmy / mission1
  objective 3 (a box smaller than the 2 m grid, 2 m off: a grid artifact). Walked in play (GVR_REACH_WALK) and
  objective completed: k1b_portal, k2d_offspring, kstory4_3. kstory3_4: reachable by the fill only with the
  68-degree climb; the walker reached the island but not the portal within 30 000 frames.
- 7400b54: the texture doubling fix (blocky magnified ground in every rework APK before it) - unit test added
  (fails on the old code); sweep 56/56 clean. Also checked since 68003c5: shot knockback, near-miss flinch
  (GVR_AUTOAIM_MISS=3.5: "Reaper 1 flinches (bolt 0.8 m off)"), the 17 cutscenes not seen at mission starts
  (contact sheets; intro_island_1's flyover no longer swung), duplicate heartbeat removed.
- ce48a8b: unit tests pass incl. the new binaural test (a click to the right peaks louder and 10+ samples
  earlier in the right ear); harness audio render of a fight is sane (no clipping or NaN, RMS L/R 2347/3141);
  GVR_HEAD_DROP crouch -> "Kabuto slams the ground"; sweeps clean at 9e93e37, after the marksmen and at caeb29e.
- kstory3_4 (a8bc86e + the in-face hold): scripted two-hand climb from the sea at the plateau's foot (game -695, 3589):
  up to 110 m, then 6 s of walking -> "Objective 0 done", "Mission kstory3_4 complete". Kabuto's campaign is now
  completable end to end in the harness (gates, portals, bosses). Device test kabuto_climb_k34.
- Completion checks (scripts/completion/, this build): popup_prison7, popup_4a, popup_5c, tornado_rstory2,
  tornado_rstory5, tornado_rstory2_base, kill_4b_offspring, kill_5a_charger, kill_sonak1, kill_3sonaks4: each
  "Mission ... complete"; kill_r1_raik1/3 each their objective. Before the fixes: 5a stuck at 50 hp (point-blank
  misses), 3sonaks_4 at 14/15 (marker box). Sweep 56/56 clean, unit tests pass. **Not device-tested.**
- f69d09f (APK built, sha256 2684fc38..., copied to the PC and verified): sweep 56/56 clean with the new image stats
  (no white or blank frames), unit tests pass, completion scripts pass (popup scripts now walk away: health 100).
  Visual checks on contact sheets of all 56 missions and 14 worlds: fog/haze colours match the skies; r1_saveyan
  starts with Yan in front (was inside him); 2b_2's wife rides on the back. **Not device-tested.**
- e7e1721: APK built and copied to the PC (sha256 87477ac3... verified); devtest files kabuto_chest_beat, wave_smarties copied. Sweep 56/56 clean, unit tests pass. **Not device-tested.**
- 7f771c1: all 21 devtest scripts run in the harness: exit 0, no errors, images normal; kabuto_grab_eat eats, grab_throw throws (55 m/s), climb_k34 completes the mission, delphi_parry deflects. **Not device-tested.**
- 2026-10-26 (01be860, 3889d03 and after; APKs 01be860 and 3889d03 built and copied to the PC, sha256 verified):
  unit tests pass (new: ragdoll held by the hand / thrown / in water); all 12 completion checks reach their
  objectives; sweep 56/56 clean after every commit; all 34 devtest scripts exit 0 with no errors (warnings: three
  known base-placement / missing collision-model lines). New harness checks, each in its devtest script: body_grab,
  body_throw_hit, kabuto_body_eat, stone_throw, gun_grab, swim_strokes, smartie_throw_hit, ski_handlebars,
  reaper_ragdolls_water - each logs its expected lines (see ENGINEERING_LOG). Pebble refactor: the stones render
  pixel-identical (ImageMagick AE 0). **Not device-tested.**
- 2026-10-26, 3187f93 (after the review fixes and the water fallback): ASan/UBSan build - unit tests pass; all 39
  devtest scripts exit 0 with 0 sanitizer reports (every new hand mechanic exercised). Campaign soak in one process
  (GVR_GOD, firing, a mission completed every 2000 frames): 1a -> end, 54 missions, 0 errors; resident memory
  227 -> 572 MB (B15: the model cache is not emptied between worlds; +20-30 MB per new world, flat within one).
  Completion checks 12/12, sweep 56/56. **Not device-tested.**
- 2026-10-26, 8c464bb (ambient birds): unit tests pass; sweep 56/56 exit 0 E=0; ASan 0 reports on birds_takeoff,
  birds_shoot (2 birds shot down), birds_lobirds, stone_throw. APK built, sha256 b104c725... verified on the PC.
  **Not device-tested.**
- 2026-10-26, skinned meshes freed (B15): soak 1a -> r1_turbo (22 missions, 12000 frames) 0 errors, 229 -> 341 MB
  (was 229 -> 383 MB with 8c464bb); ASan 0 reports (soak 4000 frames + 4 ragdoll/dismember devtests); sweep 56/56.
- 2026-10-26, fish: fish_school and fish_catch pass in the harness (caught, thrown, flops on land); ASan 0 reports on
  fish_school, fish_catch, swim_strokes, reaper_ragdolls_water; sweep 56/56; unit tests pass. **Not device-tested.**
- 2026-10-26, ambience/weather: unit test testAmbience (6 loops) passes; weather_rain / weather_snow / weather_storm
  run clean ("weather light rain|snow|storm"); ASan with audio output on Rstory1, Rstory2, Kstory5, Story1: 0
  reports; sweep 56/56. Sound levels and the loops' character are **not heard by a person yet** (spectra only).
- 2026-10-26, review fixes + Smartie touch: fish_catch, birds_shoot, weather_storm, smartie_pat pass (harness and
  ASan, 0 reports); ASan soak Rstory1 -> 6 world loads with audio: 0 reports; sweep 56/56; unit tests pass.
- 2026-10-26, Kabuto catches Lobirds: kabuto_lobird - caught and eaten (harness and ASan, 0 reports); sweep 56/56.
- 2026-10-26, hitchers: devtest hitchers - "6 riding on Vimps", two take off and land again (harness and ASan, 0
  reports); sweep 56/56.
- 2026-10-26, sound loading fix + sea monster: birds_shoot trace - cr_caw2 / cr_deat2 now play (were MISSING);
  sea_monster - surfaces, rears, dives (harness; ASan with audio: 0 reports, se_first/roar1/firnd loaded on first
  use); sweep 56/56; unit tests pass.
- 2026-10-26, Delphi diving + lightning bolt: delphi_dive and weather_storm pass (harness; ASan 0 reports); sweep 56/56.

## Device run 20261026-204701 — comprehensive test, user's Quest 2 (APK fccc02d, 2026-10-26 20:47-21:33)
`devtest.ps1 -Apk ..\GiantsVR-rework-fccc02d.apk -Sweep`: install verified (sha256 4e858586...), all 60 test scripts
plus 56 mission sweeps (20 s each), unattended. Analysis scripts: per-test VrApi lines of the app vs other processes;
expected log events per script.

| Check | Result | Evidence |
|---|---|---|
| Stability | **PASS** 116/116 done, 0 crashes, 0 `E/GiantsVR` lines | summary.txt; every log ends "GiantsVR exited cleanly" |
| Frame rate | **FAIL** median 28 fps, 1 of 93 clean tests at 69+ fps | VrApi `FPS=` per test |
| GPU time (game's own) | **FAIL** median 26.1 ms (p10 18.2, p90 36.1, max 55.9) vs 13.9 ms budget | VrApi `App=` of the app's pid; B19 |
| Same scene, classic version (2026-10-25) | 9.9 ms, 71.6 fps (perf_1a_village) | device run 20261025-110318 |
| Since APK 4514eda (17:02 run) | 0-2 ms more (birds_takeoff 34.7 -> 35.1 ms, kabuto_lobird 25.2 -> 25.1, scorch_marks 31.3 -> 30.8) | same tests, both runs |
| Other app drawing during the test | 4 tests (kabuto_climb, hand_water, arrows_stick, birds_lobirds): vrshell 40-70 fps, 12-36 ms GPU | VrApi lines of pid 17530 (com.oculus.vrshell); B20 |
| Expected events seen (scripts with a Log list) | 19 of 26 | see the failures below |

Under budget on the headset: delphi_dive (8.9 ms, 70 fps - under the water the haze ends the view at ~25 m), sea_monster
(9.6 ms), hitchers (11.5 ms), skirmish_3way (13.4 ms). Slowest: Kabuto's storm (kstory5_1/4, weather_storm: 54-56 ms).

Mechanics confirmed on the headset by their log lines (scripted hands, no one wearing it): Kabuto's grab/eat/throw of
Reapers, tree uprooting, chest beat and roar, the kstory3_4 cliff climb to the portal ("Chapter complete: next world
kstory4", B18), Lobird catch; Delphi's tornado, fire wall, slash dismemberment, mine, bows, dive; body grab/throw/hit,
stones, Reaper ammo drops, ragdolls (also into water), swimming strokes, Reaperski handlebars, original effects and
trails, skirmish, Smartie pat/high five/fear/fish gift, hand water, hitchers, sea monster, plankton, palms shaking,
weather (rain, snow, storm), Lobirds circling.

Failures and what was done (fixes verified in the harness at 24 Hz - the headset's rate - and 72 Hz; new harness option
`--hz`):

| Test | On the headset | Cause | Fix |
|---|---|---|---|
| birds_shoot (also 17:02) | no bird ever hit | GAME BUG: birds checked bullets only when seen in flight; a 300 m/s round hits the ground 10 m off within a frame (at 24 fps always) | WeaponSystem::onSegment: every shot's path is tested; Flak hit radius 0.3 -> 0.4 m; the script sweeps the burst across the flock |
| fish_catch (also 17:02) | no catch | GAME BUG: a player standing still in the water scared fish within 3.5 m for good, so a still hand never drew them; + a 1.7 m/s hand entry counted as a slap and splashed them away | still player scares only 0.5 m round; slap needs 2.5 m/s; script eases the hand in and waits |
| kabuto_lobird (also 17:02) | caught, then dropped at 0 m/s | a grip pulse ended right after the catch | GAME: a Lobird is let go only after the grip is open (< 0.5) for 0.1 s (grip flicker); script pulses 0.24 s on / 0.06 s off |
| gun_grab | no pickup | GAME: a GVR_PASSIVE_REAPERS test target dodged the aim laser, so the gun fell elsewhere; and the script reached 0.75 m off | passive targets don't dodge; script reaches where the gun lies (trace of its position) |
| delphi_impale | no thrust | the scripted thrust (3.75 m/s) was barely over the 3 m/s threshold; a slow frame dropped it under | script thrusts at 5.5 m/s (passes at 18, 24, 72 Hz) |
| kabuto_smartie_throw | nothing in reach | the test relied on a Smartie wandering under his hand | test hook GVR_SMARTIE_AHEAD places one; GVR_STILL_SMARTIES |
| smartie_throw_hit | thrown past the Reaper | script threw 21 degrees to the left | straight throw: hits at 24 and 72 Hz |
| delphi_dive | "surfaces" missing | a deeper dive on the headset (bed 3.8 m) needs ~15 s to drift up; test lasted 16 s | 22 s |

Known limitation found: below ~20 fps a Reaper bolt can reach the player inside the enemy weapon's own update before
the sword's parry check (delphi_parry deflects 0 at 18 Hz in the harness, 3-6 at 24-72 Hz).

devtest.ps1 now: flags OTHER APP DRAWING per test, checks the events each script lists ("Log ...") and prints
`events n/m (MISSING: ...)`, and ignores garbage App= values (1e5+ ms). 25 more scripts got event lists.
New experiment set `devtest/perf` (16 runs): the 5a meadow and Kabuto's storm, each with one feature off at a time
(look0, nograss, msaa0, up0, noclouds, noweather, quality 1/3, all off) - the next device run, to price each feature.

## Device run 20261026-220802 — GPU cost experiments (APK fccc02d, `-NoInstall -Set perf`)
16 runs, each 18-22 s, quality pinned, FFR 2. "app gpu" = VrApi App= average (game's own; p_5a_base had another app
drawing for 3 s, 12 ms - flagged by the new devtest.ps1). Saving = base minus variant.

| Scene | base | look0 (classic look) | no grass | MSAA off | textures not doubled | no cloud shadows | no weather | quality 1 | quality 3 | all off |
|---|---|---|---|---|---|---|---|---|---|---|
| 5a meadow (Meccaryn) | 32.4 ms, 23 fps | 19.6 (-12.8) | 26.7 (-5.7) | 30.2 (-2.2) | 32.4 (0) | 32.2 (0) | - | 31.2 (-1.2) | 17.9 (-14.5) | 17.2 ms, 41 fps |
| kstory5_1 storm (Kabuto) | 56.8 ms, 15 fps | 36.1 (-20.7) | - | 48.3 (-8.5) | 56.2 (0) | - | 55.9 (-0.9) | - | 32.4 (-24.4) | 29.9 ms, 24 fps |

Reading: the rework look (relief, specular, filmic tone, haze, triplanar, caustics, detail octaves) is the largest
single cost; grass and MSAA come next; texture doubling, cloud shadows and weather cost nothing measurable. But with
everything off the storm still needs 30 ms with 171 draw calls where the classic build drew 260-350 at 12 ms in a
mission of the same world (2026-10-25) - the one program with every path in it is the remaining suspect. Commit
77d7d98 compiles per-feature variants (identical renders in the harness); APK 77d7d98 with four `*_uber` runs in
devtest/perf measures the difference on the headset in one run.

## Device run 20261026-223402 — shader variants A/B (APK 77d7d98, `-Set perf`)
Same scenes as 220802, now with per-feature shader variants; the `*_uber` runs use the one program (same run, same
conditions). GPU ms (VrApi App=), fps.

| Scene | one program | variants |
|---|---|---|
| 5a meadow, everything off (lean) | 17.2, 40 fps | **7.5, 71.8 fps** |
| storm, everything off (lean) | 29.9, 24 fps | **10.1, 71.7 fps** |
| 5a, classic look (look0), rest on | (19.6) | **9.7, 71.7 fps** |
| storm, classic look, rest on | (36.1) | **13.0, 68 fps** |
| 5a, fallback level 3 | (17.9) | **8.7, 71.8 fps** |
| storm, fallback level 3 | (32.4) | **12.7, 71.6 fps** |
| 5a, rework look, all on (base) | 32.5 | 30.1 |
| storm, rework look, all on | 56.2 | 48.5 |
(brackets: run 220802.) No variant failed to compile on the Adreno 650 (all 6 logged). Verdict: the one program cost
10-20 ms in the cheap looks; the rework look itself costs another ~20-35 ms - priced part by part next (devtest/look).
Also: the sky-coloured haze is now skipped where the fog weight is 1 (exact; harness renders identical).

## Device run 20261026-225245 — the rework look's parts (APK f24bf1f, `-Set look`)
Rework look, quality pinned at 0, shader variants on; one part compiled out per run (GVR_LOOK_OFF). GPU ms.

| Scene | full look | no sky haze | no rock triplanar | no relief | no highlights | no detail octaves | all six off | fallback level 2 |
|---|---|---|---|---|---|---|---|---|
| 5a meadow | 32.6 (24 fps) | 16.0 (-16.6) | 31.5 (-1.1) | 29.3 (-3.3) | 30.8 (-1.8) | 30.7 (-1.9) | 12.1 (72 fps) | 9.3 (72 fps) |
| storm | 43.9 (18 fps) | 25.6 (-18.3) | 34.9 (-9.0) | 41.0 (-2.9) | 39.4 (-4.5) | 43.5 (-0.4) | 17.1 (45 fps) | 13.4 (65 fps) |

The sky haze (sky band looked up per pixel: asin, atan, two pow, the grade) is most of the look's cost; then the
triplanar rock where there is much rock. Commit df9af08: the haze from a 64x64x6 cube map rendered once a frame by
the same shader code (harness: 0.5-1.4 % of pixels differ by more than 4/255, in the distant haze only); rock reads
only the projections that show; the fallback steps two levels under 50 fps. To be measured (devtest/look,
`l_*_hazepix` = the per-pixel haze for comparison).

## Device run 20261026-231650 — the haze cube and lighter rock (APK df9af08, `-Set look`)
| Scene | full look | per-pixel haze (old) | no haze | no rock | no relief | no highlights | no octaves | six off | level 2 |
|---|---|---|---|---|---|---|---|---|---|
| 5a meadow | ~15.5 (base run had the shell drawing 19 s: 16.6) | 35.0 | 15.0 | 15.4 | 13.8 | 14.7 | 14.4 | 12.7 (72 fps) | 9.3 |
| storm | 20.9 (36 fps) | 42.1 | 20.2 | 18.9 | 19.5 | 19.6 | 19.9 | 17.2 (44 fps) | 13.1 (65 fps) |
The haze cube saves 19-21 ms (full look 32.6 -> ~15.5 meadow, 43.9 -> 20.9 storm); rock now 0.1 / 2.0 ms (was
1.1 / 9.0). Variants compile in 1986 ms at start on the Quest (12 programs). Commit (lite level): the fallback's level 1
is now the rework look without relief, highlights and detail octaves (~12.7 ms in the meadow); devtest/levels measures
levels 0-3 and the adaptive choice in five scenes.

## Device run 20261026-233657 — quality levels (APK 854e651, `-Set levels`)
GPU ms (fps) per level pinned; "auto" = the adaptive fallback (level it ended at). *another app drew during the run.

| Scene | level 0 | level 1 (lite look) | level 2 | level 3 | auto |
|---|---|---|---|---|---|
| Delphi r1 | 13.9* (58) | 10.3 (72) | 9.4 (72) | 8.5 (72) | 0, 70 fps |
| Kabuto k1a | 16.6* (47) | 11.4 (72) | 9.8 (72) | 9.6 (72) | 1, 65 fps |
| 5a meadow | 15.6 (53) | 11.8 (72) | 9.3 (72) | 8.8 (72) | 1, 65 fps |
| 1a village | 19.8 (36) | 13.9 (60) | 10.8 (72) | 9.9 (72) | 1, 60 fps |
| Kstory5 storm | 21.1 (36) | 17.9 (40) | 13.4 (66) | 12.7 (72) | 3, 58 fps |

Every scene holds 72 Hz at some level. The automatic choice started too high and stepped back up after 12 s to a level
that failed again (the village: 2 -> 1 at 60 fps). Changed (next APK): starting levels per world from these runs
(Kstory5 3; Story1, Kstory3, Rstory2 2; others 1), a failed step up is not retried for 10 min, levels saved by
older builds ignored (quality2.txt).

## 2026-10-27 — the whole test suite (harness; device run pending)
Built for the user's "I want a complete whole test": one command, `devtest\devtest.ps1 -All`, runs every set
in order and then the mission sweep. Planned length: about 5 h 20 min (durations plus ~25 s a test).

| Set | Scripts | What it checks | Harness result |
|---|---|---|---|
| tests | 60 | the existing feature tests | unchanged scripts (they passed at 18/24/72 Hz on 2026-10-26) |
| missions | 29 | objectives by their own mechanic: 6 boss/guard fights (auto-aimed), 3 pop-up bombs, 3 tornadoes, 4 Reaperski races (autopilot), 11 Kabuto gates (punches, adrenaline, the magnifying glass), 2 Kabuto boss fights | 29/29 at 72 Hz; at 24 Hz all but rstory3_race_3 before its fix (below) |
| weapons | 18 | each Meccaryn weapon (handgun, shotgun, machine gun, sniper, mortar, launcher, grenades, mines, pop-up bomb, bush pack, flares), Delphi's five bows, Kabuto's punch and Offspring eggs | 18/18 at 24 and 72 Hz |
| npcs | 7 | Reapers dodging, shooting back, punching; Rippers out of a barracks and killed; a Vimp hunt; base building; an Offspring hunting | 7/7 at 72 Hz (npc_ripper_fight after a script change, below) |
| ui | 10 | main menu: new game, mission select, multiplayer, options; in-game options, controls card, new game; map; briefing; shop; hints | 10/10 at 24 and 72 Hz |
| cutscenes | 66 | every cutscene chain played to its end | 66/66 at 24 Hz |
| campaign | 1 | the whole campaign in one run (GVR_AUTOCOMPLETE 25: each mission completed after 25 s of play) | 55/55 missions complete, through to the_end, at 72 Hz |
| sweep | 56 | every mission 20 s | 56/56 exit 0, 0 errors (harness sweep, 2026-10-27) |

Found and fixed by the new tests:
- **Reaperski autopilot reversed forever** after a bump (rstory3_race_3): it backed off whenever the speed was
  under 1 m/s, and the way back to forward speed passes through that. It now backs off after a second stuck. Race
  3 won in 234 s again; all four races are won.
- **Main menu OPTIONS closed the frame it opened:** the trigger that chose it pressed RESUME. A real bug
  for players, found by ui_title_options.
- **Point-blank shots at Rippers** (1.2-1.5 m) did not land at 72 Hz in the harness. npc_ripper_fight now backs
  off; the bug itself is open (docs/ROADMAP.md 2).
- Not verified: taking a shop item by hand (ui_shop checks only walking in); Smarties cheering, Timmy and the
  bush poke (their tests did not fire; dropped for now).

New logs for the checks: "Rocket fired", "Creature N dies", "Options menu opened / closed (resume)",
"Options: <row> changed", "Options: controls card shown", "Briefing: <title>". New script commands: `title`
(start at the main menu), and GVR_AUTOAIM_VIMPS (auto-aim at Vimps).

## Device run 20261027-191542 — the whole test on the Quest (APK bac967b), 2026-10-27
`devtest.ps1 -All`: 247 runs in 248 min (planned 5 h 16 min), **247/247 finished, 0 crashes, 0 errors logged.**

| Set | Result on the headset |
|---|---|
| missions | 28/29 with all events - m_race_rstory3_race_2 lost focus (below) |
| weapons | 18/18 |
| npcs | 7/7 |
| ui | 10/10 |
| cutscenes | 66/66 |
| campaign | 1/1: 55/55 missions completed in order to the_end in one session (31 min of play); resident memory 268 -> ~350 MB, flat from Kstory2 on |
| sweep | 56/56 |
| tests | 54/60 with all events - 6 misses, all from stale scripts on the PC (below) |

**Frame rate (DEVICE):** median 71.8 fps; 213 of 247 runs at 70+; 3 under 60 (npc_ripper_fight 56.2,
delphi_tornado_circle 58.2, scorch_marks 59.9); median app GPU ~10 ms (sets: tests 10.0, missions 11.9,
weapons 10.4, cutscenes 9.2, sweep 9.9). On 2026-10-26 (fccc02d) the median was 28 fps / 26 ms - the
per-world quality levels and the lite look did it (B19).

**The misses, diagnosed:**
- birds_shoot, delphi_dive, gun_grab, kabuto_lobird, kabuto_smartie_throw, smartie_throw_hit: the PC's
  `devtest\tests` copies were the versions from before fae230d's fixes (diffed: e.g. kabuto_smartie_throw had no
  GVR_SMARTIE_AHEAD, gun_grab's hand went the wrong way). The current scripts pass in the harness with the
  headset's own frame times (new `--frame-times <frames.csv>` replay). All 60 current scripts are now on the PC.
- m_race_rstory3_race_2: the Guardian set-up came up 41 s into the race (session FOCUSED -> IDLE for 7 min). Not
  the game. devtest.ps1 now detects a lost focus and runs the test again.

**Tool changes:** `--frame-times` in the harness; devtest.ps1: FOCUS LOST detection + rerun, per-set totals count
each test once, the other-app flag needs 6+ s.

### Re-run of the 7 (20261028-090114): 6/7 pass on the headset
birds_shoot, delphi_dive, gun_grab, kabuto_smartie_throw, smartie_throw_hit and m_race_rstory3_race_2 (won) pass
with the current scripts. kabuto_lobird missed: the swoop reached the hand at ~5.6 s, after the squeeze pulses had
ended (5.34 s; harness 5.43 s, on the last edge). Its pulses now run to 8.4 s (a427765; harness 24/72 Hz: caught
and eaten). The race had the system shell drawing for 214 s (Guardian visible?): its GPU figures are not the game's.

## 2026-10-28 — fixes from the whole test (harness verified; APK dc94e76 not yet on the headset)
| Change | Harness check |
|---|---|
| Rippers at point blank can be shot (a ray starting inside a creature's hit box was a miss) | npc_ripper_fight without backing off: 4 kills at 24 and 72 Hz (was 0 of 324 rounds at 72); unit test |
| Smoke/dust blending budget by quality level (stable thinning, denser survivors) | GVR_FX_COST: pop-up bomb worst frame 69 -> 11 screens; side-by-side frames look alike |
| Shot splashes merged per patch, crowns/rings capped, none at the viewer's feet, spray faded sooner, Ripper burst flash faded near the eye | water fight 12 -> 4 screens; the blue sheets over the view are gone |
| Getting on a moored Reaperski put Delphi at the world origin (mountJetski read its args after resetting) | m_ski_rstory2_jetski_2 24/72 Hz: rides, objective done |
| Jet-ski no longer climbs hillsides (steep faces over 8 m above the sea stop it) | races 1/2/3 and rstory5 won at 24 and 72 Hz |
| ui_shop reaches the real item spot | "Shop: took ammo" 24/72 Hz |
| Regression | tests/missions/weapons/npcs/ui at 72 Hz: all pass except weather_storm's ambience line (harness has no audio; passes on the headset); sweep 56/56 |

## 2026-10-28 (afternoon) — new mechanics (harness verified; APK e8e1e95 on the PC, not yet on the headset)
| Change | Harness check |
|---|---|
| Reaper guns picked up fire in your hand (spear gun 12 shots, bazooka 3) | reaper_gun_fire: "fires the Reaper" + a kill, 24/72 Hz |
| Delphi's fire arrows (an arrow lit at a flame or the bow-hand trigger; sets grass/brush ablaze) | delphi_fire_arrow, delphi_fire_arrow_hand: 24/72 Hz |
| Wrist display (forearm across the face: health, ammo, injections, flares, objective distance) | wrist_display: "Wrist display shown" only with every frame rendered (`--video <dir> 1`); the plain harness renders only the last frame, so it shows MISSING there |
| Health injection jabbed into the other forearm | syringe_arm: 24/72 Hz |
| Native crash reports in files/crash/ (signal, backtrace, last 160 log lines); devtest.ps1 pulls them | BUILD only; experiments/crash_report (GVR_TEST_CRASH) is the device check |
| Kabuto's microphone roar (Options -> KABUTO MIC ROAR, off by default; RECORD_AUDIO asked when used) | kabuto_mic_roar (GVR_MIC_TEST stands in for the mic): 24/72 Hz. The real microphone: device only, not yet tried |
| Regression at 72 Hz | tests 54 ok + 3 render/audio-side (fx_water_fight and wrist_display pass with every frame rendered; weather_storm's ambience line needs audio); missions 30/30, weapons 18/18, npcs 7/7, ui 10/10; sweep 56/56; unit tests pass |

## 2026-10-28 (evening) — more mechanics and fixes (harness verified; APK 47315f8 on the PC, not yet on the headset)
| Change | Harness check |
|---|---|
| GPU time per offscreen pass (GVR_GPU_PASSES; on in every scripted device run; result.txt `gpu_passes`, review_results.py table) | Mesa has the extension: shadow / haze / reflection / eyes logged every 5 s with --video frames. Device numbers: not yet |
| Reapers take cover among palms | npcs/npc_reaper_cover 24/72 Hz |
| Bush poke: the Reaper stopped 1.6 m short and punched an unseen bush - fixed | npcs/npc_bush_poke 24/72 Hz; standing still in the bush stays hidden |
| Smarties cheer only within 90 m (was every village) | npcs/npc_smarties_cheer 24/72 Hz (6 cheer, was 11) |
| Timmy on his tower | npcs/npc_timmy_tower |
| Base table map on the Build Pad (touch a site to build) | ui/base_table_map 24/72 Hz; npc_base_build still completes; frame looked at |
| Kabuto's growth by eating (GVR_KABUTO_FEED) | tests/kabuto_growth: 50 -> 70 % in ~15 s; missions/m_gate_k1a_grown70 breaks the gate; tests/kabuto_gate_too_small (60 %) can't - 24/72 Hz |
| Parry: blade swept between frames | delphi_parry at 12/15/20/24/36/72 Hz (18 Hz: this script's bolts pass 0.14-0.6 m from the blade) |
| Options -> PLAY SEATED (+0.45 m) and the GVR_SETTINGS hook | unit test; eye 0.45 m higher than the same script standing; ui/ui_seated |
| Rounds left on top of the gun / arrows by the bow grip | tests/gun_counter, tests/bow_counter; frame looked at |
| Kabuto's footfalls and the steadying hand's gun kick in the haptics | kabuto_footprints logs the footfalls (haptics themselves: device only) |
| Options -> MOVEMENT: TELEPORT | ui/teleport_move: two 6 m blinks, 24/72 Hz; arc frame looked at |
| rstory2_jetski_3 (B21) | water map: the ski's lagoon is enclosed (lowest crossing 27 m); on foot reachable by flood fill, the walk autopilot didn't arrive in 10 min |
| Regression at 72 Hz | tests 59 ok + the 3 render/audio-side lines; missions 31/31, weapons 18/18, npcs 11/11, ui 13/13 (ui_title_options fixed for the two new menu rows); sweep 56/56; unit tests pass |

## 2026-10-28 (night) — options, speed, hands (harness verified; APK 070917d on the PC, not yet on the headset)
| Change | Harness check |
|---|---|
| Water reflection at half size at quality 1 | sea_monster: reflection pass 2.92 -> 1.33 ms (Mesa), frames look the same |
| Shadow map every other frame at quality 1+ | kabuto_k1a frames identical |
| Terrain far ring (no 2nd grass/rock, one rock projection past ~90 m) | 1a village at full quality: 9 pixels differ by <= 2 |
| Options -> DIFFICULTY | tests/difficulty_easy (a bolt hurts 2.5), difficulty_hard (7.5) |
| Mission select unlocks + progress.txt | ui/ui_mission_unlocks: 6 of 55 with 3 finished |
| Three save slots on the CONTINUE row | harness with --save: empty slot 2, Story3 checkpoint in slot 3 listed; (scripted device runs have no save file: by hand) |
| Music: battle ending phrase, death song | BUILD only (no audio in the harness) |
| Index finger from capacitive touch | frames: fist vs pointing differ as intended; tests/hand_point's binding line is device-only |
| Mission numbers on the next loading screen | tests/mission_stats |
| Delphi takes arrows back | tests/arrows_back 24/72 Hz |
| devtest.ps1 report.html | not run (no PowerShell here); bracket balance checked |
| 36 Hz run of every script (first time) | all pass as at 72 Hz except reaper_gun_fire, which found the stale aim laser (fixed) and an autoaim offset in the test |
| Regression at 72 Hz | tests 62 + 4 render/audio/device-only lines; missions 31/31, weapons 18/18, npcs 11/11, ui 14/14; sweep 56/56 |

## 2026-10-28 (late) — review fixes, checkpoints (harness verified; APK be28c75 on the PC, not yet on the headset)
| Change | Harness check |
|---|---|
| Fixes from the independent review (mic lifetime/errors, arrow tags, seated camera and gestures, wrist timing, resets) | tests, ui, npcs, weapons at 72 and 24 Hz: all pass but the 3 audio/render/device-only lines; wrist_display now passes in the plain harness (its timer moved to update) |
| Checkpoints within a mission | tests/checkpoint_respawn 24/72 Hz |
| Teleport refused on a steep face | tests/teleport_refused |
| Base table map on Delphi's base | ui/base_table_map_delphi |
| Thumb from capacitive touch | frame rendered; which digit is the thumb: device to confirm |
| Crash handler without dladdr; symbolize_crash.py | BUILD; symbolizer checked on a made-up report against the built library |
| Unit tests | progress helpers, projectile tag: pass |

## 2026-10-28 (late evening) — practice, main menu, pointing, hands (harness verified; device-only parts marked)
| Change | Harness check |
|---|---|
| Main menu PRACTICE (Baz / Delphi / Kabuto on the title island, full kit, Reaper waves, nothing saved) | ui/ui_title_practice (Delphi: 3 bows; Baz: every gun + launcher; Kabuto at full size, no guns) |
| Options -> MAIN MENU (held 2 s) | ui/ui_practice_quit; by hand with --save: 1a -> main menu -> CONTINUE reloads 1a from the checkpoint, kit dropped |
| Options -> FOVEATED RENDERING: AUTO (dynamic, up to high) | unit test (setting 4 kept, clamped); the runtime's dynamic level: DEVICE only |
| game_app.cpp split into 6 system files (no code changed) | every original line in exactly one file; full regression identical |
| Menus by pointing (dot, hover selects, trigger on a value's halves / a row's ends changes it, scroll bands) | ui/ui_title_point, ui/ui_options_point (72 / 24 Hz) |
| Bare hands point at the menus (XR_EXT_hand_tracking + XR_FB_hand_tracking_aim) | BUILD only (Android library compiles); DEVICE to check |
| Fixes from an independent review (held trigger finishing a hold, loops/music/Reaperski after a quit, a death carried to the menu, scroll under a still pointer, press on another row, pinch hysteresis) | by hand in the harness: a trigger held from play no longer quits (10.5 s run: menu opened, no quit), a quit during the death goes to the menu without a respawn; pinch: BUILD only |
| Existing menu tests after the new rows | ui_title_options / ui_title_skirmish one more step down, ui_ingame_newgame up twice |
| Regression at 72 and 24 Hz | tests 67 + the 3 known harness-only lines (fx cost, touch binding, storm audio); ui 19/19, npcs 11/11, weapons 18/18, missions 31/31; unit tests pass |
| Delphi: SUMMON HAIL and SLOW TIME by gesture | tests/delphi_hail (2 of 3 Reapers killed by the hail), tests/delphi_slow_time, 72 / 24 Hz; frames checked (ice blocks, the slow dome) |
| Skirmish balance (kill limit 20, Kabuto fumbles / slows with a flag) | bot-only survey, 12 maps x 20 min at 24 Hz: rounds finished 13 (was 6 in 15 min); 3-way winners Blue 1 / Green 1 / Red 1 (was Red 4 of 4); experiments/x_skirmish_bots_3way_l3; DEVICE: a human's games unverified |
| Weapon wheel (Baz) | weapons/w_mecc_wheel 72 / 24 Hz (machine gun chosen by moving the hand right); frame checked; regression: weapons 19/19, tests 69 + 3 known, ui 19/19, npcs 11/11, missions 31/31 |
| Review fixes (spells / wheel) | tests/delphi_throw_overhand (an overhand throw stays a fire bolt, 72 / 24 Hz); delphi_hail now 3 of 3 Reapers 10 m out where she looks; wheels unchanged in their tests |
| Delphi: CLOAK and CLUSTER (r4a's items 23 / 22) | tests/delphi_cloak, tests/delphi_cluster 72 / 24 Hz; see-through hands checked in a frame; regression: tests 72 + 3 known, weapons 20/20, ui 19/19, npcs 11/11, missions 31/31 |
| Baz's Repair Gun (base missions) | weapons/w_mecc_repair 72 / 24 Hz (the Pub 160 -> 360 / 400 over 6 s); frame: the gun in hand; regression: tests 72 + 3 known, weapons 21/21, ui 19/19, npcs 11/11, missions 31/31 |
| Homing rockets (launcher, Shop) | weapons/w_mecc_homing 72 / 24 Hz (a Reaper 35 degrees off the aim killed; the same shot with a plain rocket misses); ui_shop re-aimed for the sixth counter item; regression: tests 72 + 3 known, weapons 22/22, ui 19/19, npcs 11/11, missions 31/31; mission sweep 56/56 exit 0, 0 errors |
| Review fixes (cloak against fighting Reapers, attacks end it, Repair Gun per mission, homing saved / counted) | tests/delphi_cloak_fight 72 Hz: hit 0 times while cloaked (21 with the old cloak, same script); regression 72 / 24 Hz: tests 73 + 3 known, weapons 22/22, ui 19/19, npcs 11/11, missions 31/31 |
| Baz's portable turret (item 11, mc_turret) | weapons/w_mecc_turret (plant, take back by the grip, replant, two Reapers shot) and w_mecc_turret_spent (8 rounds, folds, back with 60 after 20 s) at 72 / 24 Hz; the drawn nozzle 0.00 m from where rounds leave; frame checked (the head on the ground, barrel toward the Reapers); regression: tests 73 + 3 known, weapons 24/24, ui 19/19, npcs 11/11, missions 31/31; mission sweep 56/56 exit 0 |
| Delphi's FOLLOW (item 30, the followspell clips) | tests/delphi_follow 72 / 24 Hz: caught at 10 m, thrown at ~22 m/s, dead on landing; frame checked (hanging limp, the blue thread) |
| Turret review fixes (grip pick-up, trigger block, recall at 60 m / respawn, roofs, dim HUD cell) | weapons/w_mecc_turret, w_mecc_turret_spent 72 / 24 Hz unchanged results; regression: tests 74 + 3 known, weapons 24/24, ui 19/19, npcs 11/11, missions 31/31; mission sweep 56/56 exit 0 |
| Delphi's FIRE CIRCLE (item 31, EFX_circle_spell) | tests/delphi_fire_circle 72 / 24 Hz: cast by the arms-out gesture, 14-15 bolts / grenades / rockets stopped in its wall in 8 s; frame checked from inside |
| Baz's shield (item 17, wheel slot 12) | weapons/w_mecc_shield 72 / 24 Hz (raised by GVR_RAISE_SHIELD as the wheel would; took 114 / 56 of the Reapers' fire in 12 s); frame checked (EFX_shield1 web; EFX_shield is a red placeholder) |
| Review fixes (FOLLOW; fire circle / shield) | delphi_follow re-aimed (a steady aim, a 12 cm pull relative to the head) 72 / 24 Hz; fire circle without god mode: no blast damage after its bursts; regression: tests 75 + 3 known, weapons 25/25, ui 19/19, npcs 11/11, missions 31/31; mission sweep 56/56 exit 0 |
| Jetpack upgrade (item 18, Story 5 on) | weapons/w_mecc_jetpack2 72 / 24 Hz (the held jetpack burns dry with an 11 s tank) |
| Proximity rockets (item 16) and the launcher loads on the wheel | weapons/w_mecc_prox 72 / 24 Hz (bursts by the Reaper; the plain-rocket control bursts ~50 m beyond) |
| Gyrocopter (Gyro_l0; practice) | weapons/w_mecc_gyro 72 / 24 Hz (in by the grip, flies 20 m up and 130 m out, lands on the sea surface, out); frames checked in both views |
| Original sounds for the new items; review fixes (proximity arming / bosses / load fallback / shop after CONTINUE) | no "unavailable" sounds in the runs; w_mecc_prox and w_mecc_homing 72 / 24 Hz still pass; regression: tests 75 + 3 known, weapons 27/27, ui 19/19, npcs 11/11, missions 31/31; sweep 56/56 |
| The Gyrocopter Pad (base) | npcs/npc_base_gyropad 72 Hz: built in 20 s, "Gyropad ready!", the gyrocopter on its top; frame checked |
| Gyrocopter review fixes | w_mecc_gyro 72 / 24 Hz (boarding by a hand on it; a short flight; out on land); regression: tests 75 + 3 known, weapons 28/28, ui 19/19, npcs 12/12, missions 31/31; sweep 56/56 |
| ASan/UBSan over the 20 new devtests (36 Hz) | 0 reports, all exit 0 |
| Options -> KABUTO VIEW | tests/kabuto_view_lower (view scale 5.0 lower vs 8.1-8.4 giant); ui set for the new row |
| Review fixes (pad rebuilt, engine after a cutscene, KABUTO VIEW keeps throws / reach, fade on resume) | npc_base_gyropad, w_mecc_gyro, kabuto_view_lower 72 Hz unchanged; regression below |
| Thrown / falling bodies and walls, roofs | unit test testFallingCollision (a 5 cm wall at 24 Hz and 25 m/s; a hut roof; the hut removed); tests/delphi_follow 72 / 24 Hz unchanged |
| The portable turret shoots Rippers | weapons/w_mecc_turret_rippers 72 / 24 Hz (Story2 by the Ripper barracks: four Rippers shot) |
| Delphi's cloak against Rippers | unit tests in testCreatures (not noticed at 15 m in front; a chasing one goes to the old spot, no bites) |
| Regression 2026-10-29 | tests 76 + the 3 known harness-only lines, weapons 29/29, ui 19/19, npcs 12/12, missions 31/31 (72 Hz); tests, weapons at 24 Hz the same; sweep 56/56 exit 0 (one warning: r4a's second Hellfire turret site); unit tests pass |
| ASan/UBSan 2026-10-29 | mission sweep 56/56 and 7 devtests (turret x2, FOLLOW, cloak fight, gyro pad, gyro, Kabuto view) at 36 Hz: 0 reports, all exit 0 |
| Delphi's SHRINK (item 29, st_rp_shrink) | tests/delphi_shrink 72 Hz (cast at 10 m, trodden on, squashed); frames checked (quarter-size Reaper running, his reflection); unit test testShrink |
| r4a's second Hellfire turret site | sweep: the warning gone, both sites placed |
| Regression after SHRINK | tests 77 + the 3 known harness-only lines, weapons 29/29, ui 19/19, npcs 12/12, missions 31/31 (72 Hz); tests, weapons at 24 Hz the same; sweep 56/56 clean (0 errors, 0 warnings); ASan: unit tests and delphi_shrink / delphi_follow / w_mecc_turret_rippers / delphi_cloak_fight 0 reports |
| Review fixes (shrunk Reapers' grenades, SHRINK vs the run grip, double wall/landing damage, 1.5 m roofs, tower re-link, Ripper lastSeen, cooldown reset, shrink-aware aim) | tests/delphi_shrink_run 72 / 24 Hz (pushed stick: not cast; idle: cast); regression: tests 78 + 3 known, weapons 29/29, ui 19/19, npcs 12/12, missions 31/31 (72 Hz), tests / weapons at 24 Hz the same, sweep 56/56 clean; ASan: unit tests + 8 devtests 0 reports |
| Delphi's SUMMON SEA MONSTER (item 32; RM_seamonster, weapon 48) | tests/delphi_sea_monster 72 / 24 Hz (summoned, a fireball, the Reapers on the shore killed); frames checked |
| Delphi's REPAIR (item 73) | tests/delphi_repair 72 / 24 Hz (Persuader 160 -> 361 / 400, no fire wall on release) |
| Regression after SEA MONSTER and REPAIR | tests 80 + the 3 known lines, weapons 29/29, ui 19/19, npcs 12/12, missions 31/31 (72 Hz), tests / weapons at 24 Hz the same, sweep 56/56 clean; ASan: delphi_repair, w_mecc_repair, delphi_sea_monster, delphi_shrink, w_mecc_launcher 0 reports |
| Review fixes (REPAIR / SEA MONSTER gesture conflicts, gaps, resets) | delphi_sea_monster (lift held overhead) 72 / 24 Hz; delphi_sea_monster_hail (brought straight down: not cast); delphi_repair 72 Hz |
| Fixes found making video 20 (rotor blur blended, gyrocopter chase camera, third-person camera kept above the sea) | frames checked in the video's clips; regression: tests 81 + 3 known, weapons 29/29, ui 19/19, npcs 12/12, missions 31/31 (72 Hz), tests / weapons at 24 Hz the same, sweep 56/56 clean |
| Delphi's TELEPORT (item 24) | tests/delphi_teleport 72 / 24 Hz (cast, the blink 7.8 m); FOLLOW / SHRINK tests: no teleports |
| Regression with TELEPORT | tests 82 + 3 known (delphi_cloak_fight's count fixed, 72 / 24 Hz 0 hits), weapons 29/29, ui 19/19, npcs 12/12, missions 31/31; tests / weapons at 24 Hz; sweep 56/56 clean |
| Review fixes (TELEPORT: roof height, stale spot, death mid-blink, wind-up; camera sea clamp; soft parts order) | delphi_teleport 72 / 24 Hz, delphi_teleport_windup ("not cast") 72 / 24 Hz, w_mecc_gyro, delphi_follow, delphi_cloak_fight 72 Hz |
| Regression after the TELEPORT / camera / soft-part review fixes | tests 83 + 3 known, weapons 29/29, ui 19/19, npcs 12/12, missions 31/31 (72 Hz), tests / weapons at 24 Hz the same, sweep 56/56 clean; unit tests pass |
| The whole campaign (devtest campaign/camp_whole_game, 24 Hz, 2026-10-29 at 5ad8e3c) | 55/55 missions complete to the_end in one session, exit 0, 0 errors; resident memory (desktop harness) 283 -> 535 MB over the run (B15: the headset's 2026-10-27 run stayed flat at ~350 MB - watch on the next device run) |
| Delphi's SUMMON SHADOW (item 26) | tests/delphi_shadow 72 / 24 Hz (summoned 2.5 m out, Reaper 7 killed in three blows, on to Reaper 6); frames checked (rising out of smoke, running at them) |
| SUMMON SHADOW review fixes | delphi_shadow and delphi_shadow_slash ("not cast") 72 / 24 Hz; tests set 85 + 3 known at 72 Hz; the full regression before them: weapons 29/29, ui 19/19, npcs 12/12, missions 31/31, sweep 56/56 clean |
| ASan/UBSan over every devtest (2026-10-29, snapshot ba48871, 36 Hz) | 186/186 scripts (tests, weapons, ui, npcs, missions) exit 0 with 0 AddressSanitizer / UBSan reports; unit tests pass under ASan |
| Controls card in pages; pause on input focus loss; Reaper weapons preloaded (4dbcb90) | ui/ui_controls_pages, ui/ui_focus_pause 24 Hz; GVR_PROF_SPIKES: the 25-30 ms first-play-frame hitch gone; regression tests 85 + 3 known, weapons 29, ui 21, npcs 12, missions 31 |
| Skirmish: spells and fists against the other players (7f4dffc) | GVR_SK_DUMMY on M_3WAY_L3: fire wall 84 -> 64 "by you", tornado 84 -> 0, fire circle 68 -> 0, Baz's punch 100 -> 57; Delphi's punch: none (her free hand casts) |
| Online spells (protocol 3) and the menu online | two harnesses (GVR_REALTIME, host + join on one machine): a joiner's fire wall shown on the host and the host's on the joiner; a joiner 24 s in the system menu not dropped |
| Release-review fixes (c1010d5) | unit test testAtomicWrite; regression tests 85 + 3 known, weapons 29/29, ui 21/21, npcs 12/12, missions 31/31 (24 Hz); sweep 56/56 clean (0 errors, 0 warnings) |
| Lift, horn, reach set (9ffc3d7) | reach/ 14/14 (24 Hz): every reach objective walked by the autopilot; m_lift_prison_6 (up 81 m, objective done); npcs/npc_guard_horn (alarm, 6 guards from 3 launchers); regression 72 Hz: tests 85 + 3 known, weapons 29, ui 21, npcs 13, missions 31 |
| Mission bot (GVR_MISSION_BOT) over 28 Meccaryn / Delphi missions from their starts, 900 s, 24 Hz | 22 complete; with targeted starts and the fixes below, every mission except k2d (Kabuto, walked earlier), kstory3_4 (DEVICE B18) and rstory2_jetski_3 (B21) has all its objectives done by mechanics in some test |
| Fixes it found | r1_saveyan / r1_smartiegrab / r1_turbo complete (Yan protected; drop-off by Yan); r4a's ships sunk (m_bot_r4a_ships: objectives 1-3); m_tornado_r4a (barracks); 2a's meat (hunted, picked up, 20 delivered) |
| Regression with the reach set (e8dcbcc / cf476cd) | tests 84 + 3 known (+ kabuto_lobird at 24 Hz: the "eats" log line came out interleaved - it ate; passes at 72), weapons 29/29, ui 21/21, npcs 13/13, missions 34/34, reach 21/23 then the two bot tests fixed (launcher_5 split, Baz flies up out of sight) |
| ASan/UBSan, snapshot cf476cd (36 Hz) | 193/193 devtests (tests, weapons, ui, npcs, missions) exit 0, 0 reports; unit tests pass under ASan; m_lift_prison_6, m_rescue_r1_smartiegrab, npc_guard_horn, m_tornado_r4a 0 reports |
| The whole campaign (camp_whole_game, 24 Hz, cf476cd) | 55/55 missions complete to the_end in one session, exit 0, 0 errors (world switches with the new canLoadWorld check and deferred save deletion) |
