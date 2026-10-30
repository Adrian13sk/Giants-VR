# Giants-VR

**A native, standalone VR port of *Giants: Citizen Kabuto* (2000) for the Meta Quest 2.**

An ambitious one-man project that brings the world of Giants into immersive virtual reality. It runs directly on the
headset, with no PC and no streaming, and loads the original game's own data: its worlds, characters, animations,
missions, cutscenes, music and sounds. You play them in stereo VR with head tracking and Quest Touch controllers.

> **Unofficial.** This project is not affiliated with or endorsed by Planet Moon Studios, Interplay, GOG or Meta. The
> repository and the APK contain **no game data**: you need your own legally obtained copy of the game (GOG or
> Steam). The original game's assets remain the property of their rights holders.

**DEVDEMO SHOWCASE** (an early build, 21.10.2026): https://youtu.be/8XQV-6sYn4s ·
[video](https://github.com/user-attachments/assets/a25e7a5d-a0cb-4da0-962e-dd3404b29bfc)

---

## Status at a glance

| | |
|---|---|
| **Latest build** | `GiantsVR-FINAL-368d43d.apk` (see [Releases](../../releases)) |
| **Runs on the headset** | Yes. The whole automated test ran on a Quest 2 on 2026-10-27: 247 scripted runs, all three campaigns played to the end in one session, **0 crashes**, median **71.8 fps** at 72 Hz. |
| **Campaigns** | Meccaryn (Story 1-4), Sea Reaper / Delphi (Rstory 1-5), Kabuto (Kstory 1-5) and the ending: **56 missions**. |
| **Finishable by hand** | Every mission's objectives have been done with the real controls in a test, except three with known reasons (see [Known issues](#known-issues-and-limits)). |
| **Quality gates** | ~190 scripted tests pass at 24 and 72 fps in the desktop harness; all 56 missions load and run clean; the whole campaign completes 55/55; AddressSanitizer / UBSan clean over every test; unit tests pass. |
| **What "verified" means here** | **DEVICE** = run on a Quest 2. **HARNESS** = the same game code run in a desktop test renderer with scripted controllers. Features added since the 2026-10-27 headset run are HARNESS until the next one. |

The game's original native code is not available. This is a **reimplementation** that reads the original data
files. Formats were reverse-engineered and checked against the real files. Where the original's rules live only in
its code (AI, damage numbers, timings, some mission logic), the behaviour here is our own reading. [BLOCKERS.md](BLOCKERS.md)
lists where.

---

## Features

### The original game, from its own data
- **Worlds:** terrain, sky, sea, fog, lightmaps, every placed object, and destructible scenery (trees, rocks,
  houses, towers, barracks, prisons, ships).
- **Characters and animation:** the original skinned models and animation clips. Rendered characters include
  Meccaryns, Sea Reapers (male and female), Kabuto, Smarties, Vimps, Rippers, Sonaks, Offspring, Lobirds, Flak birds,
  fish, the sea monster and more.
- **Missions:** objectives decoded from the mission scripts: reach, kill, destroy, rescue, bring Vimp meat, races,
  gates opened by size or adrenaline, bosses. There are also checkpoints, mission select, save slots and the
  finished mission's numbers.
- **Cutscenes:** all 66 cutscene chains play from the original director data, with the right cast, subtitles and
  voice. They play on a cinema screen in front of you, or around you.
- **Weapons and items:** from the game's own `weapondefs` tables.
- **Sound:** the original music with its moods (ambient, battle, victory, death), sound effects, voices, and
  ambience from the levels' own markers. Positional audio uses binaural cues.
- **Effects:** the original effect definitions for explosions, trails, splashes, portals and spells.

### Three heroes

**Baz (Meccaryn)**
- Jetpack with fuel, overheat, nitro, sputtering and hard landings, plus the jetpack upgrade.
- Guns:
  - handgun, shotgun, machine gun, sniper rifle (with scope), Millennium mortar;
  - rocket launcher with plain, homing and proximity rockets.
- Other kit:
  - grenades, mines, flares, health injections;
  - the bush-pack disguise, the shield, the portable turret and the Repair Gun;
  - pop-up bombs.
- The gyrocopter, flown with the sticks or by tilting the controller.

**Delphi (Sea Reaper)**
- Sword combat:
  - cutting by swing speed, thrusting to impale, dismemberment;
  - parrying Reaper bolts with the blade.
- Bows held in the left hand and drawn with the right: bow, RPG, sniper, homing and power-up bows, plus fire arrows
  that set grass ablaze.
- **Every spell in the game, cast by gesture:**
  - fire bolt, fire wall, tornado, hail, slow time, fire circle;
  - cloak, cluster bolts, FOLLOW (lift a Reaper and throw him), SHRINK;
  - summon sea monster, repair, teleport, summon shadow warrior.
- Movement: turbo running, diving and swimming, the Reaperski with handlebars, races.

**Kabuto**
- A true giant's scale and view, with a GIANT or LOWER option.
- Grab, carry, eat (to heal and grow), throw (Reapers, Smarties, trees, boulders).
- Punch, stomp and ground-pound; roar, including into the headset's microphone if you enable it.
- Adrenaline, laying eggs for Offspring you command, catching Lobirds out of the air, climbing cliffs.
- The magnifying glass; gates that open only at the right size.

### Made for VR
- **Your hands are the hero's:**
  - punch, grab and throw bodies, stones, guns and limbs;
  - pick up a Reaper's dropped gun and fire it;
  - swim with arm strokes, climb by gripping rock.
- **Your body is the inventory:**
  - holsters at the hips and over the shoulder, a weapon wheel;
  - a wrist display (lift your forearm to see health, ammo and goal);
  - inject health into your other arm.
- **Combat:** the aim laser, ammo counters on the guns and bows, Reapers who take cover and flank, hit reactions,
  ragdolls.
- **Comfort options:**
  - smooth or teleport movement, snap or smooth turning;
  - a comfort vignette, seated mode, left-handed mode;
  - first or third person.
- **Presence:** hands in the water, rain and snow, lightning, cloud shadows, birds that take off when you come near,
  fish schools, footprints and scorch marks.
- **Menus:** pointed at with the controller or bare hands. Options include subtitles, music and volume, frame rate,
  foveated rendering and difficulty. A controls card per hero describes every gesture.
- **System behaviour:**
  - the game pauses when the headset's own menu or Guardian takes focus;
  - saves are written safely (never half-written);
  - a practice range on the main menu.

### Base building and multiplayer
- **Bases:** for Baz and for Delphi, with crews, a shop, defences, raids, a table map you build on by touch, and a
  gyrocopter pad.
- **Skirmish** on the multiplayer maps against computer players, with capture-the-flag and kill limits.
- **LAN play between headsets:** our own protocol, not the original's network play. Spells and punches count
  against other players.

### Graphics and performance
- Stereo multiview rendering, 4x MSAA, and a reworked look: lighting relief, haze, water reflections, shadows, grass
  and rocks.
- Automatic quality levels per world so that each holds 72 Hz on the Quest 2, and dynamic foveated rendering.

---

## Known issues and limits
- **Not yet played on the headset:** the fixes from the latest sessions. These are:
  - the Guards' lift in story3_prison_6;
  - Delphi's first rescue missions;
  - r4a's ships;
  - the horn guards;
  - the new sword angle, turbo feedback and the Reaper model pool.
- **Missions not yet finished by hand in a test:**
  - **k2d_offspring:** Kabuto's autopilot gives up on the way to the portal, though an earlier test reached it.
  - **kstory3_4:** needs Kabuto's climb, which works on the headset.
  - **rstory2_jetski_3:** the intended route to the moored Reaperski is unknown. A recording of the original would
    settle it.
- **Rules that are ours:** AI behaviour, damage numbers and many timings are our own reading. Two items the data
  names but doesn't describe (Motion Bomb, Laser) are left out.
- **LAN multiplayer** has only been tested between two copies on one machine, not between two headsets.
- **Quest 3 / 3S:** expected to run but not tested or tuned.
- **Toolchain:** the APK is built with a checksum-pinned community NDK build, because Google's servers are
  unreachable from the build environment. A public release should be rebuilt with the official NDK.

---

## Install (Quest 2)
1. Enable **Developer Mode** for your headset in the Meta Horizon phone app.
2. Install the APK from [Releases](../../releases):
   ```
   adb install -r GiantsVR-FINAL-368d43d.apk
   ```
3. Copy **your own** game's `Bin` folder (about 440 MB). Optionally also copy `Music`, `Stream` and `StreamEnglish`
   for music and spoken dialogue:
   ```
   adb push "C:\GOG Games\Giants - Citizen Kabuto\Bin\." /sdcard/Android/data/io.github.giantsvr.dev/files/Bin/
   ```
4. Launch **Giants VR (unofficial dev)** from *Library → Unknown Sources*.

Full guide, controls and every gesture: [docs/INSTALL.md](docs/INSTALL.md). Hands-on checklist:
[docs/PLAYTEST.md](docs/PLAYTEST.md).

---

## Build from source
```
scripts/fetch_deps.sh                       # toolchain (checksum-pinned) into ../toolchains
cmake -S . -B build/host -G Ninja && ninja -C build/host && build/host/gvr_tests
scripts/build_apk.sh --check                # → build/apk/GiantsVR-dev.apk
build/host/gzptool validate /path/to/Bin    # check your game data
build/host/headless_render --data /path/to/Bin --world Story1 --out story1.png
```
The headless harness runs the same game code with Mesa/EGL and scripted controllers:
`headless_render --data <Bin> --script devtest/tests/<test>.txt`. The `devtest/` scripts also run on the headset via
`devtest/devtest.ps1` (results, frame times and screenshots pulled back to the PC).

## Repository layout
- `src/core` math, logging, profiling · `src/render` OpenGL ES 3 renderer · `src/assets` file formats (GZP, GBS, GTI,
  world `.bin`, SKN/ANM, mission and cutscene streams).
- `src/scene` terrain, model cache, worlds · `src/game` the game: heroes, AI, missions, VR interaction ·
  `src/xr` OpenXR · `src/platform/android` the entry point.
- `tools/headless` desktop harness · `tools/gzptool` archive and asset inspector · `tools/py` reference decoders ·
  `tests/` unit tests · `devtest/` scripted tests for the harness and the headset.

## Documentation
| | |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | Install, controls, every gesture |
| [docs/REWORK.md](docs/REWORK.md) | Every feature, with its verification status |
| [docs/RELEASE_CHECKLIST.md](docs/RELEASE_CHECKLIST.md) | What "finished" means, and where each item stands |
| [PROJECT_STATUS.md](PROJECT_STATUS.md) | Milestones and the build history |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Design and subsystem boundaries |
| [docs/formats/FORMATS.md](docs/formats/FORMATS.md) | Reverse-engineered file formats |
| [BLOCKERS.md](BLOCKERS.md) | Open questions and limits |
| [TEST_REPORT.md](TEST_REPORT.md) · [ENGINEERING_LOG.md](ENGINEERING_LOG.md) | Every test run and decision |
| [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) · [docs/THIRD_PARTY_NOTICES.txt](docs/THIRD_PARTY_NOTICES.txt) | Third-party code and licences |

## Credits and licences
Developed by Adrian13sk, with Claude (Anthropic) as the engineering assistant. Third-party code:
- the Khronos OpenXR loader and headers (Apache-2.0);
- `android_native_app_glue` (Apache-2.0);
- `stb_image` (MIT / public domain).

See [docs/THIRD_PARTY_NOTICES.txt](docs/THIRD_PARTY_NOTICES.txt). Format research used GiantZiP, GiantsTools and
vibe-giants-decomp as factual references only. No code was copied from them, and every claim was checked against
the real files.

*Giants: Citizen Kabuto* © Planet Moon Studios / Interplay. This project's own code has no licence chosen yet.
