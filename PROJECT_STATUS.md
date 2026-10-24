# Project status — GiantsVR (unofficial Quest 2 port)

Last updated: 2026-10-24 (all 545 cutscene shots parse; cutscene effects (op 0x27); carried weapons held in cutscenes; APK 97241e5 delivered; next automated run pending)

**Summary:** A native Quest APK (OpenXR + GLES 3, arm64) runs on the user's Quest 2 at 72 fps. It loads the user's
original Giants data and renders the original worlds in stereo.
- **Verified on device:** first-person Meccaryn (body and IK arms), jetpack, the gun, Reaper enemies, collision and original audio.
- **New, verified in the headless harness only:** the mission setup stream and object definitions are fully decoded.
  - Story1 is populated from the original mission data: 300 objects (palm groves, all Smartie houses, Ripper nests).
  - The player starts at the mission's start marker.
  - Timmy's Rippers attack, with the original clips and sounds; Ripper behaviour is our own.
  - Not yet tested on the headset.

| # | Milestone | Status | Evidence |
|---|---|---|---|
| M0 | Environment & repo inspection | DONE | ENGINEERING_LOG 2026-10-21 |
| M1 | Source / asset investigation | DONE | ARCHITECTURE.md strategy table; docs/formats/* |
| M2 | Architecture + dependencies | DONE | ARCHITECTURE.md, scripts/fetch_deps.sh (sha256 pinned) |
| M3 | Native Android/OpenXR project builds | DEVICE VERIFIED (launch + session) | `scripts/build_apk.sh` → signed APK (v3 signature + zipalign verified by apksig) |
| M4 | Stereo rendering + head tracking | DEVICE VERIFIED (runtime side) | Quest 2: 1440×1584 sRGB swapchains, LOCAL_FLOOR, FOCUSED, steady 72 fps. User confirmed the island, sky, sea and characters are visible in the headset. |
| M5 | Quest controller input | DEVICE VERIFIED (binding) | touch_controller profile bound on both hands. User confirmed movement, snap turn and trigger haptics work. |
| M6 | Asset pipeline | BUILD VERIFIED (host) | gzptool validate: 5291 entries; 1561/1561 GBS, 1001 TGA, 32/32 GTI, 782/782 SKN, 1318/1318 ANM parsed |
| M7 | Original assets render correctly | IN PROGRESS (device feedback applied: mirrored bones, key values, brightness) | Terrain/sky/sea/fog render; 4 characters animate (rigid skinning verified exactly against GBS rest positions). Still provisional: lighting rules, scale (0.2 m/unit), texture tiling and TCB interpolation (linear/slerp used). Object placement DEVICE VERIFIED (device test 3: village buildings, raft and canopy look as intended). |
| M8 | Original engine/gameplay integrated | IN PROGRESS | Object definitions and mission setup streams fully decoded (807 types; 56/56 missions); mission creatures placed and animated. Original AI/objective logic is native code, not decoded (B9). |
| M9 | Meaningful original gameplay | IN PROGRESS | Player Meccaryn: run/strafe/backpedal/jetpack with original clips and cross-fades. First person (default) with full body, arm IK to the controllers and finger curl; third person optional (DEVICE VERIFIED, test 7: "Now it's perfect"). No weapons, AI or missions yet. |
| M10 | VR cameras & controls integrated | IN PROGRESS | Smooth move, snap/smooth turn, left-handed swap, terrain following |
| M11 | Performance & stability | NOT STARTED | No on-device profiling possible yet |
| M12 | APK packaged & validated | BUILD VERIFIED | Manifest/badging, arm64 ELF, exported `ANativeActivity_onCreate`, NEEDED libs checked |
| M13 | Quest 2 hardware verification | IN PROGRESS | Device test 1 (TEST_REPORT.md): runs on the user's Quest 2 at a stable 71.8 fps / 72 Hz, Touch controllers bound. Start-up log clean: Oculus runtime 85.200.0, all 129 archives mounted, world loaded in 0.53 s. Waiting on the user's description of what they saw. |

## Next three priorities
**Cutscenes, 2026-10-24 (host verified, not device-tested):** all 545 shots from the user's data parse; every
mission's opening cutscene (33) plays in the harness without errors; effects on actor nodes (op 0x27) show with
stand-in particles; weapons carried by cutscene actors are held in the hand instead of floating over the head.
**Also done 2026-10-24:** Delphi and Kabuto have first-person bodies with arm IK; Delphi's fire wall, bubble and
tornado; all 56 missions pass a headless sweep (scripts/mission_sweep.sh). Kabuto's story powers (31f0064 and the
magnifying-glass build): giant walls that fall only once he's grown, the adrenaline attack (both grips), Offspring eggs
(hold Y), and the magnifying glass from the giant crystal that burns through the crystal gate (kstory3_3, kstory4_1).
A push-out bug that could fling a hero out of a structure overlap is fixed. Kabuto also throws: boulders off the
volcanic rock pillars (kstory3_2's barracks) and Reapers at each other (hold X, let go). Host verified only.

**Saved-for-later items, done 2026-10-24 (host verified, waiting for device test 26):**
- Story 2's bush-pack disguise (14386c1).
- Flares against homing missiles, from any level's Shop (41f2a89).
- Kabuto growing from eating (3886e95).
- Host-side performance pass (075a50f): update cost halved in busy scenes. **Still needs device numbers** (Options ->
  FRAME RATE) in 1a's village, at a tower collapse and in flight; draw calls (~470 per eye in 1a's palm groves, no
  multiview yet) are the likely next cost on the headset.

**Done (user list, 2026-10-23 16:45; host verified, build 8515055, waiting for device test 26):**
- Rippers burrowing, mini Rippers, latching, faster spit, spit health drops.
- Building health bars and the collapse frame drop.
- VR map (Y).
- Timmy fighting on his tower.
- Jetpack plasma and the out-of-fuel sputter.
- Piranha gore.
- Name tags and Smartie lines.
- Shop and health pool (1d).
- Water interaction.
- HUD injection number.
- TP weapons attached to the hands.
- Projectile colours.

1. **Device test 25** (build f811216, host verified only): Timmy leaning; Timmy's lines and subtitles; the new terrain
   (beach, sea bed, earth patches, rock on slopes); **frame rate with the terrain shader** (watch `perf: ... quality down`).
   The three requests from device test 24 are implemented.
   - **Then, in priority order:**
     1. ~~Finish Story 1~~ (5b56bea, host verified): village Smarties, flying Dactyls, 1d → Story 2. Next for Story 2:
        Vimp-meat and destination objectives (B9) and the world opcodes that stop Story2/3/5's streams (B5).
     2. ~~Save and checkpoints~~ (e3d512c, host verified).
     3. ~~In-headset settings menu~~ (0a8b1c0, host verified).
     4. Performance pass on the headset: per-effect cost in the busiest scenes, keep 72 fps with headroom, maybe 90 Hz.
        **Needs device numbers:** turn on options → FRAME RATE and report it in 1a's village, at a tower collapse, in flight.
     5. ~~VR feel~~ (8626e3c, host verified).
     6. ~~Visual upgrades~~ (2d57afb, host verified): day/night with stars and moon, anisotropic filtering. Light shafts skipped.
     7. ~~Next characters~~: ~~Delphi~~ (93a8eed, host verified; FP body still missing), ~~Kabuto with his own camera
        scale~~ (866dfdc, host verified; FP body not drawn, Kstory mission objectives not implemented).
     8. ~~Polish~~ (42ac61a, host verified): ~~main menu~~ (intro island, 3D logo), ~~loading screens with the original
        art~~, music per world (a1/a2 by chapter is our mapping; the original's choice is native code, not decoded).
   - Resume planned 2026-10-23 00:31 (user).
2. ~~World opcode table (B5)~~ (2026-10-23): all 33 worlds parse to the end. Later chapters gain their full scenery
   and their scenario lists, which give the original campaign order: Story1–5 → Rstory1–5 → Kstory1–5 → the_end.
3. **Remaining Story1 work:** Smartie AI in the villages, Dactyl flight, 1d (sneak past); the world opcode table (B5).
3. **Visual fidelity:** compare against original screenshots (B6).

## Resume instructions
1. `scripts/fetch_deps.sh` (or reuse `../toolchains`), then build host + APK (see CLAUDE.md).
2. Original data: stage the user's `C:\GOG Games\Giants - Citizen Kabuto\Bin` (all 899 files,
   ~440 MB) into the workspace. Keep it outside the repo.
3. Check `git log` and ENGINEERING_LOG.md before changing anything.
