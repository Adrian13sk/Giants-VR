# Architecture

## Strategy decision (2026-10-21)

| Strategy | Verdict | Evidence |
|---|---|---|
| A. Direct engine port | **Not possible** | Source never released (community patch maintainer, Steam thread). No source on disk/GitHub. |
| B. Existing open engine | **None exists** | No public reimplementation found (`giants-rs` is referenced but not public). |
| C/D. Native reimplementation loading original assets | **Selected** | Formats documented & verified here: GZP/LZSS, GBS, GTI, world .bin (partial). Only route to true stereo 6DoF. |
| E. x86 emulation (box86/Wine) of Giants.exe | Fallback only | Would yield a flat virtual screen at best (violates "full 3D environments"), 32-bit x86 + D3D7/8 on Adreno 650 untested; render-DLL seam (`gg_*r.dll`) noted for future research. |

Gameplay/AI/mission logic lives in native code inside Giants.exe (mission `wm_*.bin` files are
data; cutscene `flk_*.bin` is bytecode). Those systems must be reimplemented incrementally,
using the decompilation as an *untrusted factual reference* and the original data as ground truth.

## Runtime layers

```
android_main (NativeActivity, EGL pbuffer context)          src/platform/android
  └─ XrApp: loader init, instance (KHR_android_create_instance, KHR_opengl_es_enable,
     FB_display_refresh_rate*, EXT_local_floor*), session lifecycle, reference space
     (LOCAL_FLOOR > STAGE > LOCAL-1.6 m), per-eye sRGB swapchains + depth RB, frame loop,
     XrInput (Touch + simple-controller bindings, grip/aim poses, haptics)   src/xr
  └─ GameApp (platform independent)                                       src/game
       ├─ VrRig: tracking→world transform, smooth locomotion, snap/smooth turn about the head,
       │         left-handed swap, world scale, terrain following
       ├─ AssetWorld: AssetFS (all *.gzp + loose Bin/), WorldDefinition (w_*.bin), Terrain
       │         (GTI → chunked meshes, lightmap), sea plane, sky band, fog, ModelCache (GBS+TGA)
       ├─ TestScene: synthetic engineering geometry + controller visualisation
       └─ Renderer: GLES 3.0 forward renderer (unlit/lit, alpha test/blend, linear fog)
```

### GameApp's files (2026-10-28)
GameApp is one class spread over `src/game/game_app*.cpp`, one file per system. `game_app.cpp` keeps init, play
mode and the camera, characters and the per-frame update; `game_app_mission.cpp` (mission setup, objectives,
checkpoints, loading screen), `game_app_combat.cpp` (waves, the player's damage and death, blasts, rockets),
`game_app_render.cpp` (sun, shadows, reflections, the eye views and world pass), `game_app_audio.cpp` (effects,
mix, music, streams), `game_app_cutscene.cpp`, `game_app_gore.cpp`, `game_app_title.cpp` (main menu, practice),
`game_app_menu.cpp` (Options) and ~50 smaller ones named after what they do.

### Multiplayer (2026-10-25)
```
GameApp (game_app_skirmish.cpp): the multiplayer maps (w_M_*), bodies, hits, HUD, menu
  └─ Skirmish (game/skirmish.*): teams, flags, scores, rules; computer players driving
     PlayerControllers (the player's own movement) along SkNav routes (A* over the terrain)
  └─ SkNetHost / SkNetClient (game/sk_net.*): our UDP protocol on the LAN (net/udp.*)
       host: runs the Skirmish, beacons, 20 Hz snapshots, events resent until acknowledged
       joiner: a mirror Skirmish filled from snapshots; sends its own player 30 Hz
```
Authority: each headset owns its player's movement and health (it reports hits it lands and its
own death); the host owns the computer players, flags, scores and the round. Protocol version 2:
states and snapshots are numbered (late ones dropped); events go oldest first; a snapshot fits one
1400-byte frame. A player who leaves is `gone` (slot reused by the next joiner); a joiner that
loses the host knocks again every 4 s. Bots learn impassable cells (stuck reports) and check once
which flags their side can reach; the route grid is on the water's surface. Not compatible with
the original game's network play (DirectPlay, native code, not decoded).

The headless harness (`tools/headless`) drives the identical `GameApp` with scripted poses on
Mesa llvmpipe, so rendering and asset code is verified on every build without a headset.

### Rework subsystems (branch `rework`, 2026-10-25/26)
- **Ragdolls** (`src/game/ragdoll.{h,cpp}`, `game_app_ragdoll.cpp`): a Verlet particle skeleton built from a
  skinned model's node tree at the moment of death, applied back through `SkinnedModel::setPoseModifier`. One per
  model slot (Reapers, character models, villager and vimp slots), keyed by the NPC's uid. `explode()` records
  recent blasts for the push. A hand holds a body by pinning one particle to the grip pose (`game_app_bodygrab.cpp`:
  `Ragdoll::pinParticle/movePin`, 24 constraint passes while pinned); a thrown body knocks living Reapers.
- **Hand items** (`game_app_stones.cpp`): the ground's stones are a hash grid (`pebbleAt`/`emitPebble` in
  `game_app_grass.cpp`); a picked one becomes a `LooseStone` (its cell marked taken) drawn from one dynamic mesh.
  Dropped guns and severed limbs are `Gib`s with `hand`/`thrown`. Throws use the fastest hand speed of the last 12
  frames. `reachPoint` (GROUND REACH option) lowers a low, forward hand's reach. Also arm-stroke swimming
  (`PlayerInput::stroke`) and one-time hints (`game_app_hints.cpp`, settings `hints_shown`).
- **World life** (rework, 2026-10-26): `game_app_marks.cpp` (scorch marks, footprints: terrain-following grids),
  `game_app_casings.cpp` (shell casings, stuck arrows), `game_app_birds.cpp` (a pool of 20 skinned birds: Flak
  flocks, Lobirds; `birdHitBy` for shots and stones), `game_app_fish.cpp` (Shiner schools; caught by hand: a held
  fish counts in `stoneInHand`). `AssetWorld::underwater` is a hook called just before the alpha-blended,
  non-depth-writing sea, so fish and swimming piranhas show through the water. `SkinnedModel` owns its GPU meshes
  and frees them in its destructor (keeps the renderer from `load`).
- **Ambience and weather** (`audio/ambience.cpp`: procedural seamless loops; `game_app_ambience.cpp`): the level's
  `a_...` markers (`AssetWorld::ambientMarkers`) choose voices per kind (nearest few, placed 1-6 m off in the
  marker's direction, the mixer's roll-off undone) and the weather (rain / storm / snow streaks in a cylinder
  round the head, one QuadBatch). Cloud shadows: `cloudShade()` in the shader's `sunShadow` (terrain noise,
  `uCloud`).
- **Dismemberment** (`SkinnedModel::sever`, `GameApp::dismember`): a bone subtree's vertices collapse onto its
  joint; the cut triangles become a mesh owned by a gib.
- **Cutscene cinema** (`game_app_cinema.cpp`, `Renderer::initScreen/beginScreenPass`): the shot camera renders the
  world (`GameApp::renderWorld`, shared with the eye pass) into a 1024x768 4x MSAA texture shown on a screen fixed
  in tracking space; `renderViews` draws only the dark room then. `CutscenePlayer` matches untyped performance trees
  to characters by mover name, size-aware.
- **Grass material** (`DrawParams::Material::Grass`): cards carry blade coordinates in the normal and the ground's
  texture coordinate in uv; the fragment shader evaluates `terrainColor()` there (one call site: llvmpipe).

## Threading / timing
Single render thread; simulation `GameApp::update(dt)` runs once per XR frame with dt clamped to
0.1 s. A fixed-step simulation loop will be introduced with gameplay systems (M8).

## Data location on device
`/sdcard/Android/data/io.github.giantsvr.dev/files/Bin/` — the user copies the contents of
their own `Giants - Citizen Kabuto/Bin` folder there (no runtime permission needed).

## Graphics API choice
OpenGL ES 3 (not Vulkan) for the first milestones: the original renderer is fixed-function
DX7/8, GLES maps directly, and with no headset available for testing, the smaller API surface
lowers the risk of device-only failures. The renderer is isolated behind `Renderer` so a Vulkan
+ multiview backend can replace it once on-device profiling exists.

## Diagnostics and comfort additions (2026-10-28)
- `src/render/gpu_timer.*` — GPU time per offscreen pass from timer-query timestamps (GVR_GPU_PASSES; on in scripted
  device runs). Marks only at whole-pass edges: on the Quest's tiling GPU a mark inside the eye pass splits nothing.
- `src/platform/android/crash_handler.*` — native crash reports in `files/crash/` (signal, backtrace, last log lines).
- `src/platform/android/audio_aaudio.*` `AAudioInput` — microphone loudness for Kabuto's roar (opened only on demand).
- `src/game/game_app_teleport.cpp` — teleport movement (Options); `UserSettings::seated` lifts the tracking floor in
  `GameApp::placeCamera`. `applySettingsList` (GVR_SETTINGS) overrides settings in test runs.
- `src/game/game_app_base.cpp` `renderBaseTable` — the base table map on the Build Pad.
