# GiantsVR — definition of "finished" (release checklist)

Written 2026-10-29. "Finished" means: every item below is met, and the last one, the headset run, passed on the
APK that is released. Statuses: **HARNESS** (the cloud test harness: the same game code, software-rendered, scripted
input), **BUILD** (compiles and packages), **DEVICE** (ran on the Quest 2). Nothing counts as finished on HARNESS alone
when it depends on the headset (frame rate, comfort, controllers, sound on the Quest, Wi-Fi).

## A. Content (what the original game has)

| # | Criterion | Status |
|---|---|---|
| A1 | All three campaigns playable start to end: Meccaryn (Story1-4), Delphi (Rstory1-5), Kabuto (Kstory1-5), the 56 missions and the ending | HARNESS: campaign run 55/55 to the_end; sweep 56/56 clean. DEVICE: whole campaign completed 2026-10-27 (bac967b) - but that run marks missions complete (AUTOCOMPLETE) |
| A1b | Every mission finishable **by hand** (objectives done with the real controls, not marked complete) | HARNESS 2026-10-29: all but k2d_offspring (walked earlier), kstory3_4 (DEVICE, B18), rstory2_jetski_3 (B21: route unknown). This check found four missions that could not be finished: story3_prison_6 (no lift), r1_saveyan / r1_smartiegrab / r1_turbo (rescues with no drop-off), r4a's ships (couldn't be hit) - all fixed. devtest/reach + the mission bot |
| A2 | Every inventory item in the data's item table works (weapondefs.bin, 73 items) | HARNESS: all but Motion Bomb (13) and Laser (4), which the data names but gives no behaviour; mines and the shotgun cover them |
| A3 | Every Delphi spell (items 21-33, 73) by gesture | HARNESS (all 14); DEVICE for the ones in bac967b (fire bolt, wall, tornado); the rest pending the next run |
| A4 | Cutscenes: all 66 chains play with the right cast; subtitles; skippable | HARNESS; DEVICE (cutscene set ran 2026-10-27) |
| A5 | Bases (build, crews, raids, repair), shops, pop-up bombs, vehicles (Reaperski, gyrocopter) | HARNESS; DEVICE for bases/shops/ski in bac967b |
| A6 | Skirmish vs computer players on the multiplayer maps; LAN play between headsets | HARNESS (bots; two copies on one machine incl. spells, 2026-10-29). Two headsets: not tested (one Quest) |

## B. Quality gates (re-run on the release commit)

| # | Criterion | Status |
|---|---|---|
| B1 | Every devtest script passes in the harness at 24 Hz and 72 Hz (tests, weapons, ui, npcs, missions) | HARNESS (3 known harness-only misses: GPU-cost / touch / ambience lines the harness can't produce) |
| B2 | Mission sweep: 56 missions load and run with no errors or warnings | HARNESS |
| B3 | Address/undefined-behaviour sanitizers clean over every devtest | HARNESS (on the release commit: see TEST_REPORT.md) |
| B4 | Unit tests | HARNESS |
| B5 | Independent code reviews at each milestone, findings fixed or recorded | Done 2026-10-29 (release review: 9 findings, all fixed) |
| B6 | Saves can't be lost: atomic writes, a new game keeps the old save until the new world loads, no one-button delete | HARNESS (unit test + code) |
| B7 | Headset lifecycle: pause on the Quest button / Guardian, resume, headset off and on | HARNESS for the pause; DEVICE pending |

## C. On the headset (needs the user's Quest 2)

| # | Criterion | Status |
|---|---|---|
| C1 | THE WHOLE TEST on the release APK: `devtest.ps1 -All` - every script finishes, 0 crashes, 0 errors | DEVICE on bac967b (247/247, 0 crashes); pending on the release APK |
| C2 | Frame rate: median >= 70 fps, no test under 60 | DEVICE on bac967b: median 71.8, 3 under 60; pending |
| C3 | Hands-on checks: PLAYTEST.md sections 5-6 (comfort, gestures, menus) | pending (the user) |
| C4 | Memory flat over the campaign (~350 MB) | DEVICE on bac967b; pending |

## D. Release package

| # | Criterion | Status |
|---|---|---|
| D1 | APK built from a clean, tagged commit; symbols kept (build/symbols/<commit>.so) | BUILD |
| D2 | INSTALL.md for other owners of the game (GOG Bin copy, sideloading, controls) | done |
| D3 | Third-party notices (docs/THIRD_PARTY_NOTICES.txt) next to the APK; "unofficial, not endorsed" on the main menu and in the README | done |
| D4 | No game data in the repository or APK | checked (.gitignore, the APK holds only our library and the OpenXR loader) |
| D5 | This project's own licence; whether and where to publish | the user's decision |
| D6 | Official toolchain (Google NDK / build-tools) before any public release (B2) | blocked in this environment; the user's PC could build it |

## What is not in scope (stated, not hidden)

- The original's network play (DirectPlay) - ours is LAN between our own builds.
- Quest 3 / 3S tuning (no device).
- Rendering details that need the original running side by side (B6 in BLOCKERS.md), the Reapers' original AI rules
  (B9), and rstory2_jetski_3's intended route (B21) - each needs a recording of the original.
