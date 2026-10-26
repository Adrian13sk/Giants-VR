# Project status — GiantsVR (unofficial Quest 2 port)

Last updated: 2026-10-26. **The rework is under way on branch `rework`** (the user's play-test: "boring, not VR-ish enough, ugly; cutscene actors misplaced; Kabuto starts wrong, can't finish, too slow"). The classic version is tag `classic-final` (2e3cf55, branch `main`). What the rework changed and its status: docs/REWORK.md. Latest rework APK: GiantsVR-rework-4514eda.apk (on the PC, sha256 922aef1d... verified; not yet run on the Quest; adds to 0851cd3: a fix that makes ~15 sounds play that never did (birds, water plops, several Kabuto, flare, base and climbing sounds), the sea monster (the unused 30 m serpent surfacing out at sea), hitchers (the unused perching bird riding the Vimps), leftover bones after meals. 0851cd3 added to 0e72a0c: Kabuto grabs swooping Lobirds out of the air and eats them, pat / high-five a Smartie, lightning and thunder in the storm and an overcast sky in rain and snow, fixes from an independent code review, bird/fish skins loaded once. 0e72a0c added to 8c464bb: fish (Shiner schools in the shallows, seen through the water; hold a hand still in the water and squeeze to catch one, throw it), ambient sounds from the levels' own ambient markers (surf, wind, leaves, water, volcano) and weather (light rain on Rstory1, snow on Rstory2, a storm on Kstory5) - Options -> AMBIENCE, cloud shadows, skinned models' GPU memory freed (B15 leak). 8c464bb added to d226add: ambient birds - Flak flocks that peck about and take off when you come near, shoot or blast, Lobirds circling high, birds shot or stoned down in a puff of feathers; scorch marks under explosions, Kabuto's footprints, spent casings, arrows that stay where they strike, Smarties afraid of you for 40 s after a kill. d226add added to 30081f9: Options -> CONTROLS (each hero's controls on a card), the sky reflected in the water at the reduced quality level; INSTALL.md and REWORK.md on the PC updated. 30081f9 added to 3889d03: Reaperski handlebars, arm-stroke swimming, Smarties/guns/limbs thrown at Reapers, one-time hints, and six fixes from a code review (thrown kills now complete objectives, snap turns no longer throw). 3889d03 added to 01be860: stones to pick up and throw, dropped guns and cut limbs to grab and throw, disarmed Reapers going back for their guns, Kabuto picking up and eating dead Reapers, ragdolls splashing into water, the GROUND REACH option. 01be860 added to 5a7e7ce: grabbing bodies by hand - lift, drag, swing, throw, a thrown body knocks a living Reaper back; landing/cut/gun-drop sounds; shots through the gun count as gun hits; the editor's red marker cones hidden. 5a7e7ce added to d9f7424: the cinema cutscenes, NPC ragdolls, dismemberment below; Reapers hiding behind trees and ducking behind rocks, ground stones, rock anti-tiling, facet-normal rock projection). Since e7e1721 it adds the user's 2026-10-25 list: barracks and nests spawn Reapers/Rippers, placed Rippers buried, arms follow the controllers (Delphi bigger hands, sword in hand; Kabuto), Kabuto carries trees/Smarties/Vimps/creatures, the bow held in the left hand and drawn with the right, Delphi not eaten by piranhas / Kabuto drowns, Smarties killable and grabbable with reactions, Vimp souls and rebirth, sword impale and slash dismemberment, Reaper ragdolls, new grass and rock (graphics pass 1). In 496591a since d9f7424 (2026-10-26 night, harness only): cutscenes on a cinema screen from the shot camera (Options -> CUTSCENES), all 66 cutscene chains played to the end and their cast fixed (Delphi/Yan/Timmy/wife/Smarties/Sonaks no longer Meccaryns or missing, trailing black trimmed, portals glow); ragdolls for Smarties, Vimps and creatures; dismemberment (Delphi's killing slash cuts the limb off); dry tufts on non-green worlds; alpha-to-coverage foliage on the headset. Harness: 56/56 missions clean. Next device step: `devtest.ps1 -Apk ..\GiantsVR-rework-4514eda.apk -Tests <the list in docs/REWORK.md>`.

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
**Multiplayer, branch `next`, 2026-10-25 (host verified; online play verified only between two copies on one
machine):** Main menu -> MULTIPLAYER. The original's 12 multiplayer maps (3-way, Meccaryn, Reaper) with teams,
flag holders, captures and kills; computer players that route over the terrain, fight, take and return flags.
HOST / JOIN a game on the same Wi-Fi (our own protocol; not the original's network play). Next: the user's
headset check of the skirmish; two-headset test when possible; balance.
**2026-10-25 night fixes (host verified):** bots take and capture flags (0 -> 11-19 captures per 12 maps x 10
minutes): Reaper flags stood inside the command centre, the route grid ignored the water's surface, stuck cells
are learnt. A review of the multiplayer code: 13 findings fixed (players who left, late packets, lost events,
kill credit, joiner bodies, reconnect). Reapers drop ammo for a weapon carried.

**Branch `next`, 2026-10-24 evening (host verified, not device-tested; master stays at the d295fa1 APK the user tests):**
`tools/py/review_results.py` summarises a device run on one page and compares it with earlier runs. Original
effects (Options test switch) now include the projectiles' trails (fx.bin op 24). Delphi's mine is her weapon
cycle's last slot. A code review of base building and Kabuto found 4 real bugs (indices into shrinking lists,
phantom fist punches after a teleport / snap turn, Offspring outliving Kabuto, shop rewards repeating on rebuild):
fixed. Plan: device results first (fixes on `next`), then merge and build.
**Meccaryn arsenal, 2026-10-24 (host verified, not device-tested; no APK built yet):** weapondefs.bin decoded (weapon
and item tables). The missions now hand out the original's guns: shotgun (1c on, shell pickups in Story 1-2), sniper
gun (story3_bunker_3), rocket launcher (story3_launcher_5, was 1c - an earlier misreading), Millenium mortar (4a),
machine gun and grenades (5a; thrown by hand), with damage / rate / ammo from the records; the sniper's side screen
shows a 4x zoom when raised to the eye. Delphi gets her bows from the same table (bow, RPG bow, sniper bow; drawn
by hand in VR). Since then: the launcher's rockets are counted (8, 12 most), mines are sold in Shops, and the Spellshop sells
Delphi's homing and power-up bows; Delphi's mine and Reapers dropping ammo are on branch `next`.
**Base building v1, 2026-10-24 (host verified, not device-tested):** mission 5c builds the Meccaryn base from the
game's own template and build animations; Build Pad sign menu touched by hand, Vimp meat store. Next: collision,
base attacks, pop-up bomb, Delphi's base (r4a).
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
