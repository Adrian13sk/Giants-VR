# GiantsVR — development roadmap (written 2026-10-27, after the whole-test suite)

What comes after the whole device test (`devtest\devtest.ps1 -All`). The order is a proposal: the device run's
results come first and can reorder it. Status words as elsewhere: HARNESS / BUILD / DEVICE. Nothing below is
done unless it says so. Estimates are in working sessions (one session ≈ one long evening of work here),
and they are guesses.

## 0. Read the whole test (first, the day after it runs)

- Pull `devtest\results\<run>\` and score every set: done / crashed / timeout, missing events, frame rate, GPU
  ms, battery and the time each set took.
- Each MISSING event becomes a bug with a harness repro. A test that passes in the harness and fails on the
  headset is the most useful kind: usually frame-rate-dependent logic (as the parry was below 20 fps).
- Compare with run 20261026-204701 (`tools/py/review_results.py`): GPU per scene, quality levels chosen, memory.
- Decide which sets join the regular (~45 min) run and which stay in the weekly whole test.

## 1. Performance: hold 72 Hz everywhere (B19) — highest priority

The rework look costs 15.6–21 ms of GPU in the busiest scenes against 13.9 ms. Level 1 (lite) holds 72 Hz in
the meadow, k1a and r1; level 2 is needed in 1a's village; level 3 in Kstory5's storm.

1. **GPU timer queries per pass** (EXT_disjoint_timer_query) logged in test runs: terrain, objects,
   foliage, water, sky, shadows, reflection, particles and post. We price passes now by switching them off
   (`GVR_LOOK_OFF`). Direct timings would be faster and more exact. 1 session.
2. **Terrain:** a cheaper far ring (no detail octaves or relief beyond ~60 m, one texture fetch). Also
   front-to-back draw order and a depth pre-pass test for the village's overdraw. 1–2 sessions.
3. **Foliage and grass:** alpha-to-coverage costs MSAA bandwidth. Try density by distance, impostor cards for
   far palms, and fewer grass layers at level 1+. 1–2 sessions.
4. **Shadows:** cascade 2 every other frame and a static-geometry cache (the sun barely moves). 1 session.
5. **Water reflection:** half resolution at level 1+, and skipped when the water is off-screen (the storm). 1 session.
6. **Fixed foveation per level:** HIGH at level 2+, and the Meta "dynamic foveation" extension where the
   runtime offers it. 0.5 session.
7. **Quest 2 at 90 Hz?** No: 72 Hz stays the target. Offer 90 Hz only for the classic look, as an option.
8. **CPU side:** the frame-time spikes found by the soak (world switches, cutscene loads). Move texture
   decoding to a loader thread with a time budget per frame. 2 sessions.
9. **Memory (B15):** evict the model and texture cache by world, so the campaign soak stays flat (now
   174 → 436 MB). 1 session.

**Done when:** the whole test's median fps is 72 at the default quality, no scene is under 60, and the
quality levels chosen per world are logged and stay put.

## 2. Gameplay bugs and gaps the test suite already shows

- **Parry below 20 fps:** bolts resolve inside the enemy weapon's own update. Move parry into a swept
  sword-vs-bolt test that runs before the bolts' step. 0.5 session.
- **Shop: taking an item by hand.** DONE 2026-10-28: the test reached for the wrong side (items at positive
  angles stand to the LEFT); the ammo box is taken by hand at 24 and 72 Hz.
- **Moored Reaperski missions:** DONE for rstory2_jetski_2 (2026-10-28: the autopilot gets on and rides a water
  route; it found that getting on the moored ski put Delphi at the world's origin - fixed). rstory2_jetski_3: no
  water route to its objective even over 1.2 m sandbars (ice walls or a closed lake?) - not yet tested by its
  mechanic. 0.5 session.
- **Kabuto's growth in real play:** the gate tests start him at full size (GVR_KABUTO_GROW). Add a test
  that eats its way from 50 % to the size k1a's gate needs, with timings. 1 session.
- **Reach objectives on foot:** GVR_REACH_WALK walks the path in four missions. Add the rest (every
  "reach" objective in the campaign) as mission tests. 1 session.
- **Smarties cheering, Timmy, the bush poke:** behaviours that exist but whose tests didn't fire in the
  harness (dropped from the NPC set for now). Find the right setups and add them back. 1 session.
- **Point-blank shots at Rippers:** FIXED 2026-10-28 (674b41e): a ray starting inside a Ripper's hit box (it
  reaches ~1.1 m in front) counted as a miss.
- **A Reaper hiding behind cover:** `takes cover` didn't show in the open bunker field. Test it where there
  are trees and rocks. 0.5 session.

## 3. VR feel and comfort (needs the user's hands in the headset)

- **Comfort pass with the user:** vignette strength, snap vs smooth turn defaults, Kabuto's eye height and
  speed, the Reaperski view turning with the ski, and the jetpack's vertical acceleration.
- **Teleport locomotion** as an option for players who get motion sick (not in the original; VR-only).
  Blink with a fade; disabled in races. 1–2 sessions.
- **Seated mode:** raise the floor by a set height, plus a "crouch" button. 0.5 session.
- **Hand presence:** finger poses from capacitive touch (thumb rest, index on trigger) on every hero; the
  Quest 2's hand tracking for menus only. 1–2 sessions.
- **Haptics everywhere:** per-weapon waveforms (machine gun rumble, sniper crack), Kabuto's footsteps by
  size, Delphi's bow string tension rising with the draw. 1 session.
- **Weapons in the hand:** visible ammo counters on the guns (the original's HUD numbers moved onto the
  weapon), and reloading by a gesture (an option; the original has no reload). 1–2 sessions.
- **Delphi's spells by gesture:** the circle and sweep exist. Add the other spells the data has as
  gestures, each with a practice card in Options → CONTROLS. 2 sessions.
- **Kabuto at scale:** a "giant's view" setting (true eye height vs a lowered, comfortable one), and his
  hands reaching the ground without stooping (the reach assist exists; tune it on the device).

## 4. Fidelity to the original

- **B9 — AI from the data:** the mission AI/state ops are still unknown. Decode more of them from the
  mission streams, and compare the Reapers' behaviour against the original running on the PC (the user can
  record a few minutes of the original as reference video). Long-term.
- **B6 — rendering rules:** compare screenshots of the original (the user's PC) against the harness for
  10 fixed views. Fix the unit scale, fog colour and lightmap rule where they differ. 2 sessions plus the
  user's screenshots.
- **B16 — weapon records:** confirm which record each gun uses, from the original's behaviour (rate of fire
  measured in a recording).
- **Music:** the original's mood selection (calm / tension / combat), driven by our combat state. Its
  transitions should be beat-aligned where the tracks allow. 1 session.
- **`.abx` object sets (B4):** decode them and check whether missions use objects we don't place yet.
- **Cutscenes:** lip-sync the dialogue (jaw nodes, from the audio's loudness), and set the actors'
  idle loops between lines. 1–2 sessions.
- **Subtitles:** the original's text for every voiced line (the data has the strings), timed to the audio. 1 session.

## 5. Content and modes

- **Multiplayer (branch `next`):** two-headset LAN test with the user (needs a second Quest or a PC
  build). Merge `next` into `rework` once the rework's device run is clean. Voice chat is out of scope.
- **Skirmish vs bots on all 12 maps:** a balance pass from the logged capture counts.
- **A PC "spectator" build** (the harness with a window, mouse and keyboard). It's a cheap second player
  for multiplayer tests and lets the user watch a test without the headset. 1–2 sessions.
- **Save slots and checkpoints per mission** (now one checkpoint). 0.5 session.
- **Difficulty levels:** enemy damage and accuracy scaled; the god-mode test switch stays a test switch. 0.5 session.
- **Mission select unlocks:** finished missions only (now every mission is offered). 0.5 session.

## 6. Test and build infrastructure

- **The whole test on a schedule:** after each APK, a weekly `-All` run. A one-page report (HTML) built from
  the results folder: per set, per test, trends against the last run, with screenshots. 1 session.
- **Harness parity:** run every device script in the harness at 72 Hz before each APK (the `runall` +
  `expect3` scripts) as part of `scripts/build_apk.sh --check`. 0.5 session.
- **Frame-rate-independence tests:** every script also at 36 Hz in the harness. This finds the
  parry-style bugs before the device does. 0.5 session.
- **Crash reporting:** DONE 2026-10-28 (ed4ed9e, BUILD only): signal, backtrace and the last 160 log lines in
  `files/crash/crash_<time>.txt`; devtest pulls it; `experiments/crash_report` checks it on the headset.
- **Official toolchain (B2):** rebuild with Google's NDK r27c and build-tools before any public release.
- **Code health:** split `game_app.cpp` (4,900 lines) by system. There have been three code reviews; make
  them a habit at each milestone.

## 7. Release readiness (only when the user decides)

- `INSTALL.md` for other owners of the game (GOG `Bin/` copy, sideloading). No game data ever in the APK.
- Licence review of every dependency (docs/DEPENDENCIES.md); this project's own licence.
- An "unofficial, not endorsed" notice on the main menu and in the README.
- Quest 3 / 3S check (higher resolution, other GPU): another quality table.

## Suggested next three sessions

1. Read the whole test, then fix every MISSING event and crash it finds (§0, §2).
2. GPU timer queries per pass, then the terrain far ring and foliage changes (§1.1–1.3), measured on the headset.
3. Memory eviction per world (§1.9), then a new whole-test run.

## Added 2026-10-28 (from the brainstorm; harness only until a device run)
Reaper guns that fire in your hand, Delphi's fire arrows, the wrist display, the injection jabbed into the forearm,
Kabuto's microphone roar (Options). Still open from the brainstorm: tabletop base building, the Reaper cover test,
the jetski_3 route.
