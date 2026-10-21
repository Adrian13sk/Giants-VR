# Project status — GiantsVR (unofficial Quest 2 port)

Last updated: 2026-10-21 (session 1, object placement)

**Summary:** A native Quest APK (OpenXR + GLES 3, arm64) builds and is signed. It loads the
user's original Giants archives, decodes terrain, models, textures, skins and animations, and
renders original worlds (terrain, sky, sea, fog) with **animated original characters** in stereo.
**It runs on the user's Quest 2 at a stable 72 fps.** Device tests 1–2 confirmed the visuals,
movement, haptics and the animation fixes. Build 1c2b74f+ adds **the original level objects** (Story1 village:
135 fences, Smartie walls, tables, posts and plants). That is verified on the host only, with device test 3 pending. There is no
gameplay: no AI, weapons, mission logic, audio or menus.

| # | Milestone | Status | Evidence |
|---|---|---|---|
| M0 | Environment & repo inspection | DONE | ENGINEERING_LOG 2026-10-21 |
| M1 | Source / asset investigation | DONE | ARCHITECTURE.md strategy table; docs/formats/* |
| M2 | Architecture + dependencies | DONE | ARCHITECTURE.md, scripts/fetch_deps.sh (sha256 pinned) |
| M3 | Native Android/OpenXR project builds | DEVICE VERIFIED (launch + session) | `scripts/build_apk.sh` → signed APK (v3 signature + zipalign verified by apksig) |
| M4 | Stereo rendering + head tracking | DEVICE VERIFIED (runtime side) | Quest 2: 1440×1584 sRGB swapchains, LOCAL_FLOOR, FOCUSED, steady 72 fps. User confirmed the island, sky, sea and characters are visible in the headset. |
| M5 | Quest controller input | DEVICE VERIFIED (binding) | touch_controller profile bound on both hands. User confirmed movement, snap turn and trigger haptics work. |
| M6 | Asset pipeline | BUILD VERIFIED (host) | gzptool validate: 5291 entries; 1561/1561 GBS, 1001 TGA, 32/32 GTI, 782/782 SKN, 1318/1318 ANM parsed |
| M7 | Original assets render correctly | IN PROGRESS (device feedback applied: mirrored bones, key values, brightness) | Terrain/sky/sea/fog render; 4 characters animate (rigid skinning verified exactly against GBS rest positions). Still provisional: lighting rules, scale (0.2 m/unit), texture tiling and TCB interpolation (linear/slerp used). Object placement IMPLEMENTED (host-verified, awaiting device test 3). |
| M8 | Original engine/gameplay integrated | NOT STARTED | Needs object-type defs, SKN/ANM, simulation loop |
| M9 | Meaningful original gameplay | NOT STARTED | |
| M10 | VR cameras & controls integrated | IN PROGRESS | Smooth move, snap/smooth turn, left-handed swap, terrain following |
| M11 | Performance & stability | NOT STARTED | No on-device profiling possible yet |
| M12 | APK packaged & validated | BUILD VERIFIED | Manifest/badging, arm64 ELF, exported `ANativeActivity_onCreate`, NEEDED libs checked |
| M13 | Quest 2 hardware verification | IN PROGRESS | Device test 1 (TEST_REPORT.md): runs on the user's Quest 2 at a stable 71.8 fps / 72 Hz, Touch controllers bound. Start-up log clean: Oculus runtime 85.200.0, all 129 archives mounted, world loaded in 0.53 s. Waiting on the user's description of what they saw. |

## Next three priorities
1. **Device test 3:** the user checks the placed village objects in the headset.
2. **Placement follow-ups:** decode the mission opcode stream properly (replacing the heuristic
   scan), fix the world opcode table (B5) so Story2 etc. reach their op 42/92 records, and confirm yaw.
3. **First playable slice:** controllable Meccaryn (first/third-person VR camera), movement on
   terrain with original walk/run animations, then a first weapon. Also GPU skinning and TCB
   key interpolation.

Then: world opcode table fixes (24 of 33 worlds still stop early), Kabuto-scale camera
experiments, audio (WAV in VO_Sfx gzp), fixed-step simulation, Vulkan/multiview evaluation.

## Resume instructions
1. `scripts/fetch_deps.sh` (or reuse `../toolchains`), then build host + APK (see CLAUDE.md).
2. Original data: stage the user's `C:\GOG Games\Giants - Citizen Kabuto\Bin` (all 899 files,
   ~440 MB) into the workspace. Keep it outside the repo.
3. Check `git log` and ENGINEERING_LOG.md before changing anything.
