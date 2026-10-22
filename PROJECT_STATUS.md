# Project status — GiantsVR (unofficial Quest 2 port)

Last updated: 2026-10-22 (build d07dbb9: fuel, Timmy riding backwards)

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
1. **Device test 24 (d07dbb9): Timmy is better.** Next session, in this order:
   - **Timmy leans with Baz:** take his rotation from Baz's spine relative to its idle rotation, so he tilts
     forward in forward flight. (A first try in d07dbb9 laid him sideways; check the spine's axes in the game frame.)
   - **Timmy voice with subtitles:** play Smartie voice lines (gsmNN_1/2 and other sm voice WAVs) with his waves and
     yells, shown with the existing subtitle panel; find his text lines in the game's text files.
   - **Terrain texture variety:** beach sand by height near sea level, sand under water, several grass and rock
     variants blended by slope, height and noise, plus detail textures. Use original textures where the data has
     them; all in the terrain shader.
   - **Then, in priority order:**
     1. Finish Story 1: village Smarties going about their day, flying Dactyls, the 1d sneak mission.
     2. Save and checkpoints between missions.
     3. In-headset settings menu: comfort, smooth/snap turn, left-handed mode, HUD size, volume.
     4. Performance pass on the headset: per-effect cost in the busiest scenes, keep 72 fps with headroom, maybe 90 Hz.
     5. VR feel: controller vibration for hits and jetpack take-off, weapon holster points on the body.
     6. Visual upgrades: sharper sky, water foam, light shafts, a short day/night cycle.
     7. Next characters: Delphi, then Kabuto with his own camera scale.
     8. Polish: main menu, loading screen with art, the original music per mission.
   - Resume planned 2026-10-23 00:31 (user).
2. **Remaining Story1 work:** Smartie AI in the villages, Dactyl flight, 1d (sneak past); the world opcode table (B5).
3. **Visual fidelity:** compare against original screenshots (B6).

## Resume instructions
1. `scripts/fetch_deps.sh` (or reuse `../toolchains`), then build host + APK (see CLAUDE.md).
2. Original data: stage the user's `C:\GOG Games\Giants - Citizen Kabuto\Bin` (all 899 files,
   ~440 MB) into the workspace. Keep it outside the repo.
3. Check `git log` and ENGINEERING_LOG.md before changing anything.
