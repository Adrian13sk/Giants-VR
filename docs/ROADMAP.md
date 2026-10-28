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

1. **GPU timer queries per pass:** DONE 2026-10-28 (dc8f316, harness; device numbers pending): shadow, haze cube,
   reflection, scope, cinema screen and "eyes" (the rest) in every scripted run's result.txt; review_results.py tables it.
2. **Terrain:** 2026-10-28: the far ring past ~90 m drops the second grass and rock textures and uses one rock
   projection (invisible under the haze). Depth pre-pass for the village: not tried.
3. **Foliage and grass:** alpha-to-coverage costs MSAA bandwidth. Try density by distance, impostor cards for
   far palms, and fewer grass layers at level 1+. 1–2 sessions.
4. **Shadows:** 2026-10-28: the map is drawn every other frame at quality 1+ (one map, no cascades to split).
5. **Water reflection:** 2026-10-28: half size at quality 1 (already skipped at 2+ and when looking up).
6. **Fixed foveation per level:** done earlier (one step stronger at quality 1+). Dynamic foveation: 2026-10-28,
   Options -> FOVEATED RENDERING: AUTO (the runtime's dynamic level up to high); device numbers pending.
7. **Quest 2 at 90 Hz?** No: 72 Hz stays the target. Offer 90 Hz only for the classic look, as an option.
8. **CPU side:** the frame-time spikes found by the soak (world switches, cutscene loads). Move texture
   decoding to a loader thread with a time budget per frame. 2 sessions.
9. **Memory (B15):** not needed now: the headset's campaign run (2026-10-27) stayed flat at ~350 MB. Evicting the
   shared cache would risk pointers kept across worlds. Revisit if a longer device soak climbs.

**Done when:** the whole test's median fps is 72 at the default quality, no scene is under 60, and the
quality levels chosen per world are logged and stay put.

## 2. Gameplay bugs and gaps the test suite already shows

- **Parry below 20 fps:** 2026-10-28: the blade is now swept from last frame's place to this one's, so a swing that
  jumps across a bolt's path between frames still meets it. Measured in the harness: delphi_parry parries at 12, 15,
  20, 24, 36 and 72 Hz; at 18 Hz this script's bolts happen to pass 0.14-0.6 m from the blade (the Reapers' aim
  spread), not a frame-rate fault - the bolts are checked before they move, against their path 1.5 frames ahead.
- **Shop: taking an item by hand.** DONE 2026-10-28: the test reached for the wrong side (items at positive
  angles stand to the LEFT); the ammo box is taken by hand at 24 and 72 Hz.
- **Moored Reaperski missions:** DONE for rstory2_jetski_2 (2026-10-28: the autopilot gets on and rides a water
  route; it found that getting on the moored ski put Delphi at the world's origin - fixed). rstory2_jetski_3: the ski's
  lagoon is enclosed (lowest land crossing 27 m, 2026-10-28 water map) - see B21; needs the original's behaviour.
- **Kabuto's growth in real play:** DONE 2026-10-28: tests/kabuto_growth (GVR_KABUTO_FEED walks him to the nearest
  Smartie or Vimp and eats it): 50 -> 70 % in ~15 s in the harness, passing over two meals a house kept him from;
  missions/m_gate_k1a_grown70 (70 % breaks the wooden gate) and tests/kabuto_gate_too_small (60 % can't), 24/72 Hz.
- **Reach objectives on foot:** GVR_REACH_WALK walks the path in four missions. Add the rest (every
  "reach" objective in the campaign) as mission tests. 1 session.
- **Smarties cheering, Timmy, the bush poke:** DONE 2026-10-28 (npcs/npc_smarties_cheer, npc_timmy_tower,
  npc_bush_poke, 24 and 72 Hz). Two bugs found: every village on the map cheered (now those within 90 m), and a
  Reaper poking the bush stopped 1.6 m short - walking on the spot - and punched a bush he hadn't seen through.
- **Point-blank shots at Rippers:** FIXED 2026-10-28 (674b41e): a ray starting inside a Ripper's hit box (it
  reaches ~1.1 m in front) counted as a miss.
- **A Reaper hiding behind cover:** DONE 2026-10-28: npcs/npc_reaper_cover in 5a's palm meadow - Reapers take
  cover behind palms and rocks at 24 and 72 Hz (the bunker field has nothing to hide behind).

## 3. VR feel and comfort (needs the user's hands in the headset)

- **Comfort pass with the user:** vignette strength, snap vs smooth turn defaults, Kabuto's eye height and
  speed, the Reaperski view turning with the ski, and the jetpack's vertical acceleration.
- **Teleport locomotion:** DONE 2026-10-28 (harness): Options -> MOVEMENT: TELEPORT - an arc from the move hand, a
  blink with a fade; on foot only, races keep smooth movement. Needs the user's feel test.
- **Seated mode:** DONE 2026-10-28 (harness): Options -> PLAY SEATED raises the view and hands 0.45 m. No crouch
  button (the ground reach assist covers pickups).
- **Hand presence:** 2026-10-28: the index finger from the trigger's capacitive touch (pointing). Thumb: not
  done (which of the 4 digits is the thumb is not verified). Hand tracking for menus: DONE 2026-10-28 (BUILD
  only): with the controllers down, the hands' aim rays point at the main menu and Options, a pinch selects, the
  left hand's menu gesture opens Options. The menus also follow the controllers' rays now (harness).
- **Haptics everywhere:** per-weapon kicks were already there; 2026-10-28: Kabuto's footfalls by size, the gun's
  kick in the steadying hand too. The bow string tension already rises with the draw.
- **Weapons in the hand:** 2026-10-28: the rounds left on top of the gun, the arrows by the bow's grip (harness).
  Reloading by gesture: not done (the original has no reload).
- **Delphi's spells by gesture:** the circle and sweep exist; 2026-10-28: SUMMON HAIL (hand raised high, brought
  down hard) and SLOW TIME (both hands together before the chest), with the original's EFX_hail_missile /
  EFX_slow_spell models, on the CONTROLS card (harness). The data names more Reaper spells (GText iconrs*: Cloak,
  Cluster, Fire Circle, Follow, Repair, Sea Monster, Shadow, Shrink, Teleport) - candidates. 2026-10-28: CLOAK and CLUSTER
  (r4a hands them to her in the data). 2026-10-28: FOLLOW (point, pull back, swing, throw; the followspell clips) and FIRE CIRCLE (arms out; EFX_circle_spell). Baz's Repair Gun (item 19, weapon 19 "-50 every 1.5 s") is in the data but no
  mission gives it - done 2026-10-28 (base missions), with homing rockets (item 15) and the portable turret (item 11,
  mc_turret: planted, turns and fires; from Story 3 on). Still in the table and unused: proximity rockets (16), the
  motion mine (13), the shield (17), jetpack 2 (18).
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
- **Music:** the mood selection (ambient / small / large battle / victory) was done earlier; 2026-10-28: a battle
  closes on its ending phrase, dying plays the death song (device to hear).
  transitions should be beat-aligned where the tracks allow. 1 session.
- **`.abx` object sets (B4):** decode them and check whether missions use objects we don't place yet.
- **Cutscenes:** lip-sync: 2026-10-28 survey (`gzptool flick` with FLICK_GREP): the original's shots already animate
  the mouths - 384 "mouth" tracks (Smarties, Vimps, Timmy...), 139 "Lip_bottom" (Reaper women), 71 "jaw_lower"
  (Reaper men), 84 "Jaw" (Kabuto) move in the director data, which our playback applies by bone name. So no
  loudness-driven jaw (it would fight the animators' keys); to check: that those bones deform the meshes in a close
  shot. Actors' idle loops between lines: not done.
- **Subtitles:** the original's text for every voiced line (the data has the strings), timed to the audio. 1 session.

## 5. Content and modes

- **Multiplayer (branch `next`):** two-headset LAN test with the user (needs a second Quest or a PC
  build). Merge `next` into `rework` once the rework's device run is clean. Voice chat is out of scope.
- **Skirmish vs bots on all 12 maps:** a balance pass from the logged capture counts. 2026-10-28: first pass (kill
  limit 20, Kabuto fumbles and slows with a flag) from a bot-only survey; the big maps stay slow with bots alone.
- **A PC "spectator" build** (the harness with a window, mouse and keyboard). It's a cheap second player
  for multiplayer tests and lets the user watch a test without the headset. 1–2 sessions.
- **Save slots:** DONE 2026-10-28 (three, on the CONTINUE row). Checkpoints within a mission: DONE 2026-10-28 (the
  last objective's spot; in memory, not in the save file).
- **Difficulty levels:** DONE 2026-10-28 (damage taken and Reaper aim).
- **Mission select unlocks:** DONE 2026-10-28 (progress.txt; each hero's story up to its first unfinished mission).

## 6. Test and build infrastructure

- **The whole test on a schedule:** 2026-10-28: devtest.ps1 writes report.html (a coloured row per test with its
  screenshots). Trends against the last run: review_results.py.
  the results folder: per set, per test, trends against the last run, with screenshots. 1 session.
- **Harness parity:** run every device script in the harness at 72 Hz before each APK (the `runall` +
  `expect3` scripts) as part of `scripts/build_apk.sh --check`. 0.5 session.
- **Frame-rate-independence tests:** 2026-10-28: every script run at 36 Hz in the harness (found the stale aim laser).
  parry-style bugs before the device does. 0.5 session.
- **Crash reporting:** DONE 2026-10-28 (ed4ed9e, BUILD only): signal, backtrace and the last 160 log lines in
  `files/crash/crash_<time>.txt`; devtest pulls it; `experiments/crash_report` checks it on the headset.
- **Official toolchain (B2):** rebuild with Google's NDK r27c and build-tools before any public release.
- **Code health:** DONE 2026-10-28 for the first pass: `game_app.cpp` (5,200 lines) split by system into
  `game_app_{audio,gore,mission,cutscene,combat,render}.cpp` (whole functions moved, 2,300 lines left: init, play
  mode, characters, the update). There have been three code reviews; make
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
Kabuto's microphone roar (Options); later the same day: a PRACTICE range on the main menu, Options -> MAIN MENU.
Still open from the brainstorm: tabletop base building, the Reaper cover test,
the jetski_3 route.
