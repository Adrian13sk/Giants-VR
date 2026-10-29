# Engineering log

## 2026-10-21 — Session 1

### Environment (verified)
- Cloud container: Ubuntu 24.04, 2 vCPU, 7 GB RAM; git 2.43, cmake 3.28, ninja 1.11, gcc 13, clang 18,
  JDK 21, Python 3.13, Mesa 25.2 EGL/GLES (llvmpipe). No Android SDK, no adb.
- Network: GitHub git and release downloads work. dl.google.com, maven.google.com,
  services.gradle.org, PyPI, npm and Debian/Ubuntu mirrors are blocked (proxy 403).
- Linked Windows PC (eriklukacpc): file access to `C:\GOG Games\Giants - Citizen Kabuto` and
  `Documents`; no remote shell. No headset is attached to this environment.
- Game: GOG release, Giants.exe 1,630,208 bytes, renderer gg_dx7r.dll, Bin/ with 129 .gzp
  archives, 737 .bin files and 32 .abx files.

### Toolchain decisions
- NDK: Google's servers are blocked, so I used the HomuHomu833/android-ndk-custom r27c build
  (27.2.12479018, built from AOSP sources, x86_64 musl host). sha256 is pinned. It produces
  standard arm64 ELF linked against NDK API 29 stubs.
- aapt2 and the android framework resource table come from the Apktool 2.12.1 jar.
  apksig and zipalign come from uber-apk-signer 1.3.0.
- OpenXR: headers pinned to release-1.1.53, which matches the Khronos Android loader AAR 1.1.53
  (prefab arm64 .so, no libc++ dependency).
- No Gradle/AGP. The APK is assembled directly: aapt2 link (manifest), zip the native libs stored
  uncompressed, then zipalign and v3 sign. The NativeActivity uses `hasCode=false`, so no dex.

### Research findings (subagent and own verification)
- No original source exists. No open engine exists. References:
  - GiantZiP (GPL, Delphi) — GZP format.
  - GiantsTools imp_gbs — GBS format (no licence).
  - vibe-giants-decomp (Ghidra decompilation plus AI-written format docs, no licence, self-rated
    ~85% guessed names) — used for facts only.
- GZP: magic 0x6608F101 confirmed by hexdump. Standard Okumura LZSS (4 KiB window, spaces,
  r=0xFEE). The stored size includes a 16-byte local header.
- GBS: layout confirmed. A Python prototype consumed `sm_L0.gbs` to exactly EOF. Triangle runs
  are u16 `count` followed by u16 triples. Indices refer to display vertices.
- Character GBS files carry all-zero RGB arrays. Treating RGB as "baked lighting" made
  characters black, so characters are now lit dynamically and RGB is used only when non-zero.
- Sub-object flag 0x10 is set on opaque character meshes, so it is NOT an additive-blend switch.
  Blend mode now comes from texture alpha.
- Winding: after converting to (-x, z, y) — a proper rotation — 1463/1561 meshes have positive
  signed volume with no flip, so there is no flip.
- GTI: the decomp's description of the cell RLE is correct. Header specifics: flags byte at
  +0x34, name is 32 bytes, cells are {f32 height, u8 tri, [rgb], [extra]}. Decoded lightmap of
  Story1 shows the islands (out/story1_light.png, not committed).
- World .bin: ops 41 and 88 are 12 bytes (decomp said 13). Op 42 is the 21-byte object record
  {u32 type, f32 x,y,z, f32 angle} (decomp said terminator). With these fixes Story1, Story4 and
  Rstory1 walk to 0xFF exactly.
- Object types: `xx_*.bin` section 3 holds object definitions. Mesh table entries are
  `{u32 meshId, name padded to 4}`; type records are `{u32 typeId, u8 1, u32 modelGroup, ops…, 0xFF}`.
  Story1's op-42 types resolve mostly to `xx_ambient.bin` (ambient sound/creature objects),
  NOT buildings. The source of static structure placement is still open (B4).

### Build results
- Host build OK. Android build OK. APK 2.2 MB: libgiantsvr.so 0.6 MB stripped,
  loader 1.6 MB.

### Visual verification (headless)
- out/stereo_assets.png — Smartie, Reaper and Kabuto models textured. The first render showed
  black characters, which led to the RGB finding above.
- out/worlds_sheet.png — Story1, Kstory1, Rstory1 and M_Reaper_L1. Terrain was too dark with
  raw lightmap values (averages ~70/255), so I applied MODULATE2X plus a floor (provisional).

### SKN / ANM (same session)
- Both formats are serialized memory images: pointers are file offsets, there are NAM tables of
  `{u16 size, u16 type, u32 ptr, name}`, and the table start varies (0x20 or 0x28).
- **SKN:** object `{0, tree, 0, nverts, nclusters, ptrs}`. Cluster
  `{0, owner, bone, bbox[6], count, {f32 xyz, u32 index}[count]}` (rigid, one bone per vertex).
  Bone names are type-8 entries `{…, name, u16 kind, u16 boneIndex}`. Names can contain spaces.
- **ANM:** node `+0x10` parent, `+0x6C` pos, `+0x78` quat (x,y,z stored negated), `+0x88` scale,
  `+0x98/9C/A0` pos/rot/scale tracks. Track `{u8 keyType, …, u32 count@+8, ptrs@+12}`.
  VEC3_TCB value at +48, QUAT_TCB value at +24 (later corrected to +56, see device test 1 fixes). Header `{?, 0xB, 0, ptr, start, end, tpf, fps}`
  is referenced by each node at +0x0C.
- Verification: SKN clusters posed by the ANM default transforms reproduce the GBS rest
  positions exactly for bones whose stand pose equals the export pose. This confirms the
  composition order and the quaternion negation.
- **Scale changed 0.3 → 0.2 m/unit** so the animated Meccaryn is about 2.1 m. Still provisional.
- **Review fixes (independent review subagent):**
  - Never submit a projection layer if an eye's swapchain image wasn't released.
  - A failed wait after acquire is fatal.
  - `xrEndFrame` session/instance loss now triggers exit.
  - EGL config prefers WINDOW|PBUFFER (as Meta's samples do).
  - touchscreen/faketouch are marked not required.

### Device test 1 feedback → fixes (2026-10-21 14:30)
User report: everything visible; movement, turning and haptics work; scene a bit dark; a
missing leg; weird animations. A new character contact-sheet tool (`build/host/charsheet`) found
three causes:
1. **Missing leg (Kabuto):** ANM node flag word +0x94 bit 0x08 marks a **mirrored node** (a
   negative-determinant transform from the 3ds Max Mirror tool). The left thigh had it, so its
   leg pointed up into the body. Fix: apply a uniform −1 sign to the scale (Max
   AffineParts::f). Confirmed: the Kabuto left leg now reaches the ground mirroring the right,
   and Smartie's exact-reproduction count rose 89 → 124 vertices.
2. **QUAT_TCB key value:** the key holds three quaternions {A, B, C}. A and B are tangents and
   **C (+56) is the value**. In mc_idle the first key's C equals the node default for every
   node, while A doesn't. My earlier +24 choice only worked where all three were equal.
3. **mc_idle** is a genuinely energetic fidget (stretch and kick). The showcase now uses
   Mc_stand_scan2. Run, scan and shoot animations all look natural.
Also: characters turned to face the spawn point (models face −Z). Brightness raised: ambient
0.35 → 0.5, terrain lightmap 40+2x → 56+2.4x.

### Object placement (2026-10-21 14:40, commit 91b80bf)
- **Registry** (`src/assets/object_defs.*`, prototype `tools/py/objdefs.py`): scans every Bin/*.bin.
  - Pass 1: mesh/group entries `{meshId, name, groupId, 0x51 meshId …}`, validated by the repeated meshId.
  - Pass 2: type records `{typeId, 0x01, groupId}` at a 0xFF/000000 boundary.
  - Result: 629 types, 626 meshes. Scanning the 1.2 MB of .bin files is negligible; the whole host run is 0.56 s.
- **Placements:** world op 42 plus mission op-3 records. Both use the 21-byte layout with **yaw in degrees** (radians gave
  scrambled walls). Missions come from the op 92 scenario list. For now the first kind-2 mission is used
  (Story1 → 1a_1_timmy, 170 placements).
- **Helpers:** 115 of the Story1 placements are invisible engine helpers (markers, collision volumes,
  ambient-sound emitters a_*, lights) and are skipped by name.
- **Evidence:** ground-level render shows tables, posts, plants and Smartie wall pieces in the village.
  The bird's-eye render (`--at -4080 -1580 --height 70 --pitch -75`) shows wall segments joining into arcs.
  - Rstory1: 36 objects. Story4: 2.
  - Story2: 0, because its world stream stops before op 92 (B5).
- **Frustum culling:** sphere-vs-plane (Gribb-Hartmann planes from viewProj; infinite far plane) plus a fog-distance cull
  for placed objects. Village ground view: 286 → 185 draw calls, pixel-identical output (diff bbox None).

### Debug fly/noclip mode (2026-10-21 14:50, user request)
- **Toggle:** Y (the move hand's secondary button). Haptic feedback: 0.9 amplitude for 0.15 s when it turns on, 0.3 for 0.05 s when it turns off.
- **Flying:** the move stick follows the full head orientation, so pitch is included. Turn-stick Y moves straight up or down. Flying uses no ground following and no collision.
- **Leaving fly mode:** you drop back onto the terrain under your head.
- **Boost:** holding the move-hand grip multiplies speed by 5, both walking (3→15 m/s) and flying (15→75 m/s).
- **Tests:** unit tests cover the walking boost, climbing by looking up, descending below the ground (noclip), and re-attaching to the ground when fly mode is switched off.
- Temporary aid for exploring maps. Not a comfort-safe locomotion mode.

### Delivery bug (2026-10-21 14:50)
- **Symptom:** the user reported the fly mode "doesn't work".
- **Cause:** the APK on their PC was still the previous build (sha256 84835f3c…). Re-committing
  the same staged path (`outputs/GiantsVR-dev.apk`) wrote stale content, even though the commit reported success.
- **Fix:** re-sent the build under a unique staged name. The PC copy is now aa9d6e6d… (5f89e6a), confirmed by staging it back.
- **Rule added to CLAUDE.md:** always use a unique staged name and verify the hash on the PC before telling the user to install.

### Device test 3 feedback → building fixes (2026-10-21 15:20)
- **User report:** fences and wells look right; houses are rainbow-coloured and look damaged or underground; the boat is black; the tent tarp is black on top.
- **New tool** `build/host/modelsheet <Bin> <out.png> <model.gbs>[@anim.anm]...` renders static models from four directions. Prototype decoder: `tools/py/gbs.py`.
- **Smartie walls and houses are skinned meshes** (`smw_*`, plus sm_hatch, ripnest, s_weeds, smtimmy...). Each has `.skn` + `.anm`, and the object definition names the anim (`0x52 01 name.anm` after the LOD record; now in ObjectRegistry::modelAnim).
  - The GBS rest positions are a distorted copy of the shape.
  - The default node transforms (keys ignored) reproduce the clean master mesh exactly. Evidence: posed smw_curvelow equals smw_curvelow_m (mirrored in X).
  - The keyed animation starts with parts underground, which explains "damaged/underground".
  - Their RGB is **parametric skin data**: R ramps 2→252 along the piece, B is 0 or 254 by side. It is not lighting.
- **Fix:** ModelCache bakes skinned objects in their default skin pose once (static mesh, no per-frame cost) and ignores their RGB, lighting them dynamically.
- **Black parts:** baked RGB is per **sub-object**. Raft planks, the canopy top, palm leaves and barracks have all-zero RGB on some sub-objects. The baked-vs-dynamic decision moved from per model to per sub-object.
- **Rejected approaches:**
  - Borrowing a sibling skin for smw_wall_hi_hole (it uses smw_wall_hi's skin): vertex order differs, so the result was garbage. It now keeps its stored shape, lit dynamically.
  - Flag 0x10/0x80000000 as a "not lighting" marker: both are common on correctly lit models.
  - RGB saturation threshold: genuine warm lighting is just as saturated.
- **Open:** bw_* fortress walls, bw_bunker1 and mc_gun models still show rainbow-like RGB without the strict signature. None are in Story1.

### First playable slice: the player Meccaryn (2026-10-21 17:30)
- **Clip survey** (`gzptool anminfo`, new):
  - The Meccaryn clips are in place; the root "Center" does not travel.
  - The run speed comes from the planted foot's velocity in the cycle: mc_run 10.7–12.4 units/s, about 2.3 m/s at 0.2 m/unit.
  - The models face game −Y (runtime −Z).
  - mc_move does not locomote; mc_bush_walk is a crouched walk. So: idle mc_stand, run mc_run (playback rate = speed/2.3, clamped 0.6–1.6), jetpack mc_thrust / mc_hover, landing mc_land_soft.
- **SkinnedModel refactor:**
  - Several clips per skin, with bones bound by name per clip (clip node sets differ: 46–52 nodes).
  - `poseBlend` skins two clips and lerps the vertices, giving 0.2 s cross-fades.
- **PlayerController** (`src/game/player.*`, unit-tested):
  - Stick movement relative to the head yaw, with instant velocity (VR comfort). The body turns toward travel at 10 rad/s.
  - Terrain following with a 0.6 m step-down.
  - Jetpack: 26 m/s² thrust against 14 m/s² gravity, 6 m/s rise cap.
  - The landing clip plays when touching down faster than 3 m/s.
- **Camera:**
  - Third person: the tracking floor sits 3.6 m behind and 1.1 m above the feet, with rigid horizontal follow and 0.12 s vertical smoothing. The floor never goes below terrain + 0.3 m. Snap turn orbits the camera around the character.
  - First person: the floor is at the feet and the body is hidden.
  - B toggles the free camera; right stick click toggles first/third person.
- The showcase Meccaryn was removed (the player is one).
- **Headless:** `--free`, `--first-person`, `--jet`; `--at` moves the player in play mode. Renders confirmed idle, run, turn-to-travel, jetpack and the village run.

### Device test 4 feedback → first person with body and hands (2026-10-21 18:10)
- **User report:** "it is fine, the movement is kinda slow"; first person should be the primary mode, with visible hands and body; third person stays as an option.
- **Speed:** run speed raised 3 → 5 m/s. Grip sprints at ×1.5. Playback rate is capped at 2.4×.
- **First person (default):**
  - **Body pin:** the body is scaled so its eye height matches the user's (running max of the tracked head height). It is placed so its eye point sits under the tracked head, feet on the ground. The eye point is taken from the current animated pose (head-vertex centroid plus the idle offset), so a forward-leaning run clip moves the body back instead of swinging the shoulder armour into view.
  - **Hidden head:** head and neck vertices collapse to their centroid.
  - **Physical walking:** room-scale walking moves the player.
- **Arm IK** (`src/game/arm_ik.*`, unit-tested including a mirrored arm):
  - Two-bone solve done as world-space subtree rotations, so the mirrored right shoulder needs no special case.
  - The wrist target is offset from the controller grip by the palm distance.
  - Hand orientation: finger direction → aim −Z, index→pinky → aim +Y, both derived from the skeleton's finger nodes.
  - Elbow pole points down, out and back.
- **Finger curl:** the grip closes all fingers and the trigger curls the leading one. The palm side comes from the two middle fingers' rest bend; the outer digit bent the wrong way when it voted.
- **Body facing:** in first person the body faces the view (it turns at once when moving, and after more than 50° of head turn when standing). Sidestep uses mc_sidestep (played backwards for left); backpedal plays mc_run backwards (negative time; ANM evaluation now wraps negative times).
- **Rendering:** controller boxes are hidden in first-person play. The eye point was moved to the front of the face; inside the head, looking down showed the inside of the collar armour.
- **Headless:** first person is now the default. New options: `--third-person`, `--grip`, `--hands FWD UP`.

### Device test 5 feedback → body height, smooth turning, arms (2026-10-21 18:45)
- **User report:** torso fine but turning should be smoother when the head turns; the torso sinks after a recentre; arms barely visible, hands look big; hand movement OK, arm movement not.
- **Cause (height):** the body scale came from a running *max* of the head height. Lifting the headset while putting it on spikes that, inflating the body (eyes above the user's, camera inside the chest, arms around and below the view, big hands).
- **Fixes:**
  - **Eye height:** now a smoothed estimate (2.5 s time constant, only plausible 1.0–2.05 m samples).
  - **Vertical pin:** the body's eyes always sit at the user's eyes (clamped to −1.2/+0.5 m around the ground), so the torso keeps its place under the view.
  - **Recentre:** handled via XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING, passed to the game as InputFrame::recenterSerial; it resets physical-walk tracking so the origin jump is not walked.
  - **Turning:** exponential view following, no rate-limited snapping. Moving: 0.10 s time constant. Standing: free ±30° cone, carried smoothly beyond it, slow re-centring (1.5 s).
  - **Arms (IK upgrades):**
    - Upper arm and forearm stretched ×1.3 along the bone (Meccaryn shoulder-to-wrist reach is 0.49 m at user scale vs about 0.6 m for a human; the short arms pulled the gauntlets into the face).
    - Clavicle assist up to 26° for far targets.
    - The forearm carries 50% of the wrist roll.
    - The elbow pole swings opposite the controller's up vector.
    - Hands scaled ×0.8.
- **Evidence:** headless renders at rest, aim, reach and low hand poses show the forearms and gauntlets entering the view from the sides. Unit tests cover the smooth-follow behaviour.

### Device test 6 feedback → arm reach (2026-10-21 19:00, corrected 19:10)
- **User report:** "way better"; the only problem: "arms can stretch further then they are able to". I MISREAD this as "limit the stretch"; the user meant they want the arms to stretch MORE when the hand moves further from the body. The limit build (556548f) was replaced.
- **Final fix:** restored ×1.3 base length and the 26° clavicle assist. Added a **reach stretch**: when the wrist target is beyond the straight arm, both bones lengthen as needed (up to ×1.8 more) so the hand stays on the controller. Unit test: a target 1.6× beyond arm length is reached exactly; one 3× beyond stops at the limit. Renders at 0.7 m and 1.0 m show the hands on the controllers.
- **Superseded first attempt:**
- **Fix:**
  - Arm stretch ×1.3 → ×1.12.
  - Clavicle assist capped at about 11° (was 26°) and starts at 90% of reach.
  - Beyond reach the arm goes straight and the hand stays at its end, no longer following the controller outward.
- **Evidence:** headless renders with the controllers 0.7 m and 1.1 m ahead give the same straight-arm pose.

### Level collision (2026-10-21 19:30)
- **`CollisionWorld`** (`src/scene/collision.*`, unit-tested on synthetic boxes):
  - Placed objects contribute their opaque triangles in world space. Alpha-tested parts (foliage, grilles) and vegetation by name (weed/shrub/grass/plant/bush…) are walk-through.
  - Triangles are split by slope at the original's 50° walkable gate (cos 0.64278, decompilation constants table, used as a fact). Steep triangles are walls, the rest are floors.
  - The player is a capsule (r 0.35 m, h 1.8 m, step 0.45 m). Walls push it out horizontally: the closest point on each triangle to the axis is sampled at the overlapping heights, 3 passes. Floors under (x, z) up to feet + step become ground: wall tops, roofs, tables.
- **Story1:** 129 solid objects, 10,425 triangles. Broadphase is a per-object AABB; queries are cheap at this scale.
- **Player:**
  - Collision runs after movement (stick and physical walking).
  - When blocked or sliding, velocity and the animation use the actual displacement, so he goes idle against a wall instead of running in place.
  - Falling onto a top uses max(prevY, y) + step to avoid tunnelling.
- **Evidence:**
  - Running from the village centre stops at the curved wall after 14.9 m (render shows him at the wall).
  - Jetpacking over it carries him across and he lands beyond.
- **Headless:** new `--jet-window FROM TO`.

### First weapon: Meccaryn gun (2026-10-21 20:00)
- **Assets:** mc_gun_l0.gbs (gun), mc_bullet.gbs + .anm (bolt: two crossed spark quads), muzzle_flash.gbs + .anm, efx_flash.gbs (impact). The skeleton has Ref_Rhand / Ref_Lhand attachment nodes.
- **Gun RGB:** the gun's vertex RGB tracks position (R corr +0.72 with the barrel axis, G +0.56 / B −0.44 with height), so it is not lighting. Held objects are now dynamically lit (`ModelCache::get(..., dynamicLit)`), since baked lighting cannot apply to something carried around.
- **Attachment:**
  - Third person uses Ref_Rhand from the posed skeleton (`SkinnedModel::nodeWorld`, taken from the dominant clip after IK).
  - First person attaches the gun to the controller aim pose, so shots go where you point. The handle is at the grip and the gun scales with the body (×0.6). Ref_Rhand's orientation follows the original hand conventions and pointed the gun sideways in VR.
  - The gun model's forward is game −Y (runtime −Z), confirmed from the side-view model sheet.
- **WeaponSystem** (`src/game/weapon.*`, unit-tested):
  - 70 m/s bolts, 0.2 s auto-fire interval, 250 m range.
  - Swept segment raycast each frame against `CollisionWorld::raycast` (Möller–Trumbore with an AABB slab broadphase) and the terrain (march 0.5 m, then bisect).
  - Impact flash billboards toward the viewer; muzzle flash lasts 0.07 s. Haptic pulse on each shot.
- **Rendering:** effects are drawn additive, unlit, double-sided, without depth writes. b_spark is dim (max 158), so bolts are drawn ×2.5 bright with a 3-sprite trail.
- **Harness:** `--trigger X` (applied only during simulated frames; setup frames had fired at the spawn point), `--aim-pitch DEG`.
- The trigger haptic test pulse now only happens in the free camera.

### Device test 9 → third-person aiming (2026-10-21 20:30)
- **User report:** first-person gun "works flawlessly". For third person: fix the aim, add a crosshair, fix the gun size (it rendered as a tiny speck via the Ref_Rhand matrix).
- **Third-person aiming:**
  - The right controller's aim ray is raycast (buildings and terrain, 150 m; otherwise a point 60 m out) to get the aim point.
  - The original crosshair texture st_xhair_lock (red brackets) is drawn there as a billboard at a constant angular size (about 3.8°), alpha-blended, nudged toward the camera.
  - Aiming is active while the trigger is down and for 1 s after. While aiming, the Meccaryn faces the target (the strafe/backpedal clips cover movement) and his gun arm reaches toward it with the same IK as first person (no stretch, normal hand size).
- **Gun:** placed at the Ref_Rhand *position* only, oriented at the target (or forward-down when idle), ×0.75. Shots go from the muzzle to the aim point.
- **Evidence:** aiming 70° right turns him (yaw −76°) with the arm and gun raised (side render); the shot hit at 26.8 m.
- **Harness:** `--aim-yaw`.

### Enemies: Reaper grunts, combat loop (2026-10-21 21:15)
- **Assets:** rpman_l0 (Reaper grunt; the gun is part of the skinned mesh). The skeleton's `rpm_gun` node has `Ref_nozzle`, which serves as the muzzle.
- **Clips:**
  - Idle: rpman_stand_scan.
  - Advance: rpman_walk_fire (stride 8.2 units/s, 1.65 m/s).
  - Fire: rpman_stand_fire.
  - Hits: rpman_hit_med / _med2.
  - Deaths: rpman_hit_med_die / _spin_die / _gut_die (one-shot, held on the last frame).
- **EnemyDirector** (`src/game/enemy.*`, unit-tested):
  - States: Idle, Advance (sight 45 m), Fire (≤ 22 m, 3-shot bursts at 0.35 s, 1.4 s pause), Hit, Dead.
  - Dead bodies sink after 10 s and are removed.
  - Capsule raycasts; enemies are kept apart and collide with buildings.
  - **The AI behaviour and numbers are OURS, not decoded from the original scripts (MISSION_VM not decoded).**
- **Combat:**
  - Player bolts do 25 (4 kill a Reaper); haptic on hit and kill.
  - Reaper bolts: 32 m/s, red, aimed at the player's chest with spread, 7 damage.
  - Player health 100, regenerating 12/s after 4 s; respawn at the spawn point on death.
  - Damage feedback: red head-locked flash plus haptics on both hands.
  - Wrist health bar (FP, left controller) from a 1×1 white texture on the impact quad.
- **Waves:** 3 Reapers 30–40 m in front of the player (±40°, on land), the next wave 8 s after a clear. This is a temporary test encounter: creature spawns from the mission files are not decoded (B4). The static showcase Reaper was removed.
- **Terrain edge:** off the heightfield now counts as sea (ground −1.2 m), not "keep the current height" (the player could walk on air past the island edge). A ground rise of more than 0.8 m in one step blocks like a wall (climbing out of the sea needs the jetpack).
- **Evidence:**
  - Headless auto-aim (`--auto-aim`, test only): 12 hits killed wave 1 at about 29 m in 4 s; wave 2 spawned 8 s later.
  - Standing still for 12 s took the player from 100 to 23 health.
  - Renders show approaching Reapers, red bolts, the damage flash, the wrist bar and the TP crosshair on a Reaper.

### Device test 10 feedback → Reaper movement, feet, jetpack (2026-10-21 21:45)
- **User report:** "works fine", but the enemies stay in one place, their legs are underground, and the jetpack should work like the original: speed builds up gradually to a limit, with limited fuel that refills after a while.
- **Legs:** rpman's model origin is at the waist. Measured posed min Y = −1.10 m on rpman_stand_scan, while the Meccaryn's origin is at his feet (min Y 0.00). Enemies are now lifted by the measured foot offset from the idle pose (render and muzzle).
- **Movement:** they used to walk only ~8 m, then stand at 22 m. Now:
  - Run (rpman_bzka_run, 3.7 m/s) beyond 30 m; walk-and-fire (rpman_walk_fire) within 35 m, shooting while advancing.
  - Stop to stand-fire only at 10 m.
  - Each Reaper approaches a flank point up to ±9 m beside the player, re-chosen every 3–6 s, so groups spread and circle.
  - Simulation: nearest Reaper at 30 → 23 → 14 → 10 m over 15 s.
- **Enemy accuracy:** spread 0.06 + 0.004/m and 5 damage (was 7).
- **Jetpack:**
  - Thrust 21 m/s² against gravity 14, rise cap 5 m/s.
  - Horizontal speed builds at 4 m/s² up to 9 m/s while thrusting. Air momentum is kept, with 3 m/s² stick control.
  - Fuel: 4 s of thrust, refilling after 1.2 s at 0.5 tank/s.
  - Blue fuel bar under the wrist health bar. Unit-tested (gradual speed, empty tank, refill).

### Device test 11 feedback → running enemies, ground contact for bodies, real cliff block (2026-10-21 22:15)
- **User report:** enemies should also run, and must not shoot while moving; dead bodies float in mid-air; cliffs can still be climbed on foot (only the jetpack should get you up).
- **Enemy AI:**
  - Run (rpman_bzka_run) to a spot about 10 m in front of the player, offset ±10 m sideways.
  - Stop within 30 m, or after a 1.5–3.5 s run, and fire 2 bursts of 3 (stand_fire).
  - Then pick a new spot and run again. After a hit reaction they also run to a new spot. No shooting while moving.
  - Unit test: ≥6 shots fired, 0 while running, ≥2 stops in 20 s.
- **Bodies:** the fixed idle foot offset (1.10 m) kept lying death poses lifted. Each Reaper is now lifted per frame by −(posed min Y) (lightly smoothed): the planted foot when standing, the body when lying. Render confirms corpses lying on the grass.
- **Cliffs:** per-step rise vs horizontal distance is limited to rise/run 1.19 (50°, the original's walkable gate), on top of the 0.8 m step limit. The old per-frame rise check let 85° slopes through, because steps are only ~7 cm per frame.
  - Unit test: 60° slope blocked, 30° hill walkable.
  - In the world: from the sea the player is stuck at the cliff base; with the jetpack he reaches the top (52.5 m). Normal island running is unaffected (5 routes checked).

### Device test 12 feedback → sliding down cliffs (2026-10-21 22:40)
- **User report:** landing on a cliff should make you slide down, using an animation the original has.
- **Clip:** mc_skid.anm (crouched balancing skid, 0.97 s), the only slide/skid clip for the Meccaryn.
- **Behaviour:**
  - On ground steeper than 50° (terrain gradient over ±0.5 m), the player skids downhill.
  - Acceleration 9 m/s² up to 10 m/s, sideways steering 2 m/s², body turns downhill (third person), clip PlayerAnim::Slide.
  - On reaching walkable ground the slide speed bleeds off at 12 m/s² instead of stopping dead. The jetpack still works.
  - Unit test: 60° slope slides to the bottom with no input; 30° stays put.
  - World: landed 16 m up the cliff and skidded down, reaching about 9 m/s.
- **Terrain height bug found on the way:**
  - `Terrain::heightAt` used bilinear interpolation, but the mesh uses two triangles per cell (diagonal from the triangle code). On cliff cells they differ by metres; the third-person render showed the character inside the rock.
  - heightAt now evaluates the same triangles, so ground, feet, slopes and collision match the visible surface everywhere.
- **Third-person camera collision:** if terrain or a building is between the chest and the eyes, the camera pulls in front of it.

### Device test 13 feedback → enemies can't climb cliffs (2026-10-21 22:55)
- **User report:** enemies climbing cliffs should be impossible, or behave as in the original.
- **Fix:** enemies follow the player's ground rules.
  - `EnemyDirector::tryMove` rejects steps that rise more than 0.8 m or steeper than rise/run 1.19 (50°).
  - Blocked for more than 0.3 s: if the player is within 30 m they stop and shoot; otherwise they detour (heading ±63° from the goal for 2 s, flipping side if blocked again).
  - On ground steeper than 50° they slide downhill at 6 m/s.
  - The original AI's exact cliff handling is unknown (B9); this mirrors the player's rule.
- **Evidence:** unit test where the enemy below a 60° cliff never rises above 1.5 m in 20 s while the player stands on top. Regression: a normal wave still closes from 30 m to 12 m.

### Audio: original sound effects (2026-10-21 23:20)
- **Assets:** 597 WAVs across 25 archives. All are RIFF PCM 16-bit: 582 mono 22.05 kHz, 11 mono 16 kHz, 4 stereo.
- **Names are 2-letter-prefixed codes; meanings INFERRED from the names and waveform shape (attack, duration):**
  - Meccaryn: me_peas1/2 (pea-shooter gun), me_jpkst/jpklp/jpken/jpkno (jetpack start/loop/end/empty), me_fsgr1/2 (footsteps on grass), me_hdfl1/2 (landing), me_sld1/slden (slide loop/end), me_shrt1–3 (hurt).
  - Impacts: bi_dirt1, bi_hit1/2, ex_bdys1 (body).
  - Reapers: rh_sprsh (shot), rh_fsld1 (footstep), rpman_hit1–4, rpman_die1–5.
- **AudioMixer** (`src/audio/mixer.*`, unit-tested):
  - WAV parser (8/16-bit, mono/stereo, downmix to mono float).
  - Positional voices with inverse-distance rolloff (ref 2 m, silent at 90 m), equal-power pan from the listener's head pose, and 25% quieter behind.
  - Linear-interpolation resampling with pitch, per-block gain ramps, 48-voice cap, tanh soft clip.
- **Quest output:** `AAudioOutput` (`src/platform/android/audio_aaudio.*`).
  - Float stereo, low latency, usage GAME, mixed by the data callback.
  - Paused with the activity. On disconnect the error callback only flags it, and the main loop reopens the stream. Linked against libaaudio.
- **Game events:**
  - Gun shot (alternating, pitch-varied), world impacts, Reaper hit (body plus voice 60%) and death voice.
  - Reaper shots and running footsteps; player hurt (0.6 s cooldown).
  - Jetpack start/loop/end/empty, slide loop/end, landing (hard above 6 m/s), footsteps on run-cycle halves.
- **Headless:** `--audio-out file.wav` records the mixed game audio (667 frames per 1/72 s frame).
  - 10 s scripted fight: continuous gunfire at the 0.2 s cadence, right channel louder (gun in the right hand), no clipping (peak −5.4 dBFS).
  - **Not listened to yet:** the device test will judge whether the sound choices are right.

### Device test 14 → audio confirmed (2026-10-21 18:43 local)
- User: "Yeah, it works just fine". AAudio output, positional mixing and the inferred sound mapping are DEVICE VERIFIED.

### Mission setup streams and object definitions decoded (2026-10-21 22:20 local)
- **Sources:** the decompiled `mission_init` and `read_objects_bin_PS2` (vibe-giants-decomp) used as a *factual* reference for
  op sizes only. No code copied. Every claim checked against the shipped files.
- **wm_*.bin:** `u32 4`, then `{u8 op; payload}` until op 2. 37 ops, all fixed-size or counted, no control flow.
  - **All 56 mission files walk exactly to EOF.**
  - 1a_1_timmy: 343 objects plus 1 mounted (Timmy's jetpack). The old op-3 heuristic found 170.
  - Op 5 is an id, not a team. Spline points use it as the path number (paths 5–10 in 1a, with op 7 = point index and
    op 0x21 = cumulative length). The only `marker` with id 1 stands 280 m from the village, facing it. It is used as
    the **player start** (provisional reading).
- **Object definitions:** section 3 of every 0x1A0002E5 file has offset tables of meshes, groups and types.
  - Groups: LODs (0x51), animation slots (0x52), fx events (0x7F/0x80 = u32 + 5 strz + u32).
  - Types: op 1 = group, 0x1E = object class ("ospec"), 0x17 = scale.
  - Every file parses: **807 types / 799 groups / 1570 meshes**, versus 629 types from the old heuristic.
  - It missed palms, Smartie houses and Ripper nests because their LOD records don't follow the mesh name.
- **Placement:** applies per-object scale (palms 2.2, the large Ripper 2.3) and op-4 pitch/roll (axis order provisional).
  Story1 now places **300 static objects + 9 characters** (was 135).
- **B8 partial fix:** some sub-objects carry strongly coloured RGB that decodes (`c*2/255-1`) to unit vectors (Smartie domes,
  bw_bunker1). Every checked correct model has mean saturation < 0.26 (raft 0.70, no unit vectors). Rule: saturation > 0.4 and
  >= 10% unit vectors → lit dynamically. The domes now show brick like the houses.

### Mission characters: Rippers, Timmy, Dactyls (2026-10-21 22:40 local)
- **Character test:** mission objects with a .skn skin are characters (not baked statues) if any of these hold:
  - their class is a creature class (1, 4, 18, 23, 42, 45, 51, 53, 99);
  - they have no class and at least 5 clips (Smarties, Sonaks);
  - they are Timmy.
- **Class 51 (Reaper grunt):** spawned into the existing Reaper AI at its mission position. Test waves now run only when a
  mission has no enemies (Story1 has none).
- **Class 23 (Ripper):** new `CreatureDirector` (src/game/creature.*, unit-tested). It uses the original clips by slot:
  1 idle, 2 walk, 10 attack, 11 jump attack, 44/49 hits, 45 suffer (ends lying down, used as death).
  - Behaviour (ours, B9): idle → notices within 20 m → runs (2.8 m/s, walk clip ×2.2) → bites within 1.5 m (5 dmg, 0.8 s pause)
    or lunges from 3–6.5 m (12 dmg) → hit flinch → walks home past a 45 m leash.
  - Size, reach, speed and health scale with the mission scale: 2 bolts kill a small one, 6 the large one.
  - Same cliff rule as the player.
  - Sounds rs_* (small) / rl_* (large): chitter on alert, attack, injury, death, feet (meanings inferred).
- **Other characters** loop their default clip (Timmy's wave, Dactyl flight).
- **Headless:**
  - Player 12 m from Timmy's three Rippers: they close in and bite (100 → 73 health in 4 s, 28 at 8 s if you don't fight back).
  - With `--trigger 1 --auto-aim` all three die and the player stays at 100.
  - The start view shows the village on the far ridge.

### Device test 15 feedback → Ripper spit, burst deaths, steering (2026-10-21 23:30 local)
- **User report (build 4a1d8c2):**
  - Frame rate is smooth with the populated level.
  - The Rippers feel alright, but their navigation toward the player is weird.
  - In the original they also shoot orange/blue balls that explode.
  - Killed Rippers either fall to the ground or explode.
- **Found in the data:**
  - Three Ripper types share one model: 229 small, 73 medium, 230 large (scale 2.3). Mission 1a has 5 medium and 1 large.
  - Type op 0x31 is the hit points (the loader stores it as current and maximum health): 20 / 40 / 90. Reaper grunt: 100.
  - Projectiles: `efx_ripper_mspit` (orange) and `efx_ripper_lspit` (blue), 0.6 m ball clusters with `ref_spit` nodes.
  - Splash sprites: `ripspit` / `ripspitbig` (smoke_ring).
  - Gore: `rip_gore_head`, `rip_gore_arm`.
  - Spit clip: slot 50 `rip_spit`. Mouth node: `ref_mouth`.
  - Sounds (meanings inferred): `rm_atfr`/`rl_atfr` (fire), `rm_frexp`/`rl_frexp` (explode), `ex_bdyl` (body burst).
  - Type-op sizes added (flags without payload): 0x2A, 0x3A, 0x3D, 0x7D–0x7F.
- **Steering (ours):**
  - Turns before running: speed scales with cos²(heading error), and acceleration is limited.
  - Look-ahead probes against buildings and climbable slope, with detours preferring the creature's side.
  - Detours when blocked by a cliff.
  - Each Ripper closes in from its own angle around the player.
  - When one notices the player, pack-mates within 15 m join in.
- **Spit (ours):** medium and large Rippers only.
  - Range 7–28 m, needs a clear line of sight, 3.5–5.5 s cooldown. Launches at 55% of the clip from `ref_mouth`.
  - Ballistic lob at 15 m/s horizontal with 12 m/s² gravity.
  - Bursts on terrain, buildings or the player; splash damage medium 8 within 1.8 m, large 15 within 2.6 m.
- **Death:** 40% burst into gore (flash, sound, the three original gore pieces thrown along the shot, bouncing, then sinking).
  Otherwise the existing collapse.
- **Headless:**
  - 26 m from Timmy's Rippers, standing still: 4 spits in 12 s, about half hit.
  - Misses land 2.5–2.8 m away, past the player's side.
  - Firing back kills all three; `GVR_BURST_CHANCE=1` frames show the flash and the flying gore.

### Device test 16 → Rippers confirmed (2026-10-21 22:38 local): "It's fine now."

### Mission objectives, the story jetpack, original music (2026-10-21 23:55 local)
- **Data:** the workspace held only 331 of the 899 Bin files. Copied from the user's PC:
  - the Story1 and intro cutscene scripts (`flk_*.bin`);
  - `VO_SfxEnglish.gzp`, `GTextEnglish.bin`, `MusicSet.bin`;
  - the Stream/Story1 soundtracks;
  - Music segments for ma1, msb and mvy.
- **Mission triggers (decompiled `mission_trigger_name`, `mission_check_obj_proximity`, used as references):**
  - Mission op 8 sets the object's trigger: 1 proximity, 2 death, 3 destination arrival, 4 Vimp-meat proximity.
  - Proximity fires within 10 game units (2 m) of the object's bounds.
  - Each fired trigger increments the mission counter.
  - 1a_1_timmy has 4: the three Rippers at Timmy's tower (death) and `sm_tower_timmy` (proximity).
  - 1b: 8 Smarties (destination). 1c: a trigger sphere (proximity).
- **Mission text:** `GTextEnglish.bin` = `u32 n, u32 keyBytes, u32 valueBytes, {u32 keyOff, u32 valOff}[n]`, then the key and
  value strings. 2003 entries, including every cutscene subtitle line ("flk_story1_jetpack_3_2": "There's no way I was
  giving my new jetpack to that old Ripper!").
- **Story jetpack:**
  - The opening cutscene has Baz lose his jetpack ("WAIT!!! Don't shut the hatch... I need my jetpack!").
  - Mission item lists (op 0x0E) contain item 1 in every later Mecc mission and not in 1a. Item 1 is read as the jetpack
    (provisional).
  - The jetpack is off until 1a is complete; the fuel bar is hidden until then.
- **Completion:**
  - All objectives done → the outro flick's soundtrack plays. The flick names its stream (`f_jetpack`), resolved in
    `Stream/<World>/`.
  - The jetpack is granted. The flick visuals are not played yet.
- **Music (MusicSet.bin decoded, see FORMATS.md):**
  - 429 songs (sequential playlists; segment joins are sample-continuous, e.g. a1→a2 jump 10 vs typical step 124).
  - 380 mood-set variants; 932 resolved transitions (from segment → entry segment of the target set).
  - Segments are headerless 16-bit stereo PCM at 22050 Hz: gs_ds.dll sets every stream format to 22050 Hz.
- **Music moods (ours; the original's choice is native code):** ambient `ma1` (Mecc), `msb` with 1–3 engaged enemies,
  `mlb` with 4+, the `mvy` stinger on mission completion, silence during a cutscene soundtrack.
- **Headless:**
  - Idle recording has ambient music (RMS 0.01–0.05).
  - At Timmy's tower with auto-aim: objectives 4/4 → "Mission 1a_1_timmy complete" → `f_jetpack` (40 s) plays → jetpack granted.

### Cutscenes play in VR (2026-10-22, commits f85cb67, 23f52f7, 89ca00b)
- **Parser:** shot files decoded (layout in FORMATS.md). The 2003-entry GText database supplies the subtitles; the game
  font (`giantfont_eng.tga`, 63 glyphs measured from alpha) draws them.
- **Bug found on the way:** Bezier position keys (type 5) were read at +4 (tangents ≈ 0), which put most shot cameras and
  the flying lander at the world origin. The value is at +32. This only affected type-5 tracks (33 tracks in the shots;
  character clips use TCB).
- **Look-at cameras:** their rotation track (type 2) has no keys; facing the `.target` node gives the intended framing
  (checked: camera→target agrees with the camera's −Z where its rotation is static, and frames the subject elsewhere).
- **Actor trees:** matched in order: mover named after the actor (fuzzy, Levenshtein ≤ 1), a node named after it, then
  the unclaimed tree that best fits the actor's skeleton. Untyped trees fitting the Meccaryn skeleton are shown as
  Meccaryns (Baz and Gordon in the opening). The lander is skinned with its own `M_lander_l0.skn/.anm` (canopy, door,
  chairs, stick are bones).
- **Props (op 3):** placed on the carrier's bone each frame (jetpack, gun, Timmy on Baz's back, the flag).
- **Presentation change (from the agreed fixed viewpoint):** many shots move the camera with the action (the opening
  shots follow the lander at ~27 m/s). A fixed viewpoint lost the subject within a second. The viewer now travels with
  the shot camera, translation only; the view never rotates by itself; fades between shots. Needs the user's comfort
  verdict on the headset. The eye is kept 0.7 m above our terrain (story1_jetpack_2's low camera was inside a hill).
- **Headless (contact sheets of every shot):**
  - Opening (story1_start_1…12, 62 s): the lander flies to the island with Baz and Gordon in the cockpit; Baz drops;
    flak and flags at the end ("YOU VICIOUS BASTARD!!!").
  - Jetpack (story1_jetpack_1…8, 39 s): Timmy with the jetpack, Baz with gun and flag, Timmy on Baz's back.
- **Known gaps:** prop alignment for static props (`ref_handle` on the gun) is not applied; actor facial/hand detail
  depends on the shared skeleton; a few close cameras sit inside geometry at VR scale.

### Mission 1b, story progression, sentries (2026-10-22)
- **Mission data (gzptool `mission`):**
  - 1b_1_catchsmarties: 8 Smarties (type 11, class 18, scale 1.33) with trigger 3 (destination arrival) on an islet
    among rock pillars (`w_story1_hang`, about 1 m above the sea); the pad `sm_dropoff` is ~90 m away.
  - Opening `story1_save_sm_1…15` (Timmy explains the wax Smarties); outro `story1_get_rpg1…9` (the RPG present).
  - Mission text (GText `MO_1b_1_catchsmarties`): "Save the SMARTIES using your sexy new JETPACK. Drop them off on the
    conveniently located SMARTIE PAD nearby. Watch out for hungry PIRANHAS in the water!"
- **Carrying (ours, VR-native; the original's pick-up rules are native code):** reach with either hand and squeeze the
  grip. The reach test allows a hand up to 1.6 m above the Smartie's head, so nobody has to touch the floor. Release to
  let go: it falls with the hand's motion (capped at 6 m/s). On the pad it is saved (the objective); in the water it
  returns to the islet after 3 s.
  - Original clips: sm_wave, sm_squirm, sm_fall, sm_celebrate, sm_drowned.
  - Original sounds: me_smton / me_smtof / me_smtdp (Meccaryn pick up / let go / drop off), gsmNN_1/_2 Smartie voice
    pairs (our use: pick-up line, thank-you), se_splsh, pi_swrm1/2 (piranhas).
  - `RescueDirector` is unit tested (grab with the reach-down assist, carry, save on the pad, splash and return,
    out of reach).
- **Progression:** `AssetWorld` lists the world's missions in scenario order and `selectMission()` rebuilds the level
  state. After a mission's outro the next mission loads (1a → 1b → 1c_1 → 1c_2 …). In the free camera, X skips to the
  next mission. `GVR_MISSION=<name>` starts a given mission (headless).
- **Headless:** `GVR_AUTO_RESCUE=1 GVR_MISSION=1b_1_catchsmarties`: 8/8 saved → "Mission complete" → story1_get_rpg
  outro → 1c_1_firstmecc loads (181 objects, 24 Rippers, 1 objective). First-person grab via `--grip-window` and
  `--hand-path`: "Smartie 0 picked up (right hand)".
- **Mounted sentries:** 1c_2/1d_1 put Reaper grunts on guard towers (op 0x16 mount, point 255). They now stand on the
  tower's highest walkable surface (the crow's nest, found with the collision floor query below the model's top) and
  fire from there without moving (`Enemy::posted`). Other mounts (Tel in his crashed plane on hardpoint 0, carried
  items) are still not shown: hardpoint positions are in undecoded data.
- No test Reaper waves in missions that have objectives.
- **Piranhas:** where the mission briefing (GText `MO_<mission>`) mentions PIRANHAS (1b), standing or swimming in water
  deeper than 0.5 m gets the player bitten after 0.8 s: 8 damage every 0.5 s with the original `pi_swrm` sounds (rate
  and damage ours). Headless: health 100 → 16 after 4 s in the water at game (-1850, 3400).
- **Rocket launcher ("RPG", 2026-10-22):** Timmy's present at the end of 1b. Mission item 3 is read as it: it first
  appears in 1c's item lists (provisional, like item 1 = jetpack).
  - Original assets: `mc_launcher_l0.gbs` in hand, `mc_rocket.gbs` projectile, `me_msls1` launch, `me_mslex` explosion.
  - Behaviour (ours): 32 m/s, one rocket per trigger pull, 1 s reload; 4 m splash (120 damage at the centre, 48 at the
    edge); the sea stops rockets; the player takes splash damage too.
  - Right stick forward switches weapons.
  - Headless (`GVR_LAUNCHER=1`, 1a at Timmy's tower): one rocket killed two Rippers (objectives 1/4, 2/4).
- **Mount hardpoints found:** a mounted object's position is a reference node in its parent's own `.anm`. Examples:
  `guard_tower1.anm` has `Ref_guard01` at (0.4, 11.3, 67.3) in the crow's nest; `mecc_crash1.anm` has `Ref_Lhand` at
  (−2.7, 66.5, 18.3) on the plane's nose. Mount byte 255 = the first `Ref_` node, k = the k-th (only 255 and 0 occur in
  Story1).
  - The sentries now stand exactly on `Ref_guard01`, which replaces the collision-floor guess.
  - Tel (1c_1) hangs from `Ref_Lhand` by his left hand, using the original `mc_hang_onehand_yell` clip; his
    `ref_lhand` bone is placed on the point.

### Device test 18 feedback, fixed (2026-10-22, build a899d44)
The user's report on build 4b15f54, and what changed:
- **Cutscene audio was missing.** Voice lines are op 0x2C streams in `StreamEnglish/Newstreams` (baz006, tim001,
  gor002…). Effects such as `f_ms_msfto` are in `Stream/<World>`.
  - The op 9/10 waits name absolute frames/ticks of the shared Max timeline: story1_start_2 starts at frame 210 and
    "wait 265" = 1.83 s into the shot. Voices and subtitles are timed from them, one subtitle at a time.
  - INSTALL now asks for `StreamEnglish` to be copied too.
- **Staging:**
  - The hatch now opens: small rigs need only half their bones matched.
  - Flags pop in: Bezier (type 5) scale keys are read at +32. Props sit at 0.01 scale until they appear, which is what
    the "missing flag" was.
  - World copies of cutscene actor types are hidden during cutscenes.
  - op 0x20 places free actors (the wax-Smartie rig).
  - Feet sunk up to 0.6 m into our terrain are lifted.
  - Tel hangs from the crashed plane, in gameplay too (mount hardpoints).
- **No more waiting after cutscenes:** a loading/briefing screen fades in after a mission, shows the next mission's
  title and objectives (GText) while it loads, and continues by itself after 6 s. The first mission gets the same
  briefing before its opening cutscene.
- **Meccaryn face:** GBS sub-object flag 0x1 is glass (lander canopy, visor). It is now drawn see-through, so the
  face texture behind the visor shows.
- **Jetpack model swap:** `mc_nothrust_l0` (no jetpack) until the jetpack is received, then `mc_l0`.
- **Bridges:**
  - Stepping onto a structure is limited by step height, not the 50° terrain slope test (the deck lip was a "cliff").
  - No slope sliding while on a structure.
  - Slope uses the gentler one-sided difference, so a cliff-top landing counts as flat.
  - Headless: both 1a bridges crossed end to end.
- **Water:** deep water (> 1.35 m) floats the player at shoulder depth (no diving, 2.4 m/s swim); NPCs float too.
  - Entering water splashes (`efx_splash`); swimming leaves rings (`water_rings`).
  - Piranhas (`piranha` + `pir_swim` / `pir_swim_jump`) gather in all deep water after 0.8 s, circle, leap and bite.
    They leave when you do.
  - Water surface shader: waves, fresnel sky reflection, sun glints.
  - The sea bed tints with depth.
- **1b Smarties hang on the pillars:** `w_story1_hang.anm` has nine `ref_smartie_NN` nodes, which are the hang points.
  - Each Smartie hangs (`sm_hang`) for 70–120 s, slips (`sm_hang_onearm`), lets go, falls into the water, is eaten,
    and hangs again 25 s later.
  - It can be grabbed off the hang point.
- **Ripper nests (class 47, five in 1a):**
  - Each nest hatches a Ripper every 6–10 s (up to three at a time) while the player is within 70 m.
  - Hatched Rippers charge straight at the player and never give up. They use a pool of models.
  - Shots shrink a nest (to 35 % at 0 HP) until it is destroyed; rockets damage nests too.
- **Graphics:**
  - The sun circles once every 20 minutes and never sets (30–62° elevation).
  - Real-time shadows from a 1024² sun depth map, 56 m around the player, with 4-tap PCF. Casters: objects, characters,
    enemies, nests, and the player in third person.
  - Sun disc and glow in the sky shader, a fan of sun rays occluded by the scene, and a closed sky dome. Clouds drift.
  - Performance on the Quest is unmeasured: the shadow pass and the heavier shader are new costs.

### Device test 19 feedback → graphics, gore, death, AI, water (2026-10-22, build f5c0f47)
The user's report on a899d44: frame rate unchanged, cutscenes fine apart from Timmy floating, loading
screen fine, face and jetpack swap fine, bridges fixed. Asked for more:
- **Graphics:**
  - **Colour grade:** in the shader, a gamma-2 space with saturation 1.25, contrast 1.06, warm highlights /
    cool shadows and exposure 1.08. The sky gets a quarter of the saturation boost (the painted sky turned
    acid at full).
  - **Lighting:** hemispheric ambient (sky above, bounce below), a faint rim, and baked surfaces brighten a
    little where they face the moving sun.
  - **Sharper shadows:** 2048² map over 48 m (2.3 cm texels, was 5.5), 5-tap PCF, smaller bias.
  - **Planar water reflections:** the world mirrored in the sea, rendered once per frame from between the
    eyes into a 1024×768 colour + depth target, with a field of view covering both eyes, and clipped at the
    surface. The water shader corrects the lookup per eye with the reflection's depth, so reflections have
    real stereo depth. The player (the whole body, including the head that first person hides) and Timmy
    show in it.
  - **Reflection cost:** about +25 % draw calls (203 on 1a, ~120 on 1b). Re-rendered every other frame,
    skipped when no water is in view.
  - **Adaptive quality fallback:** if the average falls below 65 fps over 4 s, the reflection drops to every
    third frame, then off, then the shadows go off. Each step is logged.
  - **Water depth map:** terrain depth below the sea, 2 m texels. It gives clear turquoise shallows, dark deep
    water and thin animated foam at the shore. The stylised fresnel is never below 20 %.
  - Sky band seam fixed (the texture repeats, so v=0 bled into the bottom row).
- **Cutscenes:**
  - **Floating actors:** flk op 0x19 {actor, node} + op 0x22 {actor, 6, 5, footprint x0 y0 x1 y1} read as
    "this actor follows the ground" (inferred: every walking character carries the pair).
  - Per shot, the lowest the feet come over the whole shot is put on our ground (terrain, or a
    walkway/rock top), so standing actors don't float or sink while hops keep their arc. Timmy was 0.8–1 m up
    in save_sm_1, get_rpg3 and hatch_dilemma.
  - **Subtitles** are head-locked (smoothed yaw/pitch, no roll).
- **Timmy on Baz's back:** the cutscenes carry Timmy (smtimmy_l0) on baz.ref_holsterb (story1_jetpack_6/8,
  get_rpg9/10, hatch_into_tel), and no Story 1 mission after 1a places him. So from the jetpack hand-over on
  he rides there in play, with sm_mount.anm and his 'center' node on Ref_HolsterB. The 1a tower Timmy is
  hidden then.
- **Briefing on demand:** hold the left stick down to see the mission title, objectives and progress
  head-locked over the world.
- **1b completion:** the briefing (MO_1b_1_catchsmarties) and the walkthrough both say save four, so four saved
  completes the mission (eight hang).
- **Water:**
  - **Piranha schools:** a school hunts each Smartie that falls in. They bite (pi_swrm) with blood clouds and
    eat it in about 5 s (walkthrough), and a Smartie can be grabbed back out until then.
  - Wading leaves rings and splashing steps. NPCs in the water leave rings.
  - Swim stroke loop (me_swmlp).
- **Footsteps:** louder (0.3 → 0.75), and by surface with the original sets me_fs{gr,sa,wd,wa}.
- **Enemies:**
  - Reapers and Rippers patrol near their post (bzka_walk_scan / walk). They notice the player in front with
    a clear line (terrain and buildings block it) at 60 m and 38 m, or from any side at 14 m and 9 m.
  - They take a moment (alert scan / chitter) before acting.
  - Gunfire and a falling comrade alert those nearby; a hit always does.
- **Gore:** red blood from aablood.gzp. Archive precedence is now "first in name order wins", which is what
  the aa*/arpfix override packs are named for. aanoeng.gzp (language-neutral signs) is kept last so the
  English signs stay.
  - Hits bleed: drops, spray, splats. Kills leave a pool (bloodblob).
  - Bodies stay. After about 14 s, Cleaners (cleaner_l0 with walk/eat/jump and CL_EATST/LP/EATEN) come and
    eat them; the body sinks and is gone.
  - Bodies in deep water are eaten by piranhas.
  - Shooting a body (box test on its death pose) bursts it or tears it apart, with rpman_chunk_* /
    rip_gore_* pieces, blood and a pool. Rockets do the same to nearby bodies.
  - Reapers killed by a blast play rpman_hit_blowup.
- **Player death:**
  - Bullets and bites: he falls (mc_die).
  - In the water: he goes under (mc_drowning_die).
  - A blast: blown into MC_chunk1..6.
  - The camera steps back to third person, fades after about 4.5 s, and he respawns at the mission start.
    Enemies stand down while he is dead.
- **Tests:** AI tests updated (noticing in front, patrol stays near post, a hit makes aware); new gore test
  (drops land as splats or water clouds, decals fade and are capped, Cleaners eat and leave). All pass.
- **Not device-tested yet:** everything above, and the GPU cost of reflections plus 2048 shadows on Quest 2.

### Device test 20 feedback → Timmy, Cleaners (2026-10-22, build acb8c9c)
- **Reported:** no frame drops with the reflections and 2048 shadows; graphics better; gore, Cleaners, death and briefing
  work. Two things to fix: Timmy rides sideways and keeps disappearing, and the Cleaners come only two at a time.
- **Timmy:**
  - Only the bone *positions* are used now: his 'center' on Ref_HolsterB, with Baz's orientation.
  - sm_mount's posed body faces backwards in its model frame, so he is turned round. He now rides piggyback, arms
    on the shoulders, looking ahead over them (harness views from behind, side and front).
  - The disappearing was the first-person rule: he was hidden within 0.45 m of the eyes, and he sits right there.
    In first person he is now not drawn in the view at all; he still shows in reflections and the shadow.
- **Cleaners:** swarms of 5–6 to a Reaper and 3–4 to a Ripper (pool of 18). They trickle in from 7–16 m and spread
  round the body; they share the eating, about 12 s per body.

### Device test 21 request → destruction, jetpack, damage, HUD, injections (2026-10-22, build 36320d3)
- **Asked for:** destructible trees/buildings/rocks; launcher held right in FP and TP; trails on rockets and Ripper spit;
  a better damage system; a jetpack like the game's; a weapon-switch HUD animation; the game's HUD icons; health rework
  with fall damage, smoke at low health and green injections (left hand, push into the body; TP workaround); the tower
  sentry standing inside his tower, and falling when it is destroyed.
- **Destruction (f19093b):**
  - Trees (class 60) topple away from the hit and sink after 25 s.
  - Small rocks (`rock_*`) shatter into EFX_Stone_debri.
  - Houses (class 61, data hp: 330) smoke at 66 %, burn at 33 % and cave in with dust and debris.
  - Guard towers (hp 350/500 from type op 0x31) play guard_tower1.anm — which turned out to be the tower's own
    collapse — then swap to the ruin model (`<name>d.gbs`) with new collision.
  - Debris uses the EFX_*_debri models; the EX_TREE/STON/WOOD/METL/DIRT sounds play.
- **Sentry in the tower:** the world drew the tower's bind pose, which is the *collapsed* pose; the mount point was
  read at frame 0. Towers are now baked at frame 0 ("anim@0" in ModelCache), so the sentry stands on the platform.
  - He stays up while the tower takes damage and is knocked off as it bends (t > 0.9 s).
  - He falls with rpman_hit_fall_cycle and lands with rpman_hit_fall_land; more than 5 m kills him (objective, pool, blood).
- **Particles:** a ParticleSystem with batched billboards (QuadBatch: one draw per texture), using the original
  b_smoke, efx_smoke_black/mgrey, b_dust and efx_fire/bfire/gfire_trail textures.
  - Rockets leave fire and grey smoke; medium spit leaves orange fire, large spit blue.
  - The player trails black smoke below 35 health.
  - Blood decals, drops and sprays moved into the same batching.
- **Weapons in hand:** the Ref_handle / Ref_nozzle01 nodes of mc_gun.anm and mc_launcher.anm are read at load, so each
  weapon is held at its own grip and fires from its own nozzle (FP and TP).
- **Damage:**
  - Reaper headshots (hit more than 1.6 m above the feet) do 2.5×; the Ripper's mouth is a weak spot.
  - The player's own rockets don't hurt him but throw him (rocket jump); blasts shove Reapers.
  - A hit-direction marker; death by falling added.
- **Jetpack (game behaviour from the Speed Demos Archive notes; the numbers are ours):**
  - Air control only while thrusting; air drag otherwise.
  - The tank refills only on the ground; burning it dry overheats it (me_jpkno) until 30 % is back.
  - Turbo = sprint while flying (1.5× speed, 2× fuel).
  - A landing faster than 12 m/s stuns for 2 s with mc_land_hard and does damage: (v − 12)·6 + 5.
  - Flight clips by velocity relative to facing: mc_hover_turbo_back for forward (mc_fly_cycle is an arms-up
    flail, now used only for long drops), mc_fly_left/right, mc_hover_back.
- **HUD (36320d3)** with the original st_mec_* art:
  - st_mec_energy heart filled with st_mec_hbar, and the st_mec_jetpack arrow filled with st_mec_fuel. Each gauge's
    inside is found by flood-filling the outline art from a seed (113 and 126 spans) and filled from the bottom.
  - st_mec_syringe with st_mec_numbers digits; st_mec_gun / st_mec_launcher; objective pips.
  - First person: over the move-hand forearm. Third person: head-locked, lower left.
  - A weapon switch drops the st_mec_wpslot strip in at the top of the view: the chosen slot pops and glows, then
    the strip fades after about 1.9 s, with the ph_selct sound.
  - All HUD layers are batched and drawn over the scene (depth test off).
- **Health injections:**
  - Placed mc_pickup_health objects (class 37) become live pickups: 1c_1 has 3, 1c_2 4, 1d 2; 1a and 1b have none.
    Each bobs, spins and glows, with a green shimmer.
  - Reapers drop one 35 % of the time (our rule).
  - Walk over one to take it (pu_hlth), up to 5.
  - FP: X puts mc_syringe into the move hand; pushing the tip into the chest (0–0.55 m below the head, within
    0.17 m) injects it.
  - TP: X plays mc_health_pup.anm (0.97 s) through a new player "action" override, and the dose goes in half way.
  - Each dose heals 50 over 1 s (me_syrng, green flash). The amount is ours.
- **Tests:** destruction stages/collapse/tree fall; the jetpack (overheat, air drag, stun, ground-only refill);
  the gauge flood fill (ring image, leak and wall seeds); the player action. All pass.
- **Harness:**
  - tower collapse with the sentry falling;
  - house cave-in; palm toppling;
  - launcher at its grip in FP;
  - flight pose; HUD in FP and TP; weapon strip;
  - FP injection: health 30 → 80, 2 → 1;
  - TP injection animation sheet; pickup collected in 1c.
- **Not device-tested.**

### Device test 22 feedback → damage look, launcher grip, self-damage, Rippers, jetpack (2026-10-22, build 845cb14)
- **Reported (build 36320d3):**
  - Injections work flawlessly; no frame drops.
  - Trees fall right, but trees and buildings need a gradual burnt look as they take damage.
  - The launcher isn't held by its handle.
  - The player's own rockets should hurt him, less the further away they burst.
  - Ripper hitboxes are off; big blue-spit Rippers should do more damage.
  - Flying forward shows the legs-up, flying-backwards pose.
  - Small drops play the landing animation, and it glitches.
  - In level 2 (1b) the fuel isn't enough to cross the gaps between hills where the bridges are broken.
- **Damage look:**
  - The data has scorched texture variants (`<texture>_d`: sm_canopy_blue/or_d, s_stony_d, ...). They are swapped in once
    an object is damaged (stage 1); Story1's Smartie houses use none of them (0 found), other worlds may.
  - On top, our own procedural, tileable char overlays: three coverage levels drawn over the model as an alpha pass
    (depth-equal). Coverage grows with the hit points lost; the next level fades in over the previous one.
  - Cut-out parts (palm fronds) can't take an overlay, so they darken and brown instead.
  - Beyond 45 % damage, an additive ember pass flickers along the char's edges, and cools for 12 s after the fall.
  - Stone only scorches (half the char, no embers).
- **Launcher grip:** an offline textured side view of mc_launcher_l0 (`gzptool tris` dump) shows:
  - its Ref_handle is on top of four knuckles modelled over the neck;
  - the obvious handle is a wooden pistol grip running down and back to a knob at the rear.
  - The hand now holds that grip at (0, 3.85, −0.45) game units, in FP and TP.
- **Self-damage:** own rocket blasts do 40·f^1.5 (f = 1 − distance/4 m). Harness: 1 m → 26, 2 m → 14, 5 m → 0. Rocket
  jumps still throw him, with the hit marker.
- **Rippers:**
  - Shots test an oriented box from the model's idle-pose bounds (scale 1: x ±0.65, y 0–1.46, z −0.85..1.03 m) instead
    of the upright 0.45 m capsule; unit test added.
  - Spit damage: medium 10 (was 8), large/blue 28 (was 15), with a 3.2 m splash (was 2.6).
- **Jetpack:**
  - Side views of every flight clip (new `GVR_IDLE_CLIP` / `GVR_PLAYER_TURN` test hooks) show mc_hover_turbo_back is the
    lean-back (legs-forward) pose and mc_thrust the lean-forward one.
  - Forward flight is now mc_thrust; vertical lift uses mc_hover.
  - Landing:
    - the soft-landing clip plays only for drops of 2.3 m or more (8 m/s, was 3 m/s);
    - the air clips need 0.4 s in the air (bumps and small steps keep the ground clips);
    - hard landing / stun / fall damage start at 17 m/s (about 10 m, was 12 m/s, about 5 m).
  - The landing and injection clips play once and hold their last frame; they used to loop, and mc_land_hard (1.47 s)
    restarted inside the 2 s stun. The stun now lasts as long as the clip.
- **Fuel:**
  - 1b's broken bridges (bridge_end pairs) leave gaps of about 61, 97 and 76 m, a few metres lower on the far side.
  - Fuel is now 10 s (was 5), flight speed 12 m/s (was 10), turbo ×1.6, air drag 0.35 (a longer glide).
  - About 120 m powered plus the glide.
- **Tests:** landing (a 1 m drop shows no air/landing clip; a 4 m drop lands once, held), jetpack with the new tank, Ripper
  box raycast, burn textures/levels. All pass.
- **Not device-tested.**

### Device test 23 feedback → fuel, Timmy (2026-10-22, build d07dbb9)
- **Reported (845cb14):** the burning damage look is good; the flight animation is fine; too much fuel. Timmy should
  face backwards (watching what's behind Baz); he keeps disappearing in flight and when changing direction; his
  riding animation is wrong and buggy.
- **Fuel:** 8 s (was 10). At 12 m/s that's 96 m powered plus the glide, still enough for 1b's 97 m gap.
- **Disappearing, cause found:** a trace of the mount per player clip showed mc_hover and mc_fly_left have no
  Ref_HolsterB (nor Ref_Smartie). Their node lists, dumped with `gzptool skincheck`, lack every reference node, so
  updateTimmy returned and he wasn't drawn. Now the holster's transform relative to Baz's spine (present in every clip)
  is remembered whenever a clip has it, and used when it doesn't.
- **Pose:** side and back views of sm_mount, sm_fly_idle, sm_sit and sm_fly_yell1 on Baz (new `GVR_TIMMY_CLIP` /
  `GVR_TIMMY_TURN` hooks):
  - sm_fly_idle is a Smartie sitting high on a Meccaryn's back, facing backwards: the riding clip;
  - sm_mount is a sideways clinging pose.
  - He now loops sm_fly_idle and, while Baz flies, plays sm_fly_yell1/2/3 once every 5–11 s, cross-faded (0.25 s)
    and held on the last frame, not looped.
- Ref_Smartie (0, 2.84, 5.38 game units) is lower on the back; it was tried and not used.
- **Harness:** flight frames 60/300/600 (thrusting, yelling and waving, falling burnt dry); he stays visible and faces
  the chase camera. **Not device-tested.**

### Device test 24 requests → Timmy lean, Timmy talks, terrain variety (2026-10-23, build f811216)
- **Reported (d07dbb9):** Timmy on the back is fine but doesn't lean with Baz. Requested: Smartie noise/dialogue with
  subtitles while he rides (with the waves and yells); more varied terrain (beach, underwater sand, grass and rock
  variations).
- **Lean:** Timmy's rotation is now Baz's spine rotation relative to its idle rotation (model frame, columns
  normalised), applied about the seat.
  - Harness trace: idle 0°; forward flight 72° forward (up → (0, 0.32, −0.95)); hover 13–17°.
  - Renders from behind and side show him tilting with the torso.
  - The earlier "lay him sideways" attempt also used the low Ref_Smartie mount; the relative-rotation maths was
    right.
- **Timmy talks:**
  - The game text (`gzptool text` now dumps GTextEnglish.bin) has the Smarties' riding chat: smchride1–4 ("Uh,
    where are you taking me?", "You're one cool geezer!", "Do that jetpack thing again!", "I feel sick."),
    smchpickup2 ("I've hopped on!"), smchbaseattack1/4.
  - No matching voice files exist (StreamEnglish has only BAZ006, BSCREAM1, GOR002, TIM001 and TEL001), so each
    line is voiced with a gsm01–04/13/14 Smartie clip pitched ×1.18. That pairing is ours.
  - Lines and when they play:
    - "I've hopped on!" 2 s after he's on;
    - ride1/ride3 with his flight yells;
    - ride4 after a hard landing;
    - ride2 (50 %) after a kill;
    - baseattack1/4 when Baz is hurt below 45 health.
  - 10–16 s between lines. Shown as "Timmy: …" in the game font, head-locked, right of centre (clear of the
    third-person HUD), for 3.2 s.
- **Terrain:** a terrain material in the shader (one mesh per chunk, smooth heightfield normals):
  - the world's ground texture, with earthy patches (Rstory1_ground, about a third of the area);
  - a large-scale lush/dry tint;
  - rock on slopes, projected on the dominant side axis so cliffs don't stretch, two rocks (world rock +
    story3_rock) in large patches;
  - beach sand 0.8–4.6 m above the sea and on the sea bed (story3_grnd's desert sand, paled), wet at and below
    the waterline;
  - grain up close (within 30 m);
  - our own tileable noise texture drives the blends.
  - Cost: up to about 8 texture samples per terrain pixel (branches skip unused layers); the adaptive quality fallback
    (level ≥ 2) drops to ground + rock + sand. Debug renders of the weights checked the blends. **Frame cost on
    Quest not measured.**
- **Not device-tested.**

### List item 4: finishing Story 1 (2026-10-23, build 5b56bea)
- **Story 1 → Story 2:**
  - 1d already completes: reaching the hatch at game (3976, −2106) finishes it, and story1_hatch_into_mine plays.
  - The story stopped there: Story1's last mission has no next mission.
  - Now, after a world's last mission, `AssetWorld::switchWorld` loads the next Meccaryn world (Story1 → Story2 →
    Story3 → Story4 → Story5 → the_end). It frees the old terrain, sky and sea, and keeps the archives, model cache
    and registry; GameApp starts its first mission through the usual briefing/loading screen.
  - Story2, Story3 and Story5's world streams stop before the scenario list (B5: ops 110/35/0). Their missions are
    found by file name instead, in story order:
    - Story2: 2a_1_getmeat … 2c_2_borjoyzee (5 missions);
    - Story3: story3_sonak_1 … story3_prison_7 (7);
    - Story5: 5a, 5c;
    - Story4 parses fully.
  - The jetpack stays after Story 1 (2a's item list has no item 1).
  - Harness: 1d → its outro → "Chapter complete: next world Story2" → 2a_1_getmeat at its start marker (Story2
    terrain, objects, Rippers, Reapers).
  - **Still missing in Story 2:** its objectives (2a is "take Vimp meat to Timmy's Dad": Vimp-meat and destination
    triggers aren't implemented, B9), so 2a can't be completed yet; the world's own static objects (stream stops
    early, B5).
- **Dactyls:** 1a's two Dactyls (class 45) were looping their wing-beat clip in place.
  - Now each flies a wide circle round its placement: radius 25–50 m that slowly breathes, 14–26 m up and rising
    and dipping, 8–11 m/s.
  - They bank into the turn (`CharacterInst::xf()` gained roll and pitch) and are drawn out to 260 m. The path is ours.
- **Smartie villagers (ours):** the Story 1 missions place no Smarties in the villages.
  - Clusters of smw_house objects become villages: 1a's 18 houses get 8 Smarties; the 4-house village by the 1c_2/1d
    hatch gets 6. sm_L0 with the original clips: sm_stand, sm_hwalk, sm_scan, sm_talk1, sm_listen1, sm_laugh1,
    sm_stand_sing, sm_tai_chi, sm_sit, sm_celebrate, sm_wave, sm_Run_terror, sm_cower.
  - `VillagerDirector`: acts for 4–18 s or strolls to a point near home (not through a house: collision ray). They
    wave at the Meccaryn within 5 m (20 s cooldown).
  - Reapers/Rippers within 22 m, a blast within 30 m, or the player's gunfire make them run away and cower until
    6 s after it's quiet. Posed only within 70 m.
  - Unit test (stroll near home, flee away, cower, calm, wave). Harness render of a Smartie in 1a's village.
- **Not device-tested.**

### List items 5–6: checkpoints and the options menu (2026-10-23, build 0a8b1c0)
- **Checkpoints (e3d512c):**
  - `save.txt` (version, world, mission, injections, launcher) is written at every mission start, atomically
    (temporary file + rename). On Quest it sits in the app's external files directory next to `Bin`.
  - Launch continues from it; a world that won't load falls back to Story1.
  - `GVR_MISSION` (tests) overrides it; the headless harness writes one only with `--save <path>`.
  - New game: hold X 3 s in the free camera (tapping X still skips a mission, now on release), or from the options
    menu.
  - Harness: a fresh start writes 1a; a hand-written 1c_2 checkpoint resumes there with 3 injections and the launcher;
    a Story2/2b_1 checkpoint opens Story2 at 2b_1. Unit test for the file round trip and rejecting damaged files.
- **Options menu (0a8b1c0):** the left controller's menu button opens it and pauses the game.
  - It is world-fixed 1.3 m ahead, in the game font.
  - Rows: Resume; Turning (snap 30°/45°, smooth 90 or 150°/s); Main hand; View (first/third person); HUD size (0.8/1/1.3
    ×, scales the forearm/TP panel and the weapon strip); Volume; Music; Subtitles (cutscenes and Timmy); New game
    (hold the trigger 2 s).
  - Either stick chooses (up/down) and changes (left/right) with repeat; trigger/A activates.
  - Settings go to `settings.txt` beside the save and are applied at launch. The HUD hides while the menu is open.
  - Harness render of the menu; unit test for the settings file.
- **Not device-tested.**

### List item 8: VR feel; item 7 prep (2026-10-23, build 8626e3c)
- **Rumble:**
  - Hits: 0.35 + damage/40, stronger on the side the hit came from (from the hit-direction state).
  - Jetpack: a 0.55 kick when it fires; a 0.13 hum (0.3 on turbo) refreshed every 0.1 s.
  - Landings above 4 m/s, scaled by speed.
  - A double-thump heartbeat below 30 health, faster as it drops.
- **Holsters:**
  - The weapon not in hand rides on the body: the gun barrel-down on the right thigh (Ref_holsterR), the launcher
    slung across the back below Timmy (Ref_HolsterB).
  - Both are remembered relative to a bone every clip has (leg R / spine), since the flight clips lack reference
    nodes (`GameApp::bodyRef`). The back launcher is drawn in third person only.
  - First person: reach the weapon hand to the thigh (0.3 m) or over the shoulder, and grip, to draw it (strip
    animation and sound); stick-forward switching still works.
  - Harness: front view with the gun on the thigh, back view with the launcher slung.
- **Frame-rate counter** (options → FRAME RATE): smoothed fps, head-locked top left (green ≥ 70, yellow ≥ 60, red
  below), so the user can report frame rates without adb for the performance pass (list item 7, which needs the device).
- **Not device-tested.**

### List item 6: visual upgrades (2026-10-23, build 2d57afb)
- **Day and night (ours; the original's light is fixed per level):**
  - A 20-minute cycle: sun height = sin(2π·phase)·0.78 + 0.22, so about 12 min of day, dusk and dawn, ~5 min of
    night; it starts mid-morning.
  - Daylight scales and tints, from each world's own captured colours:
    - sun light: warm when low; at night the moon on the other side of the sky lights and shadows the scene;
    - ambient, fog/clear/horizon, zenith and water colour;
    - the painted sky (new `skyTint`);
    - baked lighting (new `bakedLight`, multiplied into the lightmap path, which otherwise ignores the sun).
  - Stars: a hashed cell grid on the view direction in the sky shader, faded in with night, twinkling.
  - Options → DAY AND NIGHT: CYCLE / ALWAYS DAY (the previous always-up circling sun). `GVR_DAY_PHASE` test hook.
  - Harness renders at phases 0.25 (day), 0.53 (dusk), 0.75 (night), 0.97 (dawn).
- **Sharper textures:** anisotropic filtering (4×, GL_EXT_texture_filter_anisotropic when present) on every mipmapped
  texture: ground and walls stay sharp at grazing angles. On Quest it costs some texture bandwidth; unmeasured.
- Not done from the item's wish list: light shafts (would need a post-process pass on Quest; skipped); water foam
  already existed.
- **Not device-tested.**

### List item 7: next characters - Delphi (2026-10-23, build 93a8eed)
- **Hero by world:** Rstory* worlds play as Delphi (rp_l0), every other world as the Meccaryn.
  - `GameApp::setHero` swaps the player model and the PlayerAnim→clip table, and sets player params.
  - Delphi: a jumper (A jumps, 10 m/s, uncapped; air steering), run 5.5 m/s, turbo ×1.9 with rp_turbo, swim 4.2
    m/s with rp_swim_breast / rp_swim_treadwater, hard-landing threshold 22 m/s, no jetpack.
  - The Meccaryn's models and params are kept and restored on the way back.
  - PlayerAnim gained Swim, Turbo, Attack1–3 and Cast (the Meccaryn maps them to existing clips).
- **Clips:** rp_stand, rp_run, rp_jump_cycle, rp_runjump_cycle, rp_fall, rp_land, rp_land_hard, rp_side_r, rp_slide,
  rp_swipe_1/2/3, rp_spell_arm_bolt, rp_swim_*.
- **Sword (rp_sword.gbs):**
  - First person: in the weapon hand along the controller's pointing direction. A blade moving faster than 3 m/s cuts
    what it sweeps (a ray from the tip's last position to its new one, and along the blade, through the same raycast
    and onHit as bullets, ×0.6–2.5 by speed), with a 0.4 s per-target cooldown.
  - Third person: on Ref_Rhand (as the cutscenes attach props). The trigger starts a three-swipe combo (rp_swipe_1/2/3
    as one-shot actions); the hit lands a third in, on everything within 2.8 m in a ±70° arc in front, ×2 (×3 for the
    third swipe).
  - Sounds: SR_SWSH1/2 swish and SR_SWHT1–4 hits (names read as such).
- **Fire bolt:** the move hand's trigger, 1 s cooldown; a rocket of kind 1 at 28 m/s drawn as fire particles; it bursts
  with a 2.6 m blast for 75 (self-damage rules as for rockets). SR_SPFSH (provisional).
- **HUD:** st_rp_sword as the weapon icon; st_rp_firewall, lit when the bolt is ready, where the jetpack gauge was.
- **New game** (options) now chooses MECC STORY (BAZ) or REAPER STORY (DELPHI). Chapter chains: Rstory1 → Rstory2 →
  Rstory3_race_1..3 → Rstory4 → Rstory5, Kstory1..5. The missions of incompletely parsed Kstory/Rstory worlds are found
  by file name (k1a_…, kstory3_…, r1_…, rstory2_…).
- **Harness:**
  - Rstory1 with Delphi running (sword in hand);
  - a jump (3.5 m apex);
  - a TP swipe;
  - FP sword with the controllers;
  - the menu starting the Reaper story from Story1 (hero switch at run time).
- **Not done:** her first-person body (the Meccaryn's arm IK uses his bone names); her own story's Reaper-side
  mechanics (Smartie grabbing, spells beyond the bolt); Kabuto (next).
- **Not device-tested.**

### 2026-10-23 — Kabuto (build 866dfdc)
- **Hero::Kabuto** for Kstory worlds (`heroForWorld`), in `game_app_kabuto.cpp`; `setHero` rewritten for three heroes
  (each hero's model parked and restored; per-hero death clips: mc_die…, rp_die / rp_land_dead, kb_die / kb_drown —
  fixes Delphi having used the Meccaryn's clip indices and chunks on death).
- **Size (measured):** kb_l0 in kb_stand is 17.3 m tall (posed bounds y −0.2…17.1 m at 0.2 m per game unit), eye 13.4 m
  (front of the head bone's vertices). heroScale_ = 8.3 (body / the Meccaryn's 1.8 m).
- **Giant scale:** `rig_.comfort.worldScale = kabutoEye_ / userEye_` (≈ 8.4) while playing as Kabuto (1 in cutscenes,
  loading and the free camera). Every tracked pose is scaled, so the eyes (IPD), head motion and hands are a giant's;
  physical walking and the FP camera placement scale with it. TP camera distance, height, collision margins scale with
  heroScale_.
- **UI at human scale:** `pushUnscaledRig` re-roots the tracking space at scale 1 about the head (same head position and
  heading); `beginUiFrame` re-begins the eye view from it. HUD (built in update), Timmy line, briefing, menu (open,
  update, render) and controllers are drawn in that frame, so they look exactly as for the Meccaryn.
- **Moves:**
  - Clips: kb_stand, kb_walk (8.7 m/s, its measured foot travel), kb_run1 on turbo (13 m/s; new
    `PlayerParams::turboStride`), kb_side_r, kb_slide, kb_jump_up_loop / kb_run_jump_loop / kb_fall, kb_land_light,
    kb_land, kb_mud_walk (deep water).
  - A jumper: 24 m/s take-off with gravity 30 (≈10 m up). No fall damage. Capsule 1.6 m radius, 14.9 m tall, step 2.5 m.
  - Takes 30 % damage.
- **Combat:**
  - Third-person right trigger: kb_punch / kb_punch_lft alternating; the hit lands at 45 % on whatever is in front at
    0.25–0.55 of his height (×5).
  - First person: fists are the controllers. A swing faster than 2.2 m/s (real) smashes what it meets (×1.5–7 by
    speed). A fast blow down to the floor is a ground pound (quake, 4.5 m radius).
  - Left trigger: kb_stomp, a quake (6.7 m radius, 90) at his front foot.
  - Landing from a big jump: a quake too.
  - A quake is `explode(…, fire=false)`: no fireball and no self-throw, dust.
- **Eating and other actions:**
  - X: grab and eat the nearest Smartie, Reaper or small Ripper within 9 m in front. kb_eat_small plays; the victim is
    gone at 30 % and heals 25 (KA_SMRT1/2 for Smarties, KA_EAT1/2 otherwise; names read as such).
  - Y: kb_roar (shortened to 3 s), Reapers pushed back, 6 s cooldown.
  - Walking into trees and Smartie houses lower than 70 % of his height tramples them.
  - Village Smarties now fear him.
- **Sound and HUD:**
  - Sounds: KA_FS* footsteps by surface with a rumble and dust per step, KA_PNCH, KA_LTHT/MDHT/HVHT hits, KA_STOMP,
    KA_LNDHD, KA_GRAB, KA_BELCH, KA_SNIFF, KA_ROAR1–4, KA_WOUND, KA_DETH1/2. All 163 preloaded sounds found.
  - HUD: health only (no injections or weapons).
- **Menu:** NEW GAME cycles MECC / REAPER / KABUTO STORY (Kstory1).
- **Not done:**
  - His first-person body. The view shows controllers as his hands.
  - Kstory missions' own objectives: k1a has none parsed. The world stream stops early (B5); mission objects load.
  - Kabuto's growth from eating, adrenaline / spikes moves.
- **Not device-tested.** Comfort at 8× scale (IPD ≈ 0.5 m) and the frame rate in the village are the things to check.

### 2026-10-23 — Main menu, loading art, music per world (build 42ac61a)
- **Title-scene data found:** w_intro_island (terrain, night sky, sea), flk_intro_island_1 (a 66.7 s camera flight,
  nodes Object01 / Text01 / Text02 = the logo's parts) and flk_intro_island_2 (3.3 s hold). Its camera rests at game
  (−3231, 4058, 374) looking at (2759, −2021, 2869), about 16° up. Giants_logo_3D.gbs: the 65 m G / IANTS / CITIZEN
  KABUTO logo (Giants_logo_512 + _rep textures), in xx_giants_logo_3d.gzp. The same archive holds the original loading
  screens mecc_loading, reaper_loading and kabuto_loading (640×480), giants_loading and MP_screen.
- **Main menu** (`game_app_title.cpp`, enabled by the Android entry point; `--main-menu` in the harness):
  - The app starts in intro_island. The viewer stands where the original's menu camera rests, facing its direction,
    with world colours (no day/night).
  - The logo floats 110 m out, 13° up, unlit, gently bobbing.
  - The panel offers:
    - CONTINUE: the checkpoint's mission by its title.
    - NEW GAME: Mecc, Reaper or Kabuto story. Held 1.5 s when a checkpoint exists, since it replaces it.
    - OPTIONS: the usual menu.
  - The chosen story's loading art stands beside the panel.
  - CONTINUE switches world straight to the checkpoint's mission (new `LoadState::worldMission`) and restores what was
    carried. The title world writes no checkpoint and gets no test showcase. Without the title world in the data, the
    app starts the game as before.
- **Loading screen:** the destination's race art on the left, title and objectives on the right.
- **Music:** the ambient set now follows the world on every mission start (was fixed at app start):
  - race prefix m / r / k, a1 on odd chapters, a2 on even (ours);
  - the menu plays ma2 (ours);
  - Kabuto has no small-battle set, so his small fights use kff.
- **Delphi:** regenerates health while swimming (6 / s). Her own briefing text says so: "You can regenerate by
  immersing yourself in deep water if you are badly wounded."
- **Fix:** Kabuto and Delphi never get the jetpack on later missions. initMission used to re-enable it for any non-Mecc
  mission when the hero didn't change.
- **Harness:**
  - Main menu with and without a checkpoint.
  - CONTINUE into Story1 / 1b (syringes restored) and into Kstory1 (plays as Kabuto).
  - NEW GAME → Reaper story with the hold (checkpoint replaced).
  - Loading screens with Mecc and Reaper art.
  - Story1 start render byte-identical to before.
- **Not verified:** the menu music. Only part of the Music folder is in the cloud copy; the segments it needs are on
  the PC.
- **Not device-tested.**

### 2026-10-23 — World streams fully decoded (B5 resolved)
- **Method:** a Python walker over all 33 w_*.bin streams. Every size hypothesis was kept only if the streams then
  ended exactly on the 0xFF terminator.
- **Sizes fixed:**
  - Op 50 is 3 bytes (op + u16: 8, 12 or 20), not 6. The old size swallowed the next record's first 3 bytes. This fix
    alone completes 19 worlds (all multiplayer maps, Kstory2/5, Rstory2/4/5, Story5).
  - Op 35: 17 bytes (a 16-char name, like 29–34 / 36–38): "c" in Story3, "menu_null" in menus.
  - Op 44: 33 bytes (a 32-char name: "kabuto" in Kstory1/3/4).
  - Ops 108–111: 65 bytes, two 32-char names. In w_Story2 these are music set pairs: msu/msuend, msb/msbend, mwb/mwbend,
    mlb/mlbend.
  - Result: 33/33 worlds parse to the end (w_menus 2 bytes short of its stated length).
- **Effects:**
  - Later worlds get their full object lists. Story2 places 250 level objects (was 48), Story3 98 (was 92), Rstory4
    419. Kstory1's 128 world objects now load (was 0).
  - Each world's own scenario list (op 92) is reached, so missions come from the data: kind 2 = mission, kind 1 =
    intro, kind 3 = the next world, kind 4 = "startmenu123" in the_end. This replaces the file-name fallback, which is
    kept for data that doesn't parse. Examples: Kstory2 = k2a_adrenalin, k2d_offspring; Rstory4 = r4a_buildbase.
  - **The campaign order is the original's:** Story5 → rstory1, Rstory5 → kstory1, Kstory5 → the_end. The chapter end
    now follows the data (`AssetWorld::nextWorld`), with our table as the fallback. The main menu still starts any of
    the three stories.
  - Story2's world music sets are used: msu as its ambient, msb / mlb in its battles. Reading 108 as ambient, 109 /
    111 as small / large battle is provisional.
- **Harness:**
  - `--complete-at` hook: completing 5c_securebase goes on to rstory1 as Delphi.
  - All 33 worlds load.
  - Story1 start render byte-identical to before.
  - Story2 shows its palms and logs; Story3 its own sea texture.
- **Not device-tested.**

### 2026-10-23 — Mecc story objectives: Vimps, buildings, bosses (Story 2–5 completable in the harness)
- **Trigger kinds** (named in the decompiled `mission_trigger_name`, a factual reference only): 1 proximity, 2 death,
  3 destination arrival, 4 "On Vimp Meat Proximity". Kinds 1 and 2 were done for objects and Reapers/Rippers. Across
  all 22 Mecc missions, 7 still had no objective.
- **Vimps** (`vimp.{h,cpp}`, `game_app_vimp.cpp`):
  - Type 8 (vp_l1, class 21, 20 hp) is placed by no world or mission. The original spawns them in native code (its
    "-novimps" switch).
  - Ours: Story2–5 and Kstory worlds get 6 herds of 3–5 on open grass, away from villages, bases and the start, at
    deterministic spots per world. They graze (vp_graze / vp_idle) and amble (vp_walk). The player within 16 m, or
    shots and blasts within 45 m, set the whole herd stampeding 60 m away (vp_gallop), uneasy (vp_scared) until calm.
  - Hit: vp_hit and VI_INJR. Killed: vp_hit2 (ends lying down), VI_DEAT, blood, and 3 pieces of meat
    (VimpFoodChunk1–3) that fly out, bounce and lie.
  - Walking over meat picks it up (PU_MEAT1/2). The HUD shows st_vimpmeat and the count, gold at 20. Kabuto eats meat
    (+5 health, KA_VIMP) and whole Vimps (X). Delphi's sword, Kabuto's blows and every blast hit them.
- **Kind 4:** bringing 20 pieces (2a's briefing: "Return 20 PIECES OF VIMP MEAT") to the trigger completes it.
  - The vimp_dropoff marker floats over the trigger.
  - Arriving short shows "VIMP MEAT: n OF 20 - HUNT SOME MORE VIMPS!".
  - 2a now completes and leads to 2b.
- **Buildings as death targets:** trigger 2 on a structure becomes "destroy it", matched to the destructible of that
  type nearest the trigger.
  - Newly destructible: classes 98 (Ripper and Reaper barracks) and 101 (bw_prison, story4_door) with their data hit
    points, plus log_launcher_base (250 hp) and guard_army_barracks (600 hp, ours, as the data has 0).
  - Missions: 2c_1 (5 barracks), story3_bunker_3, story3_launcher_5, story3_prison_7, 4a, 5c.
- **Group spawners** (class 97: 1mcsonak_4guards, 1tsonak_4guards, 1tsonak): their guards appear round them as Reapers,
  and the group dies with them (story3_sonak_1, story3_3sonaks_4). The Sonak itself, a flying mount, is not
  implemented.
- **Characters with triggers:**
  - Trigger 1 = reach them: Borjoyzee in story3_borjoyzee_2.
  - Trigger 2 without our AI = shootable targets with the data's hit points: Offspring ×2 in 4b (500 hp each), the
    Charger in 5a (500 hp). They sink away when killed. They don't fight back yet.
- **Result:** every one of the 22 Mecc missions now has at least one objective.
- **Harness:**
  - 2a meat delivery → 2b loads.
  - Blasting story3_prison_7's prison → Story 4.
  - Blasting the Charger in 5a → complete.
  - A Vimp herd blasted: 3 killed, the rest stampede.
  - Unit test for herds: graze, stampede direction, damage, carcass removal.
  - Story1 render unchanged.
- **Not device-tested.**

### 2026-10-23 — Reaper and Kabuto story targets (build 77eef3a)
- **Buildings:** every building a death trigger names now takes damage and collapses, with hit points ours where the
  data has none: Kabuto's giant walls and gates 800, the Reaper queen Sappho 1500, the ships in r4a.
- **Hero choice:** the mission's race (setup op 0x0B: 1 Kabuto, 2 Mecc, 3 Reaper) picks the hero. kstory5_5 is played
  as the Meccaryn against Kabuto.
- **The other Kabuto** (class 1 placed by a mission, or ours for kstory5_4, "Kabuto, meet Kabuto", which places only
  the player's Kabuto as id 1 and a marker as id 3):
  - Full size, 2500 hp.
  - Walks up, punches (28), stomps (22 in a quake), roars, and falls with kb_die.
  - Shot by the Meccaryn, the belly takes triple damage and the rest a quarter, after kstory5_5's hint "Fire at the
    WEAK SPOT on Kabuto's stomach".
  - kstory5_4 starts at the player's own placed Kabuto.
- **Not device-tested.**

### 2026-10-23 — The Reaperski (build 049ffc4)
- **Course data** (per race mission):
  - buoy_spline objects = the gates;
  - r3_start_finish = the start/finish arch;
  - marker_spline grouped by id = the rivals' raw racing points;
  - rpman_l0 with id 11 = rival riders, id 12 = the starter;
  - p_jetski_turbo / _missile / _homing = pickups;
  - rp_jetski = a moored ski (rstory2_jetski_*).
- **Gate order:** most gates are listed in order, but some were appended out of place (rstory3_race_2: 15 of 19). The
  driving order is a closed tour from the start (nearest neighbour + 2-opt, land crossings costed ×7), run in the
  direction that agrees with the data's own order.
- **Riding** (`stepJetski`, ours):
  - Speeds: 21 m/s, 31 on turbo; turning needs way on.
  - Sandbars and ramps are climbed at speed; it leaves the ground off a ramp (gravity 16).
  - Steep faces knock it back.
  - Gate buoys and the arch are ridden through.
  - Ice walls (150 hp, rstory2_jetski_3's hint) break to rockets or a turbo ram.
  - Delphi rides with rp_Jetski_ride / rp_jetski_L_turn / R_turn.
  - Engine loops and the other sounds: JS_* (js_ridl1 idle, js_rmve1 moving, js_buoy gate, js_mbuoy missed, js_win /
    js_lose…).
- **Rivals:** RPm_jetski with rpman_l0 on rpm_jetski_ride, each on a water-routed line through its own side of every
  gate (A* over the heightfield, simplified to corners). Pace 15.5–19 m/s with a light rubber band. Rockets spin them
  out.
- **Rules:**
  - A 3 s countdown.
  - "If you miss 3 GATES, you lose the race."
  - First over the line wins the mission; any other place restarts it after 5 s.
  - HUD: position, gate, misses, turbo seconds and missiles.
- **Harness** (`GVR_SKI_AUTOPILOT`, full throttle along the middle line, no turbo): won rstory3_race_1 (182 s),
  race_2 (213 s), race_3 (233 s) and rstory5_race_1 (283 s) with no gate missed. Before the rival pace was lowered,
  the autopilot came 2nd and 3rd in two of them. Unit test: a square course (win), three misses (lost), handling (top
  speed, bump on land, stick right turns right).
- **Not device-tested.** Comfort is the main question: the view turns with the ski, smoothed.

### 2026-10-23 — Bosses fight back, Sonaks, SAM turrets (builds 0ead8b1, 99306c2, 6e3a8cb)
- **One boss AI** (`updateBosses`, ours) with a table per kind: clips (stand, move, attack 1, attack 2, roar, die), speed,
  reach, damage and sounds.
  - Approach, then two kinds of blow when in reach, and a roar when far off.
  - Kabuto: punch 28, stomp quake 22.
  - Offspring (4b): claw 14, bite 20; OF_ sounds.
  - Charger (5a): gore 18; it also lowers its head and charges at 17 m/s from 14–80 m, 30 damage and throws the player
    aside. CA_ sounds (CA_ROAR, CA_SNORT…).
  - Sonak: roar attack 16, stomp 24; SO_ sounds.
  - Dead beasts sink away after 5 s; Kabuto lies.
- **Sonaks:** sonak_l0 (class 39, 350 hp) is a walking war beast, not a flyer: walk, roar_attack, stomp, die. Each class-97
  Sonak group spawner now gets a Sonak and its Reaper guards. story3_sonak_1 has 5 objectives, story3_3sonaks_4 has 15.
- **SAM turrets:** guard_sam (class 44, 200 hp) is now destructible. Ours: a homing missile (Rocket kind 2) at a flying
  player (3 m above the ground or more) within 160 m, every 4.5 s, at most 3 in the air.
  - The missile climbs out of the tube and steers with a rate that tightens as it closes (an earlier version circled
    the target).
  - It bursts within 3 m, on the ground, or after 9 s.
  - Launch sound PR_WSMIL.
  - From 2b's briefing: "detect airborne targets and hammer them with homing missiles". Flares (from the Shop) and the
    bush disguise are not implemented.
- **Harness:**
  - 4b: the Offspring kill a standing player in 5 blows.
  - 5a: the Charger closes and gores.
  - kstory5_4: the rival Kabuto punches.
  - story3_sonak_1: the Sonak and guards attack.
  - 2b: a hovering Meccaryn is hit by a SAM missile 2 s after launch.
  - Story1 render byte-identical.
- **Not device-tested.**

### 2026-10-24 — The 16:45 list: burrowing Rippers, jetpack, water, tags, Timmy, Shop, VR map (builds 63c9816 … 8515055)
- **Earlier the same list** (committed 2026-10-23): building health bars, the collapse frame drop (fx culling), readable
  injection count (measured digit spans), third-person weapons following the hand, pink player bolts / white Reaper bolts.
- **Rippers burrow** (creature.cpp states Buried / Tunnel / Emerge / Dive / Latched; the clips are the original's:
  rip_buried_idle, rip_popup_1, rip_dive, rip_attack_mecc - their root moves the body 2.3 m underground, so the model
  keeps its standing height for them).
  - 90 % of mission Rippers start buried (GVR_BURIED for tests). They come up when the player is within 24 m (any side)
    or a shot / blast lands near (disturb), tunnel at him (8.5 m/s; large ones 0.4x), burst out 2.8 m away biting.
  - Out of the player's reach or sight for 8 s they dig back in; not in water; buried ones can't be shot.
  - Dirt thrown up ahead of a tunnelling one, rumble (BI_DIRT1); on bursting out dust, Dirt1 chunks, EX_DIRTL/S and a
    hole: the original Mound model is a low rim round a dark hole (b_mound.tga) - used as the pothole with its open clip.
  - Small nests (all five in 1a are 0.81 m) breed mini Rippers (type 229 at 0.55 scale). Small Rippers may latch onto
    the player's legs (3–6 s, 3 dps) and jump off.
  - Spit 2.4 + 1.5 s cooldown at 21 m/s. Dead Rippers leave their spit: orange +15 health, blue +40.
- **Jetpack:** the original thrust models (mcthrust2, MCthrust_nitro, mcthrust star) at the pack's Ref_Thrust node,
  blue plasma particles, a ground glow and dust. New renderer point light (one, from the game) gives the blue light on
  the body, hands and ground - what shows the jetpack in first person. Dry in the air with the button held: falls at
  most 4 m/s and loses forward speed (no fall damage); the flame flickers, smoke coughs, sputter sounds.
- **Water:** efx_splash's clip only carries 22 debris reference points (the mesh doesn't move) - so the crown is
  animated by us (open, leap, fall back) with drops (b_drip) and spray (EFX_splash). Shots ending under the surface
  splash where they went in (BI_WATR1). Spray, drops and a ring wake at the legs wading or swimming. Piranha bites:
  blood spurting, red clouds, KA_BITE (pitched up) and squelches.
- **Name tags:** Smartie names from GTextEnglish's name syllables (smnamesyla/b/c, 4 each: Bor/Mul/Lum/Hal +
  fiz/munz/bolzt/der + zee/burg/stein/sun). Shown when within 5 m or looked at; enemies by kind. Picked-up Smarties say
  smchpickup / smchride / smchdropoff lines as subtitles with their name.
- **Timmy (1a):** keeps his data height on the tower deck (2.4 m up) and loops timmy_swing_jetpack; Rippers within
  18 m of him stay above ground.
- **Shop:** guard_shop (class 102) is a world object in 1d (game 3430, -524). Its interior is cramped, so the counter
  opens where the player first walks in: launcher (if not owned) and injections at arm's length, once per level; a
  health pool (150 points, 25/s, never refills). Ours: the original shop's menu and stock are not decoded.
- **VR map** (Y): a holographic island table, live markers, circling objective dots with beams, fog of war (90 m).
  Vertex colours are given gamma-corrected (the eye buffers are sRGB).
- **Not device-tested.**

### 2026-10-24 — The saved-for-later items: flares, bush pack, Kabuto growing (builds 41f2a89, 14386c1, 3886e95)
- **Delivery of d982b4b** to the PC was verified by sha256 (the computer was offline at first; the files were also sent in
  the conversation).
- **Flares** (2b briefing: "throw homing missiles off your trail by using FLARES, found in the Shop"): the guard_shop
  exists in Story 2, 3 and 4 too (4a has two), so every level with one gets the counter, now with 3 flares (max 6).
  Move-hand trigger throws one (ME_FLARE, st_mec_flare on the HUD); it burns 5 s; a SAM missile within 120 m homes on
  it. Harness 2b: a missile bursts 2.7 m from the player without a flare, 5.9 m away with one (no damage).
  The original's efx_flare model is a small green smoke card, not used.
- **Bush pack** (2b on): the original assets are all there - MCbush (in / idle / shake / out), mc_bush_* body clips,
  rpman_poke_bush (6.6 s) and rpman_bush.wav, st_mec_bush. Fourth weapon-strip item. No jetpack or weapon, 2 m/s,
  drains the jetpack (16 s a tank). Reapers (enemy.cpp `playerHidden`): unaware ones walk up and poke a bush within
  3.5 m, or a moving one in front within 9 m, and find the player if he's still within 2.6 m; engaged ones lose him
  after 3 s beyond 8 m. Harness 2b: shot at 10 m without the bush; never noticed in it standing still; walking past,
  the Reaper goes to poke and finds nothing.
- **Kabuto grows** (k2a briefing: "Eat 5 Smarties to reach full strength and size"): starts k1a and k2a at 60 %
  (our reading), +8 % a Smartie, +5 % a Vimp, +3 % anything else, eased; height, eye, reach, speeds, jump, steps follow.
- **Not device-tested.**

### 2026-10-24 — Host-side performance pass (build 075a50f)
- New opt-in profiler (`GVR_PROFILE=1`, core/prof.h); the harness prints update average / worst and world draw counts.
- Finding: the update's main cost was skinning every pooled Reaper each frame regardless of distance (2b: 12 Reapers
  300+ m away). Now: none beyond 130 m (400 m as Kabuto), every other frame beyond 45 % of that; villagers beyond 25 m
  every other frame. Host averages: 2b 1.10 → 0.37 ms, Kstory1 0.82 → 0.43 ms, 1a tower 0.29 → 0.26 ms, 1d 0.56 →
  0.19 ms. (Host x86 numbers: relative only; the Quest's cores are several times slower.)
- The VR map mesh was built on the first frame of play (~5 ms); now at mission load.
- Objects under ~0.5° across are culled (1a tower view: 508 → 471 draw calls per eye).
- Not done, needs the headset: GPU timing; multiview (one pass for both eyes) would halve draw calls but changes the
  swapchain and every shader - not attempted blind.

### 2026-10-24 — First-person bodies for Delphi and Kabuto, CPU pass 2, Delphi's spells (builds 07bbbc8 … d6ef359)
- **First-person bodies:** Delphi (rp_l0) and Kabuto (kb_l0) get the Meccaryn's treatment - head hidden, eye point from
  the idle pose, arm IK to the controllers. Chains: Delphi L_shoulder > L_arm01 > L_arm02 > L_arm03 (fingers ordered index
  to little by distance from the thumb: L_fingers11/10/06/01, R_finger13/01/06/02); Kabuto L_shoulder > L-arm > L_wrist >
  L_hand (two fingers left, three and a thumb right). The IK chain needs two fingers to orient the hand (ArmChain::valid);
  without them it silently did nothing. The body's scale now includes the view's world scale, so Kabuto's is giant.
- **CPU pass 2:** Reaper animation LOD (every frame < 25 m, every 2nd < 60 m, every 4th beyond; ×3 for Kabuto); line of
  sight re-checked every 0.15–0.25 s. `scripts/mission_sweep.sh` runs all 56 missions: exit 0, no errors; mean update
  0.34 → 0.27 ms on the host.
- **Delphi's spells** (from the briefings: fire wall r1_saveyan, bubble r1_turbo, tornado for the barracks): tap = fire
  bolt or, aimed at a Smartie, a bubble that brings it to her; hold 0.7 s = fire wall (EFX_fire_roll × 7, 3 s, 6 m/s);
  hold 1.6 s in the battles = tornado (EFX_swirl funnel, 7 s). As Delphi only the tornado damages guard_army_barracks.
  Harness: rstory2_battle_1 completes with one tornado; the bubble catches the r1_smartiegrab Smartie and sets it down
  by her. The real spells' numbers are native code (ours).
- **Not device-tested.**

### 2026-10-24 — Kabuto's story powers and the magnifying glass (builds 31f0064, this build)
- **Gates by size** (giantwall1/2/3): only fall to a Kabuto of 70 % / 85 % / full size + adrenaline; growth starts are
  k1a 50 %, k1b 65 %, k2a 80 % (our reading of the briefings' "weakened"). Harness: k1a's gate holds at 50 % and falls
  at 75 % (800 hp, 30 blasts).
- **Adrenaline**: a gauge that fills at full size; both grips when full = 10 s of ×2.5 damage, bigger quakes, red light.
- **Offspring**: hold Y 0.6 s to lay an egg (offspring_egg_l0); an offspring_l0 hatches and hunts within 120 m, eating
  and growing (up to four). Harness kstory4_1: one Offspring kills four Reapers and grows 0.63 → 0.89.
- **Magnifying glass** (kstory3_3 / kstory4_1 briefings): destroying mag_crystal leaves mag_crystal_shard (2.4 × 3.5 m)
  where it stood; walking over it takes it. Held, the turn hand's trigger raycasts (weapon_.raycast) from the lens (FP:
  the right controller's aim; TP: chest-forward, tilted down) up to 3× his height: a fire cone, a warm point light and
  scorch smoke where it lands; every 0.25 s 60 hp to an object or a 1.5× weapon hit to a creature. giantwall4 (the
  crystal gate) takes damage only from the beam. Harness kstory3_3: blasts leave the gate standing; the beam brings it
  down in ~3 s from three sides and the mission completes; the shard is picked up after smashing the crystal.
- **Fix — push-out launch:** a deep overlap with a structure (spawned or squeezed into one) was resolved in one frame
  and its displacement became the velocity (dx/dt): Kabuto was flung ~200 m in a second near kstory3's gate. The
  velocity after a push-out is now capped at what it was before. Applies to every hero.
- **Not device-tested.**

### 2026-10-24 — Kabuto's throws: boulders and Reapers (this build)
- From the briefings: kstory3_2 "Find the pillar of VOLCANIC ROCK ... smash it! Grab a BOULDER ... throw it at the nearby
  BARRACKS ... the pillar will keep giving you more"; flick flk_kstory2b_1 "Punch/Kick RockTops to create boulders";
  k2a "Try THROWING your enemies at each other". Data: k_rock_gen (the pillar, placed in kstory3_1/3_2/3_4, kstory4_1,
  kstory5_1), k_rock_gen_top (class 78, the boulder; never placed by the missions, so we seat one on each pillar),
  kb_throw.anm and KA_THROW.wav. New PlayerAnim::Throw (mc_throw_stuff for the Meccaryn, kb_throw for Kabuto).
- A blow (fist, stomp, quake or blast) within reach of a pillar's top knocks it off as a loose boulder (at most three
  loose per pillar); X next to a pillar pulls it off; the top grows back in 4 s.
- X now carries a Reaper or a boulder while held. Released after < 0.35 s, a Reaper is eaten as before. Otherwise it is
  thrown: first person with the left hand's smoothed velocity × 1.5 (≤ 90 m/s; a release under 1.2 m/s real hand speed
  drops it), snapped to the lob that lands on a target within ~20° of the throw; third person (kb_throw) a lob at 20°
  onto the nearest building or Reaper within 25° and 12 heights. Gravity 14 m/s² as everything else.
- Thrown boulders raycast each step (weapon_.raycast) and burst: 250 hp to an object hit directly, a 0.3 H explosion of
  300, dust and ten tumbling shrapnel pieces. Thrown Reapers fly in the director's Falling state (knockOff); one that
  meets another Reaper explodes small (150) on it, and every thrown one dies on landing (120 around).
- The carrying hand doesn't punch (its swing killed the Reaper it held).
- Harness: kstory3_2 - a blast at the pillar top drops a boulder, X picks it up, release throws it; one boulder takes
  the army barracks from 600 to 300 hp (77 m throw). kstory4_1 - a held Reaper thrown (third person, and first person
  with a scripted left-hand swing) kills itself and the Reaper it hits; a quick X tap still eats.
- **Not device-tested.** Throw strength and the aim help need the headset.

### 2026-10-24 — On-device automatic tests (this build)
- Goal: let Claude run the game on the Quest without anyone wearing it. The user installed adb (platform-tools 37.0.1
  in Documents\platform-tools; `adb devices` lists the Quest 2, 1WMHHA696A2426). This session has no shell on the PC,
  so the user starts the runner; everything else is automatic.
- `src/game/test_script.{h,cpp}`: a text test script (world, mission, env hooks, start point, view, timed stick /
  button / hand-path input, blasts, screenshots, perf window), in seconds. Unit-tested; the headless harness runs the
  same scripts (`--script`) as a dry run on the PC.
- `src/platform/android/autotest.{h,cpp}`: when `files/autotest.txt` exists the app consumes it, skips the save and the
  menu, starts the mission, replaces controller input and eye poses with the script's (so it doesn't matter where the
  headset lies; XrApp test mode also renders without tracking), and records every frame: interval, update and render
  CPU time, GPU time (GL_EXT_disjoint_timer_query, ring of 6 queries), draw calls; left-eye screenshots (glReadPixels,
  halved, PNG); then writes result.txt (fps, frame-time p50/p95/p99/max, slow frames, CPU/GPU, memory RSS/peak,
  mission state), frames.csv and a done marker, and exits.
- `devtest/devtest.ps1` (PowerShell 5.1): installs the APK, wakes the headset and fakes the proximity sensor
  (com.oculus.vrpowermanager.prox_close; automation_disable at the end), pushes each script, starts the app, waits for
  done / crash / timeout, pulls results and logcat into devtest\results\<stamp>\, writes summary.txt. `-Sweep` adds
  every mission (20 s each). PowerShell couldn't be run here (no pwsh in this environment) - the script is untested.
- First tests: perf_1a_village, perf_1d_tower (12 blasts bring guard_tower2 down at 8 s), perf_1c_flight,
  kabuto_k1a, delphi_r1. All dry-run on the host without errors.
- New test hook GVR_GOD (player can't be hurt; perf runs were being cut short by Rippers). The death log now names
  the real cause (it said "fell" for any death that wasn't drowning or a blast).
- **Not device-tested.** Unknowns for the first run: whether the session reaches FOCUSED with the proximity override
  and nobody wearing it, whether the timer query works on the Quest's driver, and screenshot orientation.

### 2026-10-24 — First automated device run (AD1); GPU experiment set (this build)
- Automated device run 1 (TEST_REPORT): the test mode works on the Quest unattended. Findings: GPU-bound (VrApi App GPU ~12 ms
  of 13.9 ms in the village at full GPU clock), drops to 45-60 fps with Kabuto walking, in flight at height, with
  Rippers at the camera and during the tower collapse. CPU is not the problem.
- Added for the next step: fixed foveated rendering via XR_FB_foveation / _configuration / swapchain_update_state
  (XrApp::setFoveation, test hook GVR_FFR=0..3, `d` for dynamic); GPU experiment hooks GVR_SHADOWS=0,
  GVR_SHADOW_SIZE=n, GVR_REFLECT=0; devtest/experiments: 15 runs (k1a walk x 7 configs, village x 5, flight x 3).
  The runner (-Set experiments) now records the VrApi lines (vrapi.txt) and summarises fps / App GPU ms / GPU load.
- Screenshot frames (read-back stalls) are left out of the statistics (flagged in frames.csv).

### 2026-10-24 — Foveated rendering on by default (this build)
- From automated run 2 (TEST_REPORT): FFR medium takes 1.4-2.2 ms off the GPU and holds 72 fps in the Kabuto walk,
  village and flight scenes that dropped to 62-68 without it. Shadow map size made no difference; the shadow pass's
  cost is the scene draw (a later target: cull shadow casters harder). Reflections are cheap.
- UserSettings.foveation (0-3, default 2, settings.txt `foveation=`), Options row FOVEATED RENDERING; the Android loop
  applies changes through XrApp::setFoveation; GVR_FFR pins a level in test runs (experiment baselines now pin 0).
- Not yet looked at: the 3-4 s of 33-45 fps right after a mission loads.

### 2026-10-24 — Warm-up, shadow LOD, Kabuto first-person view (builds 26301db, this build)
- **Warm-up behind the briefing:** while a mission's briefing shows (0.3-5 s), the first eye of each frame also draws the
  new level from the start point in 16 directions (8 yaws, level and pitched down) and clears it, so the driver's
  first-use work on textures and shaders happens before play (on the Quest the first 3-4 s ran at 33-45 fps, App GPU
  up to 23 ms). Unmeasured on device; `devtest/tests/warmup_k1a.txt` reloads k1a through the briefing (new script
  keyword `briefing`, GameApp::debugReloadWithBriefing) and measures 7-12 s.
- **Shadow PCF LOD:** 5 taps within 8 m of the eye, one hardware-filtered tap beyond.
- **Kabuto's shadow area** grows with him (48 m square × sqrt(scale): ~140 m at full size) - map size cost nothing.
- **Kabuto first person:** his shoulders are at eye height beside the head (bone bounds: r_arm y 11.1-14.2 m vs eye
  13.2 m), so a forward punch put the upper arm across the right eye's view. The eye point is now 2 m (model) ahead of
  the face (~24 cm at his scale); harness renders at 0.2 / 1.2 / 2.0 m compared, arms lower in view at 2.0. Needs a
  headset check (looking down no longer shows his chest).
- **Adaptive quality fix (from the device logs of run 2):** the fps watchdog counted the slow first seconds after a
  load and dropped the effects quality for the rest of the session (logs: "47.4 fps average, effects quality down to
  level 1", "40.5 ... level 1" then "54.0 ... level 2" - reflections and terrain detail off), and never raised it
  again. Now the 6 s after any load or cutscene (8 s at start) don't count, and the quality steps back up after 12 s
  at >= 71 fps. (So the "FFR off" baselines of run 2 ran with reduced effects - the real gap is larger.)

### 2026-10-24 — Cutscene fades from the scripts (this build)
- Surveyed all 80 flk shots: op 0x25 (the decompilation's "fade") is never used; op 0x1F {level, seconds} is the fade
  (opening shots `{0,0}{1,2}`, last shots `{0,2}` at their final wait; FORMATS.md). Parsed into FlickShot::fades; the
  player's blackout follows them (carried across shots; shots without fades keep our 0.25 s dip), a last shot is held
  until its fade-out completes (story1_start_12: 12.0 -> 12.67 s), and the game fades back in from there. Harness
  trace: story1_start_1 fades in over 2.0 s from black; story1_start_12 fades out to black before play.
- Cutscene actor show/hide (ops 0x0B/0x0C): story1_jetpack_6 showed two Timmys at once (standing, and riding on
  Baz's back); now the rider appears and the standing one goes at frame 61 (harness renders at 0.3 s and 1.7 s).
- Looked at a large leafy slab beside the camera in story1_jetpack_6: it is the hatch actor (sm_hatch_l0, its own
  2-bone tree Box01). Probably the open trapdoor lid standing next to the shot camera, outside the original's narrow
  field of view but inside the headset's; left as is (a hypothesis, not confirmed).
- op 0x26 {f32}: 500 in the jetpack shots (close-ups at the hatch), 1000 in jetpack_5, 20000 in the Tel shots, 40000-50000
  in the hatch/save shots - a per-shot distance (clip or fog range?); not used.
- Cutscene actors closer than 1 m to the shot camera are not drawn (logged once): story1_jetpack_6's hatch lid filled a
  third of the view. Only checkable on the harness's final frame; the rule can only hide what the camera is inside of.
- **Finding: our cloud copy of Bin was missing 473 of the 899 files** - 465 cutscene shots (every chapter after Story 1:
  flk_Kstory*, flk_Rstory*, flk_story2-5*) and the French/German/Italian/Spanish text and voice. The user's PC and the
  headset have them, so 23 missions' opening cutscenes have played on the Quest without ever being checked here.
  Staged the first 50 (Kstory1_Delphi_changes, Kstory5_End, kabuto_fight): all parse; kstory1_delphi_changes_1 plays
  in the harness (6 actors, props, subtitles). They use ops not seen in Story 1 (0x2A, 0x2F, 0x31, 0x33, 0x35).
  Next: stage the other 415 and run every cutscene in the harness.

### 2026-10-24 — All 545 cutscene shots checked; cutscene effects; carried weapons in hand (build 97241e5+)
- Staged the remaining 415 flk shots from the PC (the 4 foreign-language text/voice packs left out). All 545 parse with
  our reader (gzptool flick: 545/545 with a director). Python op census in FORMATS.md.
- **op 0x27 = effect on an actor node** {slot, fxName, mode, actor, node}: 208 uses in 72 shots (fx_shotgun_flk,
  fx_stone_explode_*, fx_sm_glow_sapho, fxb_splash, fx_flick_millenium_spear/bazooka ...). Parsed into
  FlickShot::effects and fired at the node's world position during play; the effect definitions themselves aren't
  decoded, so each family gets a stand-in from our particles (dust bursts, flame + smoke, coloured glow, spray, blood;
  unknown "test3" draws nothing). IMPLEMENTED, BUILD VERIFIED, harness-checked on Rstory1_fire_4a.
- **Bug found and fixed:** static (.gbs) props carried by cutscene actors ignored their alignment node (op 3's
  `ref_handle`), so the guards' spears and bazookas in Rstory1_fire_4a floated upside down over their heads. The
  prop's .anm reference nodes are now read at rest and the handle is aligned to the hand (harness render: weapons held
  and fired into the air with flashes at the tips). Story 1 skinned props (Baz's gun) unchanged.
- Every mission's opening cutscene (33 missions) played in the harness for 5 s: all exit 0, no errors. k1b_portal
  names `kstory1b`, which doesn't exist in the data (not on the PC either) - it is skipped as before. The two Rstory3
  race missions need the race world in the harness (`--world Rstory3_race_1`), then play.

### 2026-10-24 — Base building, first version (5c_securebase, Meccaryn)
- User said yes to base building. From the data: xx_mecc_base.gzp holds the Meccaryn base - skinned buildings
  (Wood_pub/shop/gift/gate/wall, Fun_house, stone_* upgrades, s_mecc_* towers/turret/SAM/teleport/gyro pad) with
  `_build` clips that raise each building out of the ground, LODs _L1.._L6, collision meshes `_coll`, and
  Mecc_base_template_1.anm: one node per slot round base_center (command_center, workshop, giftshop, Gate,
  Wall_A1-3/B1-2/C1-3, Firepit, meat_dropoff, sm_dropoff, Repair01/02, teleport). GTextEnglish has the building names
  (basebldg*), descriptions (bldgdescr*: "The Build Pad is where you stand to direct base building", "The Pub is where
  your Smarties get food. Keep it stocked with Vimp Meat"), states (Ready To Build / Building... / Awaiting More
  Smarties / Completed!) and hints ("OUT OF FOOD!!!"). 5c's briefing: gather Smarties and Vimp meat, build a base, the
  Smarties then make the POP-UP BOMB. r4a (Delphi) is the same with souls and the TORNADO.
- Implemented (game_app_base.cpp): the template placed at Borjoyzee's start (5c), every building site at its slot with
  the slot's rotation; meat drop-off store; Build Pad menu of floating signs touched by hand; two crews building at
  once; the build clip posed by progress; dust while building; "COMPLETED!" / "BASE COMPLETE!" lines. Ours: costs,
  times, crews, Pub/Party House slots (Repair01/02), base centre choice, the template's orientation (not rotated;
  in the harness the walls sit on the basin's ridges and the gate in a gap, which suggests it is right - unverified).
- Harness (GVR_BASE_ORDER=all, GVR_BASE_TRACE): sites placed, orders taken, Pub/Workshop progress 0.25-0.28 at 5 s,
  "WORKSHOP - COMPLETED!" shown at 20 s; gate and walls visible on the ridges from above. 5c/4a/r4a run without errors.
- Pitfall: the harness's `--free` camera turns play mode off after the first frame, so nothing in updatePlayMode runs;
  use `--at`/`--look-at` without `--free` to watch gameplay systems.
- Not yet: building collision, base attacks, stone upgrades, the pop-up bomb, Delphi's base (xx_reaper_base.gzp).

### 2026-10-24 — Base building v1.1
- Harness hand test (`--hand-path` to a sign on the pad): the touch orders it. Found that the menu re-laid itself out
  after an order, sliding the next sign under the still-raised hand (it ordered a second building 0.8 s later): every
  kind now keeps its place and shows BUILDING... / COMPLETED!. Signs moved within arm's reach (0.8 m, two rows of four,
  0.55 rad apart) with wrapped text; the old 1.4 m ring needed a step forward.
- Finished buildings are solid: their own `<model>_coll` meshes placed with the site transform (Pub 1007 tris, box
  around the site; Workshop 238 tris).
- Giftshop finished -> the existing shop counter opens at its front (toward the base middle); Workshop finished ->
  building 1.5x faster (ours, after bldgdescr "where your Smarties go for tools"). Harness 50 s with everything
  ordered: Pub, Workshop, shop opened, Giftshop, Party House, "Base: complete", Wood Gate.

### 2026-10-24 — Pop-up bomb
- Data: type 822 mc_popupbomb; MC_PopUpBomb.gbs/.skn with MC_Popupbomb_closed/tick/fire/noanim clips
  (xx_mecc_flick.gzp). Text: Reg's lines in story3_popup_bomb ("destroy the doors to the prison", "extra light ...
  attached to your back", "set off the Pop-up Bomb right by those doors"), story3_prison_7's briefing ("Only the
  POP-UP BOMB can destroy the GATES", "Check the Shop for an extra"), 5c's (made by the Smarties once the base is done).
- Implemented (game_app_popup.cpp): 4th weapon-strip choice; trigger sets it down 1.2 m ahead; tick clip and beeps
  for 5 s, fire clip, then explode(radius 14 m, 2500). Only its blast damages bw_prison in story3_prison_7 and
  guard_army_barracks in 5c (as the Delphi barracks need the tornado). Given: 1 at prison_7's start, +1 in that
  level's Shop, 1 when 5c's base completes. Timings/radius/damage and the VR handling are ours.
- Harness: prison_7 with the bomb set 10 m from the prison -> "Destroyed: bw_prison", objective done, mission
  complete; 40 rocket blasts at the prison -> 0 objects destroyed; prison_7's Shop lists 4 items; 5c base complete
  -> "Pop-up bomb given (1)"; bunker_3 unaffected.

### 2026-10-24 — Delphi's base (r4a)
- Data: RP_base_template_1.anm (21 nodes: command_center, rp_workshop, spellshop, Repair01/02, teleport, Gate,
  Wall_A1/B1/C1, soul_dropoff, sm_dropoff, waiting_1/2) and xx_reaper_base.gzp (RP_command, Extractor_1/2,
  RP_workshop, spellshop_1/2, RP_Fun_house(+_build), RP_generator, magic_wall1/2, magic_gate1/2, Rp_soul_dropoff,
  s_reaper_*; every clip 3.3 s like the Meccaryn build clips). r4a's briefing: gather Smarties and VIMP SOULS, build a
  base, the Smarties then create the TORNADO for the barracks/fleet.
- The base code is now per race (Meccaryn 5c / Reaper r4a tables). Reaper: Persuader (Repair01), Spell School,
  Spellshop, Party House (Repair02) complete it; Shield Generator ("teleport" slot), Reaper Gate, Forcefields extra.
  Centre: r4a has no Borjoyzee, so its marker (id 2, game 2965 30) - in the harness a flat clearing that fits the
  layout. Vimps enabled on Rstory4 (souls = the meat pieces, counted the same). In r4a the tornado now needs the base
  (tornadoOwned); other Reaper battles unchanged.
- Bug found: initBase ran in initCharacters before initMission picks the hero, so a Delphi mission saw the previous
  hero; initBase/initPopup now also run right after setHero.
- Harness r4a with everything ordered: all four core buildings solid and completed, "Base: complete". r4a, 5c,
  rstory2_battle_1, k1a: no errors.

### 2026-10-24 — Base raids
- Data: 5c and r4a carry hundreds of `army_spline` points (paths from the enemy army barracks; not decoded into
  routes yet), the Smarties' base-attack lines (smchbaseattack1-4) and basehint*destroyed. The original raid logic is
  native code. Ours: once a building stands (first raid after 60 s, then every 80 s while guard_army_barracks stands),
  2-4 Reapers spawn 45-60 m from the base on the barracks' side and walk (their patrol walk, kept going) to the
  nearest standing building; within 10 m each wrecks it at 6 hp/s (core buildings 400 hp, others 250), with fire and
  black smoke and a red health sign. At 0 it collapses back to an empty site (collision off, can be re-ordered;
  rebuilding re-enables its collision). Raiders that notice the player fight him as usual.
- Harness: 5c - raid 1 (2 Reapers) marched in, Borjoyzee's lines, PARTY HOUSE destroyed, raid 2 (3 Reapers).
  r4a - first attempt placed 0 raiders: its base stands in a lagoon (ground 0.3 m below sea level), so spawn points
  now accept wading depth; then raid, Yan's four lines, SPELL SCHOOL destroyed. Test hooks: GVR_RAID_SOON, GVR_NO_RAIDS.

### 2026-10-24 — Base upgrades
- Second-level models from the archives with their own build clips (all 3.3 s): stone_shop/Stone_shop.anm,
  stone_gift, Stone_gate/stone_gate_build, Stone_wall/Stone_wall_build; Extractor_2, spellshop_2, magic_gate2,
  magic_wall2. Labels from GTextEnglish (Stone Wall A.., Giftshop 2, Spellshop 2, Forcefield 2, Reaper Gate 2).
- A standing kind's pad sign offers "UPGRADE TO <name>: cost"; all its sites rebuild as the new model (old collision
  off, the new `_coll` mesh on completion), hp x1.6. Giftshop 2 adds a pop-up bomb to the shop counter (its
  description: "Even more weapons"). Costs ours. Test hook GVR_BASE_UPGRADE=1.
- Harness: 5c - Stone Workshop, Giftshop 2, Stone Gate, Stone Walls A/B ordered and completed; r4a - Persuader 2,
  Spellshop 2, Reaper Gate 2 completed, Forcefields 2 ordered (3 sites). No errors.

### 2026-10-24 — Raids follow the data's army paths
- army_spline (type 1162) points grouped by id (op 5) in file order form 18 paths per base mission. 5c: from about
  game (700, 1500), beside the army barracks at (1014, 1626), to the base's surroundings (e.g. id 1 ends at
  (-1411, -1293), id 7 at (-1451, -2283)); ids 10/11 loop by the barracks. r4a: from about (-2900, 600) to the base
  marker's area (2900-3500, -400..1460). So these are the armies' routes to the player's base (V, from positions).
- Raiders now start on a path that ends within 150 m of the base, where ~60 m of it remains (the whole march at a walk
  would take minutes), walk its points, then go for the nearest standing building. Without usable paths: as before.
- Harness 5c (14 of 18 paths end within 150 m of the base; r4a 12 of 18): raiders walk their path, the attack lines
  follow, WOOD WALL C (on the perimeter, where the paths end) destroyed, raid 3 follows. Slower than the direct
  approach (they reach the walls first), as an army coming down its route would.
- Regression sweep (all 56 missions, before this change but after the base/bomb/raid work): 56/56 exit 0, no errors,
  mean update 0.27 ms.

### 2026-10-24 — Base workforce: Smarties from the hatches, working at the sites
- Data: 3 sm_hatch_l0 (class 38) in 5c and in r4a; Smartie clips sm_hatch_exit, sm_run_with_materials,
  sm_hammer_use, sm_saw_use, sm_dig, sm_spell_builds, sm_sit, sm_celebrate; GText "Required: %d", "%d/%d Working",
  "Awaiting More Smarties". Ours (game_app_basecrew.cpp): crew of 3 to start, 3 per hatch released when the hero is
  within 7 m (they play sm_hatch_exit, then walk to the base, 4x faster while > 60 m from the player); each building
  under construction needs 2 workers (replaces the fixed two crews); workers hammer/saw/dig (Reaper: spell) facing the
  building, the rest sit (and celebrate once the base is complete). Test hook GVR_BASE_WORKERS=n.
- Harness 5c: hatch at game (281, -127) opens with 3 Smarties; at the base the crew walks to the Pub, it completes,
  they move on to the next site. No errors.

### 2026-10-24 — Base leader and kidnapping (5c / r4a)
- Briefings: "Protect BORJOYZEE / YAN at all times. If he gets kidnapped and taken back to the enemy base, you lose!".
  The game's text frames him as the base's "flag holder" ("The Command Center protects your build pad and your flag
  holder"; multiplayer "Capture the Smartie"); both templates have a "borjoyzee" node by the command centre.
- Ours: the leader (SmBorjoyzee_L0 / smyan_l0 with sm_stand/squirm/hwalk/wave) stands at that node (5c's own
  Borjoyzee character hidden); one raider per raid is a snatcher - it goes for him, carries him (over its shoulder)
  back along its army path; killed, it drops him and he walks home; back at the raid's starting point, the mission
  fails and restarts from its briefing (beginLoading). Raiders march at 2.4 m/s in the base missions (1.1 elsewhere,
  reset per mission). Stuck raiders (the paths cross ridges our AI can't climb, rocks) get past after 3 s: 10 m on,
  or a step aside if the player is within 30 m.
- Harness 5c: BORJOYZEE kidnapped by the snatcher, carried back along path 8 (indices 10 -> 8 ...), walls destroyed
  by the others. r4a: Yan placed, snatcher on its way. Long run (22000 frames): carried off -> mission failed -> 5c reloaded from
  its briefing.

### 2026-10-24 — Outro cutscenes checked; pop-up bomb on the weapon strip
- Every mission's outro (op 0x19) played in the harness: 20 missions name one, all 20 play with their shots and no
  errors (5c story5_delphi_leaves_1, r4a rstory4_off_to_race included). Outros were already wired to mission
  completion; a stale comment said otherwise.
- Expected skinned-load warnings for unskinned base models (wood_command, Rp_soul_dropoff) silenced by checking for
  the .skn first.
- Pop-up bomb on the HUD: the game's own icon st_mec_pop (w_menus.gzp) in a fifth cell right of the weapon strip with
  the count, highlighted when chosen; also shown as the held-weapon icon. Harness render checked.

### 2026-10-24 — Bases in the two Reaper battles
- rstory2_battle_1 and rstory5_battle_2 have r4a's briefing (gather Smarties and Vimp souls, BUILD A BASE, the
  tornado after it, protect Yan) and data (5 hatches, 18 army paths, army barracks): base building, raids, the
  workforce, Yan and the tornado gating now run there too; Vimps roam Rstory2/Rstory5.
- Base centre without Borjoyzee is now chosen between the mission's marker and the middle of the army paths' ends
  (median, then the mean of ends within 250 m), by open dry level ground within 45 m (36 samples): r4a keeps its
  marker (36/36), rstory5 its marker (33 vs 3), rstory2 moves off its marker (25: a cliff over water) to the paths'
  middle (32). The soul drop-off is pulled in toward the middle when its slot lies on a rock more than 2 m above/below
  (rstory2: 18.6 m -> level). Renders: rstory2 now a plateau with the Reaper buildings; rstory5 a basin.

### 2026-10-24 — Mission select, play-test checklist
- Main menu MISSION row (ours): all 55 missions of the three stories in campaign order, read from each world's
  scenario list without loading the world (AssetWorld::campaignMissions); left/right to choose, trigger (1.5 s hold
  over a checkpoint) starts it like NEW GAME. Harness: menu shows "7. THE WIFE"; selecting loads Story2 /
  2b_2_returnwife. GVR_LIST_MISSIONS logs the list.
- docs/PLAYTEST.md: a checklist for the user's headset session (base missions first). INSTALL.md: stale statements
  fixed (Story 2 objectives, Delphi's first-person body, Sonaks/bosses), mission select documented.

### 2026-10-24 — Chapter transitions; finale last in mission select; lost-race cutscene; building pose cache
- Harness chain test (each chapter's last mission completed at frame 60, GVR_CUTSCENE=none): all 15 chapter ends
  load the next chapter's first mission - 1d->2a, 2c_2->story3_sonak_1, prison_7->4a, 4d->5a, 5c->r1_smartiegrab,
  r1_saveyan->rstory2_battle_1, rstory2_jetski_3->rstory3_pre_race_1, race_3->r4a, r4a->rstory5_race_1,
  queen_4->k1a, k1b->k2a, k2d->kstory3_1, kstory3_4->kstory4_1, kstory4_3->kstory5_1, kstory5_5->end. No errors.
  So the data's chain is Meccaryn -> Reaper -> Kabuto -> the finale; mission select now lists the_end last (55).
- Race missions' op 0x13 cutscene = the rival winning ("..._jetski_R_win", outro "..._D_win" = Delphi): now played
  on a lost race before it runs again (harness: rstory3_1_jetski_r_win plays, 4 actors).
- Base buildings are CPU-skinned: re-posed only while their pose changes (standing buildings were re-skinned for
  every eye every frame).

### 2026-10-24 — Campaign soak test; crew cowers under attack; mission select launcher
- Soak (harness, one process, GVR_GOD, mission completed every 2000 frames): 1a -> ... -> end, all 54 transitions
  through 18 worlds, 0 errors. Resident memory 174 MB at 1b -> 436 MB at the end: flat within a world, +10-30 MB at
  each world switch (models/textures of the new world cached; the cache isn't emptied between worlds; on the host's
  software GL this includes texture memory). Not a problem for normal play (one story at a time), but worth watching
  on the headset in a long session; evicting the model cache on a world switch would need every cached pointer the
  game keeps (weapons, effects) re-fetched.
- Base crew: with a living enemy within 12 m a Smartie drops tools and cowers (sm_cower), and a site with an enemy
  within 15 m stops progressing (smchbaseattack2: "It's hard to work when you're being shot at"). Ours.
- Mission select gives the rocket launcher for any mission after 1b (the campaign would have).
- Raiders at a building now besiege it: Enemy::siege (set by the base layer) makes an unaware Reaper stand, face the
  building and fire its bursts at it (Fire clip, the usual burst cadence, real enemy bolts with sound) instead of
  idling beside it; the stuck-raider rescue skips besiegers. Harness 5c: raiders "siege 1" at the walls, walls A/B
  destroyed, Borjoyzee kidnapped. Damage to buildings is still the proximity rate (6 hp/s), not per bolt.
- Smartie hatches are drawn as the game's skinned Sm_hatch_l0 (the mission's static copy hidden) and play
  sm_hatch_open when the Smarties climb out; hatch Smarties face the base as they come out. Sweep after the siege
  change: 56/56 exit 0, 0 errors.

### 2026-10-24 — Reaper soldiers hold their weapons
- Data: xx_rpman_weapons-era models rpman_spear / rpm_bazooka (/ rpm_snipergun) with Ref_handle, Ref_nozzle
  (anm); cutscenes attach them at the soldier's ref_rhand. Missions place no weapons (native in the original).
- Ours (game_app_rpweapons.cpp): every Reaper soldier holds one at ref_rhand, aligned by Ref_handle (as the
  cutscene prop fix); shots leave from its Ref_nozzle. One in four (by uid) carries the bazooka: instead of bursts it
  fires a slow rocket (22 m/s, smoke trail) every 3.5 s that bursts on impact or within 1.6 m of the player
  (radius 3.5 m, 55). Weapons cast shadows. Harness: a 2b Reaper holds the spear gun across his body.

### 2026-10-24 — 4a needs the pop-up bomb too; item readings from briefings
- 4a's briefing: "Destroy the TORTURE SQUARE GATE with a POP-UP BOMB. Only the POP-UP BOMB can destroy this gate.
  Need a POP-UP BOMB? You can get one in a Shop." Its target is story4_door (class 101, trig 2): now bomb-only, and
  4a's Shops offer the bomb. Harness: 40 rocket blasts at the door -> intact; bomb -> "Destroyed: story4_door",
  mission complete.
- Briefings name the weapons behind more item numbers: story3_bunker_3 (items 1 3 6) "Your new SNIPER GUN ... zoom
  in"; 4a (1 7) "Try out your MILLENIUM GUN"; so 6 = sniper gun, 7 = Millenium gun (not implemented yet).
  The pop-up bomb is now granted from item 10 (prison_7's list) rather than by mission name.

### 2026-10-24 — weapondefs.bin decoded; the Meccaryn arsenal from the data
- Data: `Bin/weapondefs.bin` (loose) is a key/type/value table: 53 weapon records then one record of 73 inventory
  items (format in docs/formats/FORMATS.md; parser src/assets/weapondefs.cpp, unit-tested on synthetic data;
  `gzptool weapons <Bin>` dumps it). Its item ids are the missions' op 0x0E item ids, and its key 39 names the held
  object (GameObj types 21 mc_gun_l0, 22 mc_shotgun_l0, 300 mc_machinegun_l0, 787 mc_snipergun_l0, 790
  mc_mortar_l0, 785 mc_launcher_l0...). Cross-checks: 6 = IconSnipergun in story3_bunker_3 (its briefing), 10 =
  IconPopup in prison_7, 20 = IconBush in 2b; 1c-2c list item 3 and are exactly the missions that place
  mc_pickup_shells; GText "iconmortar = Millenium Mortar" = item 7 (4a: "Try out your MILLENIUM GUN").
- **Two earlier readings were wrong:** item 1 is the handgun (not the jetpack) and item 3 the shotgun (not the
  rocket launcher; the launcher is item 14, first listed in story3_launcher_5). Consequences, following the data:
  1c onward hands out the shotgun; the rocket launcher now comes from story3_launcher_5 (or a Shop) instead of 1c.
  Further confirmation: Timmy's present at the end of 1b is the cutscene flk_story1_get_rpg1-10 ("Here, take this
  present. It's a weapon that other spaceman dropped!") - despite the file name, its actor nodes are `shotgun`,
  `shotgunpump` and `Ref_shotgun`: he hands Baz the shotgun.
  Story 1's jetpack gating on item 1 is kept as our rule (comment updated).
- Placed mc_pickup_shells were being turned into health pickups (all class 37 was read as health): they are now
  shotgun ammo pickups (2 shells each, the ammo item's key 46). 1c_1: 3 ammo pickups, 0 health (was 3 health).
- Ours (game_app_arsenal.cpp): shotgun (sel 4), machine gun (5), sniper gun (6), Millenium mortar (7), each held at
  its .anm Ref_handle and firing from Ref_nozzle01, with damage scaled against the handgun's record (fx_gun every
  0.2 s, 12): shotgun 3.3x (one shell per pull every 1.3 s, a direct hit plus a 1.8 m burst), machine gun 0.67x every
  0.1 s while held (300 m/s), sniper 6.7x every 1.5 s at 500 m/s (one-shot kills a Reaper), mortar a lobbed bomb
  (mc_lbomb) that bounces once and bursts (5.5 m, 225: the record's 150 vs the rocket's 80). Ammo: start/max from the
  item (shotgun 25/35, machine gun 800/2500, sniper 8/12, mortar 6/10); empty = out-of-ammo click (ME_OUTAM).
  Sounds by name (guessed): ME_RSGSH/RSGEX shotgun, ME_MCHST/MCHLP/MCHEN machine gun, ME_SPRSH sniper, ME_MTRSH/MTRHT
  mortar, PU_AMMO pickup. HUD: st_mec_shotgun / machine / sniper / mortar icons with rounds left; the weapon strip
  now lists what is carried (first four cells on the st_mec_wpslot art). Saved per checkpoint (gunN=<ammo>).
  Mission select grants every item listed before the chosen mission.
- Grenades (item 8, 5a and kstory5_5; ours for VR, game_app_grenade.cpp): picked on the strip (sel 8), one sits in
  the hand (MC_grenade_L0); hold the trigger, swing, let go: it leaves with the fastest hand velocity of the last ten
  frames x1.6 (capped 30 m/s); a slow release lobs it ahead at 12 m/s (third person always). It bounces (0.4) and
  bursts after a 2.5 s fuse or on a character (3.6 m, 130; weapon 2: 80 / (75, 0, 30, 50)). 5 given, 8 carried (item).
  Harness 5a: a hand swing of 0.85 m in 10 frames throws at 9.8 m/s; a lob burst 0.6 m from a Reaper 12 m away kills it.
- Sniper zoom: MC_snipergun_L0's third part (8 triangles, game x 1.22..2.41, y -2.47..-2.26, z 1.43..2.32) is a flat
  panel on a stalk beside the barrel facing the shooter, next to the .anm's "screen" node - the gun's zoom display.
  Ours: while the gun is within 45 cm of the eye (first person) the world is rendered from the muzzle along the barrel
  (level, ~5 degrees across, about 4x) into a 256x256 target (Renderer::initScope / beginScopePass) and drawn on that
  panel, with a red cross. Cost: one extra pass (57 draw calls in story3_3sonaks_4), only while aiming; none at
  quality level 2. Harness render: the panel shows the magnified horizon the right way up.
- Shops in Meccaryn levels also offer AMMO (the shells box, once per level, ours): every carried gun and the grenades
  filled to their most. Missions place no other ammo than Story 1-2's shells, so without it the sniper's 8 rounds
  would be all there is.
- Not done: ammo for the rocket launcher (still unlimited), Reaper guards dropping ammo, mines (item 9).
- Harness (story3_bunker_3, a Reaper spawned ahead, auto-aim): sniper at 40 m - 1 hit, dead; shotgun at 15 m - one
  direct shell, dead; machine gun at 15 m - dead after 8 hits; handgun - 5 hits. 1c: "Picked up 2 rounds for the
  shotgun (27)". The three new guns render in hand in first person (screenshots checked).

### 2026-10-24 — Delphi's bows (weapondefs items 44-46)
- Data: items 44 IconRBBow (rp_bow), 45 IconRBRPG (rp_bow_rpg; ammo item 55, 35 max / 25 given), 46 IconRBSniper
  (rp_bow_sniper; ammo 57, 15 / 8). r1_smartiegrab lists 45, r4a 44 45 46. Weapon records by effect: fxb_bow_rpg 40
  every 1.3 s with a small blast, fxb_bow_sniper 80 every 1.5 s at 2500. The plain bow has no effect to match: ours
  30 every 1.0 s. Models: riser at the origin, limbs curving back toward the archer (+Y game) to tips at (0, 2.1,
  +-3.2) (offline side plot of the triangles); the string is not modelled. Sounds by name: SR_RBW*/RPG*/SBW* (PL pull,
  SH shot), SR_STORB/STORP/STOSB sword -> bow, SR_BTOSW back. rparrow.gbs is a streak (b_streak2) used for RPG arrows.
- Ours (game_app_bow.cpp): right stick forward cycles sword -> bows -> sword (the sword is put away). First person:
  the bow in the turn hand along the controller's aim, ~1 m tall; the move hand's grip within 25 cm of the string
  starts a draw, pulling back up to 60 cm sets the power (string drawn to the hand, an arrow shaft shown, haptic
  ticks), letting go shoots from the hand through the riser; that grip doesn't turbo meanwhile. The bow hand's
  trigger also draws (0.7 s to full; third person always). Power 35-100 % of the record's damage, speed 25-75 m/s
  (sniper up to 200). RPG arrows are rockets of kind 7 (a direct hit plus a 1.8 m burst, like the shotgun shell).
  The HUD shows arrows left under the sword icon; Shops' AMMO box refills them (and appears in Delphi's levels).
  Saved per checkpoint (bowN=<arrows>).
- Fix found on the way: the bolts of weapon_ only moved in updateWeapon (Meccaryn only); Delphi's update now advances
  them too.
- Harness (r4a, a Reaper spawned ahead, auto-aim, trigger draw): bow - 2 arrows, 2 hits, dead; RPG bow at 25 m - one
  arrow, dead; sniper bow at 25 m - one hit, dead. Renders: third person Delphi holds the bow upright in her right
  hand; first person the bow in the right hand.
- Limits from the item table where it gives them: health injections at most 3 carried (item 0; was 5, ours) and one
  handed out by item 0 (5a, kstory5_5); flares at most 5 (item 64; was 6). Heal amount and drop rate stay ours.
- Shots look like the original's where the data names their projectile: the sniper's round is mc_sniper_bullit (its
  record's projectile type 793) with the EFX_sniper_flash muzzle flash; the shotgun shell is MC_shotgun_shell (type
  1114) nose first with a glow; machine-gun tracers are smaller and orange; arrows are the rparrow streak, with no
  flash off the bow. (WeaponSystem::shotStyle / Projectile::style.)

### 2026-10-24 — fx.bin's structure decoded (effects not yet driven by it)
- fx.bin (and its copy fx_world_dust.bin) is the effect library: 679 named effects, each a byte-coded op stream.
  The argument lengths of all 91 ops were found from the data alone (no reference available: the decompilation repo
  is no longer reachable): starting from the small effects' obvious layouts (`03 u32 06 1b f32 07 02`), each op's
  length was fixed so the most effects parse; at the end all 679 parse to exactly the next one's start, which any
  wrong length would break. Parser `src/assets/fx_defs.cpp` (unit test incl. a desync), `gzptool fx`.
- What it shows: effects are lists of particle emitters of efx_* GameObj types with parameter blocks, named
  sub-effects spawned at points of reference shapes (efx_ref_sphere), sounds by id, and projectile/trail pairs
  (FX_rocket: mc_rocket + efx_ref_line_back). Meanings are guesses (FORMATS.md); nothing renders from it yet.
- Sound ids decoded: section 2 of every 0x1A0002E5 .bin is a sound table {id, flags, 6 floats, name offset} (format
  in FORMATS.md; ObjectRegistry::addSoundTable, unit-tested). The first framing tried (name offset first) gave
  shifted names and 111 conflicts between files; the record really starts with the id and ends with the name
  offset - then 660 ids with 5 conflicts, and fx.bin's ids land on fitting sounds (rocket explosion me_mslex,
  bullet hits bi_hit1/2, the Millenium bomb's bounce me_mtrht - which is the sound the mortar code already used).
  The table names ids only, but chained with fx.bin it ties some guns to sounds: the machine gun's nozzle effect
  (FX_nozzle_machine, op 0x81) names 1531 me_mchst / 1530 me_mchlp / 1529 me_mchen - start, loop, end, as the code
  plays them - and the shotgun's impact effect (weapon 6 -> fx_explosion_shotgun) plays 1496 me_rsgex, the burst
  sound used. The sniper's and shotgun's firing sounds (me_sprsh, me_rsgsh) are still read from the names.

### 2026-10-24 — Review fixes; the original effects as an Options test
- A read-only review (subagent) of the day's weapon code found no crash paths but state leaks; fixed:
  - The machine-gun loop, the sniper's zoom pass and red cross, and Delphi's drawn bow are now dropped on any frame no
    weapon update ran (death, riding, menus, cutscenes, loading, another hero: `dropHeldWeaponState`, checked once per
    frame before rendering); the scope pass and the bow also check the hero, play mode and riding themselves.
  - Grenade, shotgun-shell and RPG-arrow bursts hurt and throw the player again (`explode`'s fire=false meant "a quake
    that spares its maker"; a separate hurtsPlayer flag now).
  - The pistol on the thigh can be drawn back with only a long gun (no launcher) out.
  - Holding a grenade / the bomb / the bush no longer freezes bolts in flight.
  - Each gun reloads on its own timer (a mortar or sniper shot no longer blocks the next gun after a switch).
  - Placed pickups: an mc_pickup_* that feeds no weapon stays a health pickup as before; the shells pickup (type 647)
    feeds whichever carried weapon its type serves when taken - the shotgun for Baz, the RPG bow for Delphi (item
    55 names the same pickup).
- fx.bin playback (game/fx_player.*, game_app_datafx.cpp): emitters of efx_* models with sizes, fades, gravity,
  bounce, spin, and child effects riding a reference shape's animated Ref_debri<n> points; sounds by id. Op meanings
  are guesses, so it is OFF by default: Options -> EFFECTS: ORIGINAL (TEST) uses it for cutscene effects (op 0x27 names
  fx.bin entries), rocket / grenade / Millenium bomb / shotgun / pop-up explosions. Harness: the rocket explosion
  shows a very large flash (fx 08: 10 -> 200 units, i.e. 40 m, for 0.1 s) and debris riding the sphere's points;
  whether that matches the original is for a headset look. Projectile ops (0x24) are not played yet.

### 2026-10-24 — Launcher rockets, mines, Delphi's homing and power-up bows
- Rocket launcher ammo from weapondefs: item 14 IconMissile carries 12 and comes with 8; its ammo item 40 gives 3
  per mc_pickup_rockets (type 801; no mission places one, so Shops' AMMO box is the refill). Empty = click.
  Checkpoints save the rockets; a save from before (launcher=1, no count) restores 8.
- Mines (item 9 IconMine, mc_mine, 5 carried / 5 at a time; weapon 13's blast 55 over (.., 30, 50)). No mission
  hands them out: every Meccaryn Shop (and the base's Giftshop) sells MINES (ours, after its "full of weapons").
  weaponSel_ 9: the trigger sets one down 1.3 m ahead; armed after 2 s (me_minlk), a red light every 0.5 s with a
  soft me_minml tick (fx.bin FXB_Flasher: FXB_redlight every 0.5, those sounds); a Reaper, Ripper or Vimp within
  2 m sets it off (3.6 m, 160 - ours, so a foe on it dies; FX_EXPLOSION_MINE / me_minex). Harness: a Reaper 3 m
  ahead of the hero, mine set 1.3 m ahead -> "Mine went off", Reaper dead.
- Delphi's homing bow (item 47, rp_bow_homing; 10 / 6 arrows; record fx_homing 50 every 1.5 s) and power-up bow (48,
  rp_bow_powerup; 10 / 3; record fxb_bow_screamer 50 every 1.5 s, key 7 = 8 read as an 8-arrow fan - ours fires 5
  at 0.6x each). No mission hands them out: the Spellshop sells them - Spellshop 1 the homing bow (with AMMO and
  an injection), Spellshop 2 adds the power-up bow (ours, after "spells and weapons made just for you" / "more
  spells and weapons"). Homing arrows (rocket kind 8) turn toward the nearest living foe within 60 degrees and
  80 m and strike it within 1 m (direct hit + the RPG arrow's small burst). Sounds SR_HBW* / SR_PBW*, switching
  SR_STOHB / SR_STOPB. Harness r4a: homing arrows shot 12 degrees off a Reaper 20 m away lock on and kill it; the
  power-up fan kills one 12 m ahead.
- Not done: Delphi's mine (item 63 rp_mine) - she has no weapon slot for it yet.

### 2026-10-24 — APK d295fa1 (user: "Build APK, and do the tests without headset")
- Built at d295fa1 (first Android build since 89f3fd7): clean, no warnings. Delivered and checksum-verified on the PC.
- Headset-free tests: unit (host + sanitizers), a whole-campaign soak in one process, four mission sweeps incl. one
  under AddressSanitizer/UBSan - all clean (TEST_REPORT.md). Three new devtest scripts cover the new weapons, the bows
  and the original effects' frame cost on the Quest; they run unattended over USB with devtest.ps1.
- Fix found while writing them: the weapon-switch log named grenades and mines "rocket launcher" (log only).

### 2026-10-24 evening — Branch `next`: run summary, projectile trails, Delphi's mine, review fixes
Work goes on branch `next` (from a3d2d52) so tomorrow's device results match the d295fa1 APK.
- **Device-run summary** (`tools/py/review_results.py`, extended): per test fps, frame-time p50/p95/p99/max, update and
  render CPU, GPU time, draws, memory and the VrApi line; the 3 worst frames with what they waited on (GPU over 13.9 ms,
  CPU over it, or neither = a hitch); the sniper zoom cost (weapons_mecc: 3-10.5 s sniper vs 14.5-18.5 s mortar) and the
  original effects' cost (fx_original: 1.5 s after each explosion vs between); the change against the newest earlier
  run with the same tests. Statistics are recomputed from frames.csv: the 20261024-121735 run's result.txt GPU means
  are timer garbage (~1.8e13 ms, from before the 200 ms plausibility filter) - flagged, not used. Checked on both real
  runs from the PC and on a synthetic later run (comparison, a crashed test, the two cost tables).
- **fx.bin op 24** (111 of 679 effects): `{object type, emitter type, k}` - an emitter riding a game object (k = 0) or a
  self-contained one (k = -1). FxPlayer::attach / moveAttached / detach: the caller moves the emitter; n = 0 children
  ride it and live while it flies; 20/22/25 draw a streak behind it (colours, width in game units, seconds - our
  reading of the values; 26 used as its opacity, floored at 0.25; unconfirmed); 5b/27 trails follow it. One not
  moved for 0.5 s ends by itself (a cleared projectile can't leak it). Projectiles carry their original trail with
  the Options test switch on: FX_rocket (launcher, Reaper bazooka), FX_homing (SAM), FX_shotgun_trail, FX_Lbomb,
  FXB_bow_RPG, FXB_bow_screamer; the fire bolt and grenade keep ours. Unit test on a synthetic library (attach, ride,
  streak, detach, auto-end). Harness: the launcher rocket shows a blue flame (efx_bfire_trail) and a pale streak;
  every listed kind logs its trail in the targeted runs.
- **Delphi's mine** (item 63 IconMine, race 3, object 1222 rp_mine, ammo item 9 like the Meccaryn's; weapon 44 the
  same 55 splash, blast fx FXB_Rp_mine): the last slot of her right-stick cycle (sword -> bows -> mines), the trigger
  sets one down 1.3 m ahead as his; her Spellshop sells mines. Harness r4a: set, armed, a Reaper next to it sets it off.
- **Review of base building and Kabuto** (a separate read-only agent): fixed
  1. held / thrown Reapers and the eat target were list indices kept across frames while dead enemies, creatures and
     Vimps are removed from the lists (an index could land on another Reaper and kill it, or complete an objective):
     now uids (Vimps and villagers gained one);
  2. fist swings were measured on the world-space hand, so a respawn teleport, a snap turn at giant scale or a fall
     counted as a punch (up to 7x): now the tracking-space hand (the user's real swing), drawn where the rig is now;
     Kabuto's state is also reset each mission (setHero skips Kabuto -> Kabuto);
  3. Offspring and eggs stayed drawn after another hero took over: Kabuto's powers and throws are reset for every hero;
  4. a raided and rebuilt upgraded Giftshop / Spellshop added its shop item again (and re-opened the counter): rewards
     are given once per kind and level ("REBUILT" otherwise);
  5. the 9 hatch Smarties were loaded (3 skinned models x 10 clips) in the frame a hatch opened: now loaded with the
     mission;
  6. pulled pillar tops are capped at 12 loose boulders.
  Not changed (low): the fire pit re-posed per eye, the build menu showing during death, the base set up twice per load.
- Harness checks: snap turn with the hands still -> no fist blow; a 0.9 m swing in 3 frames -> one blow (21.6 m/s);
  kstory5_4 with eggs laid, completed -> kstory5_5 loads as the Meccaryn.
- Housekeeping: `stereo.png` (the harness's default output, the test scene) was tracked by accident since d9e445b;
  untracked and ignored.
- Build lesson: stopping a ninja build while editing a header it is compiling against can leave object files built
  from the old header but newer than it (ninja then calls them up to date): the ASan harness crashed in `initShop`
  writing through a vector whose pointer held 1.0f (another member's float). `touch` the header and rebuild after
  interrupting a build mid-edit.
- Sanitizers: the 46-run targeted set (Kabuto x 32, mission chain, races, Reaperski, trails, Delphi's mine) is clean
  under ASan + UBSan.

### 2026-10-25 — Multiplayer: skirmish against computer players, and LAN play (branch `next`)
User: "Do this: Multiplayer, from the original game"; chose "Bots first, then online"; no second Quest for now.
- **What the data holds** (FORMATS.md "Multiplayer data"): 12 multiplayer worlds w_M_* without mission files; each
  places its sides' base templates (mecc_base_template_1 / rp_base_template_1) and, on the 3-way maps, Kabuto's eat
  pad; markers round each base; Smartie hatches. GTextEnglish's multiplayer lines describe team games with kills,
  flag captures (the flag a Smartie, "flag holder"; Kabuto eats it on his pad), score limits, base options and TCP
  joining. Flag holder = smborjoyzee_l0 with Smborjoyzee_flag / _fly / _fly_rp; flags sm_flag1-3.
- **Skirmish** (game/skirmish.*, GL-free, unit-tested): teams from the map (Blue/Mecc, Green/Reaper, Red/Kabuto),
  spawns = markers within 160 m of a base; flags at the bases; take / drop (carrier killed) / return (own side
  touches, or 30 s) / capture (home with an enemy flag while your own is home; Kabuto's side has no flag); win at 3
  captures or 30 kills, then a new round. Rules are ours.
- **Computer players:** each drives the player's own PlayerController (terrain, collision, jetpack). Roles attack /
  defend alternate; they shoot (gun bolts 70 m/s, arrows 45 m/s with drop, leading the target; Kabuto stomps),
  carry flags home, chase thieves, return dropped flags. Routes: SkNav, A* over a terrain grid (~4 m cells over the
  map) with per-hero rules (Meccaryn jets up banks, Sea Reaper swims, Kabuto wades), the steepest half-metre of
  each step measured (banks between samples had trapped them), turn waypoints kept (thinned routes cut across the
  banks they avoided); bounded searches (4000 cells, weighted, partial routes continue), one per update.
  Debugging found: the base templates are solid world objects (a red gizmo pillar) - hidden and made non-solid;
  jumping while wading stalled them; the test waves of Reapers spawned in mission-less worlds (off in skirmish).
- **Game side** (game_app_skirmish.cpp): bodies with their hero's model and clips, gun / bow in hand, team-coloured
  name tags, shots, flag holders (carried on the back); the player's guns, blasts, sword, arrows and Kabuto's blows
  hit them; their shots hurt the player; death respawns at the team's base; score panel; the original's lines
  (mpchkilled1, mpchflagtaken1, mpchflagcap1...). Main menu MULTIPLAYER rows. Mission-less worlds are now set up on
  load (loadMission with an empty name).
- **Measured (harness, 200 s, all 12 maps):** everyone moves; kills on every map; flags taken on most, captures on
  3-way L1 (the Kabuto bot captured twice). Skirmish update 0.2-0.4 ms average; routes 0.8 ms average, 5 ms worst
  (PC); posing 7 bots 0.34 ms.
- **LAN play** (game/sk_net.*, net/udp.*): host-authoritative; beacons + probes find games; join/welcome; joiner
  state 30 Hz with its hits (resent until applied once) and shots; host snapshots 20 Hz (players, flags, scores,
  shots, events resent until acknowledged). Joiners mirror the host's game. Verified: unit test over loopback; two
  harnesses paced in real time (GVR_REALTIME=1): joined, positions agree within 1 m, the host's bot killed the
  joiner and both logged it; joined through the main menu (found by probe, map loaded, team taken); a joiner that
  quits times out and leaves cleanly. Android: INTERNET / Wi-Fi permissions, multicast lock via JNI; the APK
  compiles. **Not tested between two headsets** (one Quest).
- **Bases in multiplayer** (the original's "starting base level"; ours: built from the start): each Meccaryn or Reaper
  side's buildings from its race's template (Mecc_base_template_1.anm / RP_base_template_1.anm, the same layout as
  5c and r4a), turned by the map's template angle, posed at the end of their build clips, solid, routed round. No
  walls or gates (a closed ring would shut the side's own flag in; the original opens its gates for its side - not
  done). The flag holder stands at the build pad ("The Command Center protects your build pad and your flag
  holder"); captures are scored there. 5 minutes on six maps: 2-10 kills each, flags taken on four, no captures
  (carriers die on the long way home), 0 errors.

## 2026-10-25 (night) — fixes on branch `next`: bot captures, a multiplayer review, Reapers' ammo
No new device results on the PC since 2026-10-24's runs, so the known issues were worked on (host only).
- **Bots never captured** (12 maps x 10 minutes, before: 13 flags taken, 0 captures). Causes found with a per-bot
  trace (GVR_SK_DEBUG=<id>) and the flag state in the 15 s trace:
  - On every Reaper side the flag stood inside the command centre (RP_command: 42 m across, its floor ~8 m up;
    its collision profile measured along the pad). The bots walked into it for the whole match. Now the flag stands
    at the nearest spot by the pad that is open sky (no floor overhead), where a capsule stands free and can walk
    out (a clear line outwards), searched first towards the base's middle: 18 m in front on all 8 Reaper bases;
    Meccaryn pads (open, 3 m) unchanged. Ours.
  - The harness's first map set up the mission before the player had ground / sea (a menu start on the device
    already had them): bots had no sea and walked on the -20 m floor off the island. Moved before initMission.
  - The route grid measured steps from the sea floor: a swimmer could never climb out. Now surface to surface.
  - Buildings blocked their axis-aligned bounds (a turned base blocked its own flag); now their turned footprint;
    a goal in a blocked cell is still enterable; no diagonal steps past a building's corner (review finding).
  - Rocks and ledges the terrain grid doesn't know: a bot stuck going into a cell marks it for its kind of mover
    (costlier, impassable after 3), and getting no nearer a waypoint for 5 s counts as stuck (swimming along a
    bank moved enough not to count before).
  - Flags no route reaches (checked once per side at the start, unbounded search, 1-27 ms) are left alone; a bot
    with no route anywhere hunts for 10 s.
  - Behaviour: attackers no longer all run home when their flag is taken (defenders do, and attackers within
    150 m); attackers escort a team-mate carrying a flag; carriers and travellers keep sprinting / jet-hopping
    under fire; a partial route's last 40 m are walked straight.
  - **After:** 11-19 captures and 64-70 flags taken per 12 x 10 minutes over runs; rounds won on 3-way L1 and
    Reaper L1. Still slow: Mecc L1-L3 (600 m between bases, about one capture in 10 minutes), Reaper Lw1 and 3-way
    Lw1 (bases behind banks; a flag carried by minute 10).
- **Review of the multiplayer code** (an independent agent, read-only; 13 findings, each checked against the code
  before fixing): players who left came back at the next round as frozen killable figures and their slots were
  never reused (after 3 rejoins a full game refused everyone) -> a `gone` flag, slots reused, hidden; late UDP
  packets could kill a joiner twice or restart its round -> sequence numbers on states and snapshots (protocol 2);
  events beyond 12 were lost -> oldest first; snapshots could exceed 2 KB -> kept to one 1400-byte frame; stale
  attackers credited falls -> only damage in the last 10 s counts; bolts froze through the win screen; the host drew
  its own shots twice; joiners gave everyone a 1.8 m body (Kabuto hit only at the ankles) -> their hero's size;
  a joiner's player stood at its spawn while it loaded and arrived to a pile of stored hits -> out of the game
  until it reports; a lost joiner stayed frozen -> knocks again every 4 s (rejoined a restarted host on loopback);
  host input limits (no negative damage, 4 shots a state, 96 in flight), joiner index checks, only known maps;
  joiners listen on the game port (hear beacons). Not done: manual IP entry; models still load synchronously
  when someone joins (a hitch on the host).
- **Reapers drop ammo** (ours: after the 35 % injection, 30 % ammo): a pickup for a weapon carried that isn't
  full, with the original's pickup object and model (weapondefs ammo item -> pickup type -> GameObj model:
  mc_pickup_rockets / _shells / _clips; sniper rounds are type 787, whose model is the sniper gun itself - as the
  data says). Placed ammo pickups show their own model now (all were the shells box).


## 2026-10-25 afternoon - the rework (branch `rework`)
- User play-test verdict: boring graphics and gameplay, not VR-ish, ugly; cutscene actors misplaced; Kabuto's
  story starts wrong, can't be completed, he's too slow. Asked for a complete rework on a separate branch,
  working nonstop, with progress videos. Classic kept: tag `classic-final`, bundle on the PC.
- Root causes found: (1) every Kabuto and Delphi mission started at the terrain's default point - the start
  marker id goes with the race (Mecc 1, Delphi 2, Kabuto 3) and only id 1 was read; k1a's default point is an
  empty canyon, so Kabuto could never eat enough Smarties to break the gate. (2) Kabuto's cutscenes: the world
  scale changed (8x -> 1x) after the shot camera was placed, dropping the eyes 5 m into the ground.
- Rework so far (all harness-checked, not device-tested): 4x MSAA on the multiview swapchain; new lighting
  (texture relief, sun highlights, haze glow, highlight shoulder, terrain detail octaves; GVR_LOOK=0 = classic);
  no debug controllers in play; gun recoil; two-handed aim; body holsters; punches; reach-to-take pickups;
  comfort vignette; Reapers press in, lob grenades at hidden players; head-height hit box (cover works); hit
  flashes; light flashes; objective marker; ambushes; aim laser; Kabuto faster.
- Mesa llvmpipe (harness renderer) miscompiles exp2/pow in the new specular block (whole frame wrong);
  softpipe renders it right. Specular uses repeated squaring.
- Harness sweeps: 56/56 missions clean after each batch. scripts/make_video.sh renders progress videos.
- Later the same day: grass tufts (green worlds only, lit like the ground, swaying); Kabuto's first-person
  hand grab (reach down, squeeze: a Reaper or boulder stays in that hand to throw or bring to the mouth;
  Smarties, Vimps and small creatures are eaten); Delphi's sword deflects Reaper bolts (a swing returns them
  at the shooter); impact sparks and a soft glow replace the blocky 32 px flash sprite (rework look only).
- Bug found by the parry test: Reaper grenades left from inside the thrower's own capsule and burst on him at
  once (Delphi, r1_saveyan). They now start over his head, toward the target.
- Cutscene review: every mission's opening cutscene rendered to a contact sheet (30 missions). Two fade
  cases blacked out whole shots: an opening shot whose only fade is out at its end (rstory3_channel_islands_1,
  10 s black) and a shot starting black after its predecessor faded out whose script only fades out again
  (story3_lift_2). Both fixed in CutscenePlayer::loadShot. Steep shot cameras (more than 28 degrees down on or up
  at their target) are swung round the target to 28 degrees, since the VR view never pitches by itself.
- Kabuto's story completability (harness, scripted, full size): every "destroy" objective completes with its
  own mechanic - giantwall1/2 by punches (k1a, k1b, kstory3_1, kstory3_2), giantwall3 with adrenaline (k2a,
  kstory4_2, kstory5_1/2/3), giantwall4 with the magnifying glass (kstory3_3, kstory4_1); kstory5_4's boss (the
  other Kabuto, 2500 hp) falls to punches in 17 hits. Not checked: walking to the "reach" objectives (portals in
  k1b, k2d, kstory3_4, kstory4_3) along the terrain; growth from eating to the sizes the gates need in play.
- Kabuto's portals ("reach" objectives in k1b, k2d, kstory3_4, kstory4_3): a flood fill over the terrain
  (src/game/game_app_reach.cpp, GVR_REACH_CHECK) mirroring the controller - 50-degree uphill limit, a giant's
  stride window, jumps only onto ground he can stand on, swimming everywhere (off the terrain is sea with a
  -20 m floor, as for the player), scenery a body would be pushed out of. kstory3_4's portal stands on an
  island ringed by cliffs: unreachable below a 68-degree limit. Decision: Kabuto climbs up to 68 degrees
  (maxSlope 2.5), judged over 0.35 x his height (slopeWindow) - a giant scrambles where a man can't.
  GVR_REACH_WALK then walks the path in play with the real controller (stuck: jump; still stuck: mark the
  spot blocked, plan again). Walked to the portal and completed the objective: k1b (at the old limit),
  k2d_offspring, kstory4_3. kstory3_4: on the island, out of frames (30 000) before the portal. The walker
  is crude (many replans on rough ground), so "not arrived" is weak evidence; "arrived" is strong.
- Found while comparing the beach in rstory2_battle_1 with GVR_UPSCALE=0: the rework's texture doubling
  (upscaleSharpen) used Catmull-Rom weights one tap off for every other row and column, so magnified
  textures (the ground above all) came out in jagged blocks - in every rework APK up to 68003c5, and very
  likely part of "ugly". Fixed in 7400b54 with a unit test that fails on the old code. Also: shots shove
  Reapers back, near misses make them flinch, a duplicate low-health heartbeat (the rework had added a
  second one) removed, and the cutscene camera swing limited to shots within 60 m of their target
  (intro_island_1's flyover had been swung down to the sea). The 17 cutscenes not seen at mission starts
  were reviewed on contact sheets (GVR_CUTSCENE=<first shot>).
- 54cddc8 delivered (with progress video 4): Kabuto leaps when the user jumps (tracked head rising > 1.3 m/s
  from standing; PlayerInput::jumpBoost), badly hurt Reapers fall back, touch haptics for hands in scenery,
  a lighter comfort vignette while a cutscene camera travels (updateVignette now also runs in cutscenes),
  menu panels restyled. kstory3_4's portal: a 109 m plateau whose surrounding faces exceed even Kabuto's new
  68-degree limit in places - recorded as BLOCKERS B18 for the play-test.
- 9e93e37 delivered (with a short climbing video): climbing for Kabuto and Delphi in first person - a grip on
  ground steeper than walkable holds, the body moves opposite to the holding hand's tracked motion (Kabuto
  x1.5), PlayerController::climbing suspends gravity and walking. Harness: one hand, 5 pulls up a 61-degree
  bank: 1.6 -> 14.6 m; two alternating hands: up and onto the top of the bank at 22 m. Meant also as the way
  up kstory3_4's plateau (B18, unverified there).
- After 9e93e37: grass patch shrinks at the first adaptive-quality step; Delphi's sword trail; marksman
  Reapers (1 in 4: 26 m stand-off, single aimed shots at a third of the spread, a glow for 0.7 s before
  each); binaural sound in the mixer (interaural delay up to 0.65 ms, head-shadow low-pass on the far ear,
  duller from behind and with distance); Kabuto's physical ground pound (a quick crouch).
- a8bc86e delivered (with video 6). Two bugs found making the video: Kabuto's ground pound put a fireball at his
  own feet (every damaging explosion had been given one; now fire, grenades and rockets only, and fireballs fade
  near the eyes), and the hit feedback was a full-view red wash (now a red rim; classic lighting keeps the wash).
  Marksmen never showed their charge (the cool-down ran out with the burst empty): each burst now starts 0.7 s
  off with a red aiming line from the gun. Delphi also jumps on a real jump.
- Meccaryn/Delphi completion checks (scripts/completion/): pop-up bomb -> prison_7, 4a gate, 5c barracks; tornado ->
  rstory2_battle_1 and rstory5_battle_2 barracks (GVR_BASE=0), and rstory2_battle_1 again with the base built first
  (GVR_BASE_ORDER=all: "Base: complete" then the tornado). Bosses with --auto-aim: 5a's Charger stalled at 50 hp
  once it stood over the player - bossRay (and the vimp and nest sphere tests) rejected rays starting inside the
  sphere; now a hit at t=0. 4b's two Offspring, r1's three Raiks (one only from its hill's own level), sonak_1 done.
  story3_3sonaks_4 left one guard alive in every run: shots hit "1tsonak_4guards", the class-97 group spawner's
  8-triangle marker box, which was loaded as solid scenery - class 97 is now skipped like other helpers; two of
  three runs then complete (the third leaves a guard 12 m below a cliff edge - terrain, not a bug).
  rstory3_pre_race_1: all 12 guard objectives done across runs. Harness: --auto-aim now applies after a script's
  hand poses; GVR_AUTOAIM_HAND=l; GVR_ENEMY_DUMP; GVR_HIT_TRACE names the world object hit.
- Fog colour byte order: the world record 41 colour was read B,G,R (an unverified assumption since the start). Measured
  against each story world's sky texture (average of its four row bands): R,G,B matches in 14/14 (Story3 orange
  123/64/26 vs sky 136/67/24; Rstory1 teal 45/101/78 vs 96/154/150; Kstory4 blue 76/143/225 vs 96/154/199), B,G,R in
  none. Fixed - every world's distant haze, clear colour and the water's sky colours now match its sky (Rstory1's
  horizon had been a flat olive plane against a teal sky).
- Sky haze (rework look): fogOut() blends a fragment toward the sky band's own colour in its direction (the band's
  u = azimuth/2pi + drift, v = elevation over -0.08..0.75 rad, textureLod 2 so the atan seam can't pick a mip;
  the sky's tint, sun glow and grading) instead of the flat fog colour; fully fogged ground matches the sky drawn
  behind it. Sky texture on unit 10 (Renderer::setSkyTexture). Skipped where fog > 0.995. GVR_SKY_HAZE=0 restores
  the flat colour; classic look unchanged. Sweep 56/56 clean.
- Regression caught by eye, not by the sweep: after 6209b36 a "skip the sky lookup when fog > 0.995" early return in
  fogOut() made Mesa render every textured surface white and drop the terrain and hands (no compile error, exit 0,
  sweep clean). Removed (the look3 contact sheet was rendered before that line went in). The harness now logs
  "image: mean N, white P %, black Q %" and the sweep prints it per mission, so a broken frame shows in the sweep.
  Also: a disk-full during that build (make_video.sh never deleted its multi-GB frame folders - it does now) truncated
  an object file; the host build was cleaned and rebuilt.
- Palms (ptree*, canopy*, *palm*) sway: DrawParams::sway < 0 bends the whole tree in world space by -sway m at 10 m up
  (quadratic in the height above its origin, phase from its position), mapped back to model space for the mono
  path. Rework look only, not in the reflection. Two frames 2 s apart in 1a show the fronds moved; sweep 56/56 clean,
  image stats normal (no white frames).
- The development showcase (Smartie, kB_L1 walking in place, MC_turret, Ptree near Terrain::suggestSpawn) was still
  placed in any mission without characters: end, kstory3_1, kstory5_4, rstory5_queen_4, rstory5_to_queen_3 - in
  kstory3_1 a second Kabuto stood in front of the player at the start. Now only with GVR_SHOWCASE=1. Found on a contact
  sheet of the remaining worlds. HUD backing plate: soft rounded texture instead of the white texture.
- r1_saveyan: a white-and-brown shape filled the lower right of the view after ~6 s. GVR_NEAR_DUMP (new) showed the
  rescue Smartie Yan 0.19 m from the player: the race-3 start marker is on top of him. clearSpawn() now steps the
  spawn back along the facing (0.5 m steps) until no visible character / rescue Smartie is within 1.1 m (r1_saveyan:
  1.57 m, Yan in front). 2b_2_returnwife's wife (type 216, ambient) stands on its marker too - the mission title says
  she rides on your back: ambient characters within 0.8 m of the marker become riders (follow the hero's back, not
  drawn in first person). Found while chasing that: Delphi's first-person body hid only head and neck - hair*, l/r_curl*,
  eyes, lips (and Kabuto's jaw, teeth, tongue, brows) stayed; SkinnedModel::headBones() now hides them all, the eye
  anchor uses head+neck only. Plus a near-eye cut for the own body (fragments within 0.14 x height of the eye are
  discarded, Renderer::setNearCut). Popup completion scripts now walk away after setting the bomb (still complete,
  health 100). Sweep 56/56 clean.
- Waving: updateWaveGesture tracks each free hand's sideways speed (head-relative) while it is above the head;
  four direction changes, each within 0.8 s, = a wave (4 s cool-down). VillagerDirector::waveBack puts the calm
  villagers within 30 m into their Wave state (sm_wave), the nearest calls out (gsmNN voice). Harness: 5a village 0,
  scripted wave -> "Wave gesture (hand 1): 6 Smarties wave back". Device test wave_smarties.
- Kabuto's chest beat: in tracking space (the user's own metres) a hand reaching within 0.2 m of the chest point
  (head -0.36 m, 0.1 m forward) while closing at > 0.9 m/s is a blow; three blows alternating hands, each within
  0.9 s = the roar (the Y roar's code path, its cool-down and ground rules). Harness: kabuto_chest_beat -> three
  blows, "Kabuto roars". Device test kabuto_chest_beat.
- Reaper variety from unused original clips (rpman_snipe_kneel_fire, rpman_punch, rpman_hit_eye_die,
  rpman_hit_to_knees_die, rpman_grenade appended to EnemyClip): marksmen fire kneeling; within 1.7 m a Reaper that
  sees you punches (the blow at 42 % of the clip if you are within 2.1 m: 9 damage, a 3.5 m/s shove; 2 s between
  punches; off for Kabuto); Enemy::hitHeight (set by the shot) picks the head or leg death; Reaper grenade throws play
  the throw clip. One-shot actions hold the clip (Enemy::actT; hits, deaths and falls cancel them). Unit test: head /
  leg / body deaths and exactly one punch in 2 s; harness: "Reaper 1 punches you"; sweep 56/56 clean.
- Air ambushes: updateAmbush picks an air squad half the time (GVR_AMBUSH_AIR=1/0 forces): spawned 22-32 m ahead and
  18-24 m up with Enemy::dropping - EnemyDirector drifts them toward the player and down (slowing near the ground,
  clip rpman_Hover), then Advance; one killed in the air falls with gravity. Flame/smoke puffs and a jet sound
  under each. Harness: 2a_1 -> "Ambush: 3 Reapers dropping from the sky", landed and firing 11-16 m away by 9 s.
  Sweep 56/56 clean.
- Villagers cheer: updateVillagers notes whether a living threat (Reaper, Ripper) is within 45 m of any villager;
  when that goes from yes to no and the number of living threats fell (one died - not one walking off),
  VillagerDirector::cheer() puts the calm ones into sm_celebrate for 3.5 s and the nearest calls out. Harness: 5a
  village 0, two Reapers spawned and blasted -> "The danger is over: 6 Smarties cheer". Sweep 56/56 clean.
- Crouchers: EnemyParams::crouchers (0.3 of the non-marksmen; GVR_CROUCHERS) fire with rpman_crouch_fire; while a
  kneeling or crouched Reaper is firing, EnemyDirector::bodyHeight is 0.66 x his height - the hit capsule, the
  head-shot line and the hit-height death choice follow it. Harness: two crouchers firing crouched; auto-aim still
  hits (target 0 at 12 m). Unit tests pass, sweep 56/56 clean.
- Cover: EnemyDirector::findCover (game layer) picks a solid world object of 0.7-6 m radius on the ground near the
  Reaper's next flank spot, and a spot beside its far side from the player; on repositioning (60 %, GVR_COVER) he runs
  there (Enemy::inCover) and fires crouched; abandoned when the player moves 10 m from where it was chosen.
  Unit test: runs to the offered spot; harness 1c: "Reaper 1 takes cover 7.3 m away". Sweep 56/56 clean.
- Knockdowns: a non-lethal hit of 45+ (head shots, close shotgun, blast edges) plays rpman_hit_big then
  rpman_hit_big_getup before he fights on; bodyHeight 0.45 / 0.75 of his height meanwhile. Unit test (down, up,
  fighting; a light hit only flinches). Sweep 56/56 clean.
- Bazooka Reapers fire with rpman_bzka_kneel_fire (they had the spear gun's standing clip) and count as kneeling for the hit box. Sweep 56/56 clean.
- Between bursts a Reaper in cover or a croucher stays down (rpman_crouch_breathe, the low hit box). Sweep 56/56 clean.
- The sky haze is off in the adaptive fallback (look 2) - back to the flat haze, which costs no texture lookup or trigonometry per pixel (its cost on the Quest is unmeasured).
- Room echo: AudioMixer gets a reverb bus (four damped combs + two all-passes per ear, Freeverb tunings scaled to 48
  kHz, feedback 0.80, damping 0.25); positional voices send 0.5-1.0 (more with distance), 2D ones 0.25.
  GameApp::updateRoomEcho casts 12 level rays (40 m x sqrt(hero scale)) and one up every 0.25 s: wet = 0.05 + 0.32
  x enclosed^2 (+0.15 under a roof), glided over ~1 s. Harness (--audio-out, GVR_ECHO_TRACE): 1a open 0.05,
  kstory5_2 0.27, kstory3_2 / 1d 0.32, 2b_2's cave 0.52; a fight's audio there peaks at 15238/32767, no clipping.
  Unit test testReverb (tail rings only with reverb on). Sweep 56/56 clean.
- Delphi's spell gestures (updateSpellGestures, tracking space): with the casting trigger held, the hand's angle
  round the head summed while it is above the head - past 1.1 turns = tornado (if owned); sideways speed > 2.2 m/s
  below the chin accumulated past 0.55 m = fire wall. castTornado/castFireWall factored out of updateSpells; a
  gesture cast blocks the release cast and the bolt for that pull. Harness: delphi_tornado_circle -> "circle
  overhead (400 deg) -> tornado"; delphi_firewall_sweep -> "sweep (0.60 m) -> fire wall". Sweep 56/56 clean.
- User feedback 2026-10-25 22:16 (list): barracks and big nests should spawn, Rippers' burial rule, ragdolls,
  Smarties killable/pickable by all, Vimp corpse + soul + regrowth, textures (grass, rock), Delphi/Kabuto arms
  following the controllers (Delphi's tiny hands, sword not in hand), Kabuto picking trees/NPCs, Delphi not eaten
  by piranhas, Kabuto drowning in deep water, sword sticking/dismemberment, bow (left holds, right draws, fire only
  on release), all cutscenes again. Worked through in that order.
- Spawners (game_app_spawners.cpp): barracks / barracks_story2 / bw_minibarracks / guard_army_barracks send 1-2
  Reapers out of the side facing the player every 20-35 s (in range 110 m x sqrt(scale), cap 4 own alive, by uid);
  guard_ripperbarracks breeds medium Rippers from the nest model pool (which is now also built for Ripper barracks).
  Those buildings are destructible (rework hp 600) and stop when down. Reaper model pool +3 per barracks (max 16);
  when every model is in use a living Reaper takes the farthest corpse's. Placed Rippers: all but Small buried.
  Harness 2c_1: 6 Rippers from two Ripper barracks in 50 s; 5 Reapers from barracks_story2. Completion scripts all
  still pass; sweep 56/56 clean.
- First-person arm IK: Delphi armStretch 1.0 -> 1.25, maxReachStretch 1.4 -> 1.9, handScale 1.45; Kabuto maxReach
  1.3 -> 1.9. GVR_IK_TRACE logs the wrist's miss: 0.000 m for both with the hands pushed forward (they used to
  stop short - the sword, which follows the controller, then floated ahead of her hand).
- Kabuto carries (game_app_kbcarry.cpp): HoldTree / HoldVictim. A tree (destructible kind Tree, up to 1.5 x his
  height) is uprooted - its object hidden, collision off, stage down - drawn along the controller's aim from the
  hand; a fast trunk sweep (tip > 2 m/s real) clubs Reapers / creatures near the trunk segment (x4 damage, 0.5 s per
  target); let go, it flies and tumbles and crashes (blast 0.18 H if fast). Smarties, Vimps and small creatures are
  held (eat at the mouth, thrown = killed on landing, slow release = set down). The X-button grab and the Reaper
  and boulder paths are unchanged. Harness: kabuto_tree -> uproot, throw (15 m/s), crash; kabuto_smartie_throw ->
  pick up, throw, "lands and dies"; grab_eat / grab_throw / climb unchanged. Sweep 56/56 clean.
- Bow (user: left hand holds, right hand pulls, fire only on release): first person the bow is in the move hand
  (left), the turn hand draws the string; the trigger draw is third person only; the fire bolt is off while a bow
  is out (its trigger is the bow hand's). devtest delphi_bows rewritten: right hand to the string (0.09 m), pull
  0.5 m, release -> "Arrow from the HOMING BOW at 76% draw", twice per bow; a trigger pull in between fires nothing.
- Water: Delphi swimming -> no piranhas (waterTime 0); Kabuto past his float depth (0.7 H) -> drowning: 12 raw damage
  per 0.5 s (x0.3 hide = ~7/s), splashes, gasps, a warning line; no piranha bites or blood frenzy for him or Delphi.
  Harness: Delphi at r4a in the sea 12 s, health 100; Kabuto off kstory3_4's coast, "out of his depth", health 17 at
  12 s. Sweep 56/56 clean.
- Smarties (user: killable by all races, picked up, reactions): VillagerDirector gains hp 30, Hurt / Dead / Held /
  HandsUp with the original clips sm_gets_shot, sm_death, sm_squirm, sm_hands_up (one-shots hold their last frame;
  bodies go after 120 s). Weapon target ids 10000+ (villagerRay: a 0.28 m capsule 0.25-0.6 m up), explosions hit
  them, Kabuto's fist/foot squash now kills (a body) instead of erasing. updateVillagerHands: Meccaryn/Delphi grip
  within 0.45 m of one picks it up; let go fast (> 2.5 m/s) throws it (landing > 9 m/s kills), slow sets it down;
  a gun or bow aimed within ~6 degrees for 0.4 s -> hands up. Offspring and name tags skip the dead. Harness: 5a,
  aiming -> "Smartie 5 puts its hands up"; a blast -> two killed; kabuto_smartie_throw -> killed on landing. Unit test
  extended. Sweep 56/56 clean.
- Vimps (user: body drops, soul stands on it, they respawn and grow, killable when adult): VimpDirector keeps the
  dead (no removal), rebirth after 60 s at the herd centre with growth 0.3 -> 1 over 90 s; damage and the targeting
  (vimpRay, auto-aim, explosions, Kabuto's grab/eat, the Offspring) use VimpDirector::killable (alive and grown).
  Eaten by Kabuto: VimpDirector::eat (no body, soul and rebirth as usual). The soul: one extra vp_l1 posed with
  vp_idle, drawn at each dead Vimp with SkinnedModel::renderGhost (additive, blue-white, no depth write), bobbing,
  fading in after 1 s and out over the last 4 s. Harness 2a_1: a blast kills one, its soul stands over the body.
  Unit test: reborn small, not huntable, grown, huntable. Sweep 56/56 clean.

### 2026-10-25 — Rework: Delphi's sword runs enemies through
- User: "Make Delphi's sword stick in enemies, perhaps dismemberment?" In the sword sweep hit (game_app_delphi.cpp) a
  thrust is the tip moving along the blade axis (dot(dir, axis) > 0.7); a slash is anything else above 8 m/s.
  A thrust does x4 damage (85 was not killing a Reaper). A thrust kill sets impaledUid_/impaleAlong_: each frame the body
  is placed at hilt + axis*along (minus 0.55 m so the pose's middle sits on the blade), vel 0, FallCycle looping so he hangs
  limp, blood drips, haptics. Yank (tip velocity along -axis < -2.5), |v| > 9 or 8 s releases him with half the tip
  velocity and FallLand. A slash kill gibs the corpse (dismemberment burst) and bleeds. EnemyDirector::setBodyClip added.
- First render had him lying at the bottom of the view (the death pose with a 1.1 m offset); with FallCycle and 0.55 m he
  hangs on the blade. devtest delphi_impale (Rstory2 battle_1, one Reaper 1.9 m ahead, thrust, lift, yank): harness log
  "Delphi runs Reaper 1 through" / "pulls the sword out". Tests pass; sweep 56/56 clean. HARNESS only.

### 2026-10-25 — Rework: Reaper ragdolls
- User: "Try to implement ragdolls somehow". New src/game/ragdoll.{h,cpp} (our design): the pose a body dies in
  (model-space node matrices from the clip) is captured; nodes at least 0.4 game units (8 cm) from their simulated
  ancestor become Verlet particles (1/60 s substeps, 8 iterations): sticks to the parent particle, siblings rigid
  (hips, shoulder girdle), and a minimum distance (0.6 x rest) to the grandparent so joints don't fold flat; terrain
  contact with friction; sleeps after 0.5 s still (or 10 s). A first soft grandparent spring kept the stick figure
  standing on its feet (unit test failed "fallen over") - changed to a minimum only. Each pose rebuilds every
  particle's frame from its primary child and its parent (root: its two farthest children), F1*F0^T, shortest arc
  when degenerate; ends (hands, head) turn with their limb; other nodes ride rigidly on their simulated ancestor.
- Game layer (game_app_ragdoll.cpp): one Ragdoll per Reaper model slot, keyed by uid; the slot model's pose modifier
  starts (pending capture) and applies it; model->world = T(pos + lift + sink) R(yaw) C(giants->runtime), so a
  sinking corpse sinks whole; ragdolls are posed every frame until asleep, then not at all; e.pos follows the
  particles' centre. Enemy::kick collects pushes (also on the dead: EnemyDirector::push records before its Dead
  check; a lethal fall records half the fall speed). Blast/fling/fall deaths go limp at once (launch capped 7 m/s
  across, 6 up - the first try threw bodies 25 m); a shot death plays 0.35 s of its clip then crumples. The director
  leaves ragdolls' vel and air drop alone. Delphi's impale now pins a ragdoll at the blade (Ragdoll::pin, the
  particle nearest the body's middle); the yank unpins and kicks it. GVR_NO_RAGDOLL turns it off.
- Harness: Rstory2 blasts - three bodies fly flailing, rest 9-12 m off; Story3 auto-aimed shotgun kill - the hit,
  then he falls back and lies (asleep after 1.5 s); delphi_impale - hanging by the torso, legs dangling, dropped
  when yanked. Unit test (stick figure: falls, rests above ground, keeps bone lengths, helper node rides on the
  pelvis; pinned it hangs, unpinned it drops). Sweep 56/56 clean. devtest reaper_ragdolls. HARNESS only.

### 2026-10-26 — Rework: graphics pass 1 (grass, rock)
- User: "All maps need graphics/texture redesign. Grass looks awful and the rocks textures as well." Montage of 9 worlds:
  grass tufts were bright uniform lime sprites on olive ground; cliffs (terrain rock layer) flat teal wallpaper,
  dominant-axis projection smearing where faces turn; ground blurry close up.
- Grass: new renderer Material::Grass. Card blade coordinates (x, height, tuft tone) ride in the vertex normal; vUv
  is the terrain's own texture coordinate at the tuft (Terrain::uvAt), so the fragment runs terrainColor() there
  (bias 10: the ground's local average, not one texel) times a procedural 128x128 blade texture (34 thin curving
  blades, hue per blade, dark roots) bound on unit 11 (setGrassTexture); light = Terrain::lightAt in the vertex
  colour; baked like the terrain. Clumps (3.2 m) within patches (14 m) of value noise, 0.55 m grid, 3 cards per
  tuft, <= 3600 quads within 15 m. Alpha: coverage kept in small mips, edge sharpened by fwidth; alpha-to-coverage
  when the eye buffers are MSAA (Renderer::setMsaa from XrApp::msaaSamples, device only; headless keeps the 0.5
  test). Brightness calibrated by measurement (grass pixels via a magenta mask render vs the ground there): the
  grade curve is non-linear, final blades ~1.15-1.2x the ground.
- llvmpipe (PC test renderer) pitfalls found by bisection, documented in the shader: log2 in the grass branch, and
  terrainColor() called twice or from inside an if-branch, each broke the whole frame (white ground, black cards);
  now one select-expression call site with a material-dependent normal and mip bias.
- Rock (terrain rock layer, rework look only): true triplanar (normal^4 weights), strata bands (~9 m, tilted,
  warm 1.28/1.12/0.9 vs cool 0.72/0.8/0.88), moss on up-facing rock in noise patches, a finer rock octave within
  30 m, a contrast lift; relief: the rock's luminance becomes the bump height (x2.2, 2.5x depth) and the baked
  light lets the bumped normal through more on rock (0.6 + 0.85 ndl vs 0.86 + 0.32 ndl).
- Harness: 1a/1d/5a close views before/after; 1d cliff views; 9-world sweep montage. Tests pass; sweep 56/56
  clean. HARNESS only (A2C path only on device).

### 2026-10-26 — Rework: cutscenes on a cinema screen; cutscene cast fixes
- User: "All cutscenes need fixing. Go through all of them, actors misplacement, black screen, objects placement".
  All 66 cutscene chains (545 shots, computed as shots no other shot names as next) played to their end in the
  harness (GVR_CUT_EXIT=1 stops when the cutscene ends; "cutframe" log lines give shot/time/fade per frame), a
  frame every 2 s, contact sheets plus flags (black frames outside script fades, unstaged actors, near-camera
  culls, actors drawn as the Meccaryn fallback).
- Main finding, immersive view: the shots were staged for their own camera; with a level, free head at the camera
  position many show sky, floor or an undressed side of the set (intro_island_1: 66 s of haze, the camera 615 m up
  looking 34 degrees up; Kstory5_End: mostly dark blue). Decision: cutscenes default to a CINEMA SCREEN - each
  frame rendered from the shot camera (position, look-at or free axes incl. roll; 4:3, 75 degrees across - the data
  carries no FOV, op 0x26 is a distance) into a 1024x768 4x MSAA target (Renderer::initScreen/beginScreenPass,
  blit-resolved), shown on a 3.6 m screen 3.2 m ahead in a dark room, fixed in tracking space (stays put through
  cuts), subtitles under it. Options -> CUTSCENES: CINEMA SCREEN / AROUND YOU (the old view). GameApp::renderWorld
  factored out of renderViews for the second camera. The cinema camera is not lifted out of terrain (a camera
  inside a cliff sees out through back faces, as the original - Kstory5_End_18 showed the ground otherwise) and
  has no 1 m near-cull.
- Cast fixes (both views): untyped performance trees were all drawn as Meccaryns, and one that didn't fit the
  Meccaryn skeleton stole another character's tree (Kstory5_End_18: "wife" on Reg's tree, "smartie" on Baz's; wife,
  Smartie, Timmy, Delphi, Borjoyzee never shown). Now each untyped tree is pinned to itself and gets the model its
  mover names (delphi rp_l0, yan smyan_l0, timmy smtimmy_l0, wife smf_l0, borj* smborjoyzee_l0, smartie/weeman
  sm_l0, toby, kabuto kb_l0, offspring), idle clip via ObjectRegistry::animForModel, Meccaryn only if nothing
  else fits. Typed actors whose model doesn't fit their own tree take anonymous trees first, another character's
  only when nothing else fits (kstory5_kabuto_fight_2's second Kabuto performs on "Delphi_mover").
- A shot faded to black with nothing after it (no later fade, subtitle, cue, effect) ends 2 s later
  (Kstory5_End_18 held 130 s of black after THE END).
- Alpha-to-coverage for every alpha-tested draw on the headset's MSAA (threshold 0.3), so shrub mounds' feathered
  rims and palm fronds get soft edges (device only).
- Harness: cinema contact sheets of the reviewed chains look as staged (Kstory1_Delphi_changes, Kstory5_End,
  Rstory1_vomit/fire, story1_start/jetpack...). Tests pass; sweep 56/56 clean. HARNESS only.

### 2026-10-26 — Rework: ragdolls for the other NPCs; ragdoll demo video
- User asked whether ragdolls cover all NPCs (they covered only Reapers) and for a ragdoll demo video.
- game_app_ragdoll.cpp: one Ragdoll per character model (Rippers and other creatures), villager slot (Smarties) and
  vimp slot, hooked into each model's pose modifier (start from the death pose, apply); keyed by the NPC's uid (pooled
  models are lent on). npcToWorld gives each one's render transform (creature xf incl. scale; villager lift; vimp
  lift x growth and scale), the creature lift is frozen while ragdolled. explode() records recent blasts (0.25 s):
  an NPC dying within 1.6 radii goes limp at once with the push (2-8 m/s out, 2-6 up; Vimps x0.6), otherwise after
  0.3 s of its death clip, pushed away from the player. Reborn Vimps stop theirs; eaten Vimps have no body; burst
  Rippers (exploded) have none.
- Harness: 5a blast - two Smarties thrown limp, one flies up, tumbles, lies; 2a_1 blast - a Vimp goes limp; 1d_1 -
  a Ripper killed by a blast without bursting goes limp, lies still after 5.3 s. devtest smartie_ragdolls.
- scripts/video_ragdoll_clips.txt: before/after (GVR_NO_RAGDOLL), shot crumple, impale, Kabuto throw, Smarties.
- Follow-ups from the cinema review (66 chains, cutrev2/3): "not staged" flags came from the first, too strict
  named-tree rule (fixed by the two-pass preference); story3_mrhorn_2's 9-bone drawbridge took a Sonak's 74-node tree
  (step 3 ties went to the first tree) - ties now go to the tree nearest the skeleton's size, all three Sonaks stage;
  efx_* actors (efx_portal: a "black" void quad and a glow bar) are drawn unlit (SkinnedModel::renderGlow, their own
  blending). scripts/make_video.sh captions now come from a text file (drawtext escaping cut "Delphi's sword: run
  through, ..." at the comma and the clip was dropped from the join).

### 2026-10-26 — Rework: dismemberment
- SkinnedModel::sever(bone): the bone's node and its descendants in the dominant clip's tree; their vertices are
  collapsed onto that bone's joint from then on (per-cut joints, cumulative, cleared when the model slot is given
  to another Reaper), and the cut triangles as posed now are uploaded as meshes relative to the joint (Piece).
  GameApp::dismember: the limb (head, arm l/r, leg l/r) whose vertex centre is nearest the blow in model space
  (<= 1.2 m), the piece thrown as a Gib that owns its meshes (shared_ptr with a destroyMesh deleter; Gib::base keeps
  the body's yaw), blood from the stump. Delphi's killing slash now does this (the old chunk burst only if no limb
  is near the cut); a Blown death within 0.6 radii of a blast loses a limb 60 % of the time.
- Harness: devtest delphi_dismember (GVR_REAPER_HP=30, new test hook for one-blow kills): "Reaper 1 loses his
  head", the head flies, the body lies headless; ragdoll blast script: "Reaper 5 loses his head". HARNESS only.
- Graphics: non-green worlds now get sparse dry tufts (30 % density, the ground's own colour: orange scrub on
  Story3's sand); snow/ice grounds (blue above green, or very bright) none - Rstory2's flats stay bare.
- Graphics: ground stones. Up to 260 jittered, squashed icosahedra (20 flat-shaded faces each, every face oriented
  outward from the centre - a remembered face list had mixed winding and showed half-shells) on a 1.3 m hashed grid
  in 9 m patches within the grass radius, rebuilt with the grass, one dynamic mesh, one draw (also at the fallback
  quality). New Material::Stone: the ground's colour at the stone (terrainColor with an up normal, bias 3), paled,
  grained by the rock texture's luminance against its mean, lit like rock (0.55 + 0.9 ndl) - the terrain rock layer
  itself is near-black on several worlds (their cliffs only read through baked light).
- Building health bars no longer show within 4 m (x hero scale) of the eye and stay under ~10 degrees across (video 9 found a bar filling Kabuto's view when his swung tree hit a hut).
- Graphics: Kabuto's cliffs (Kstory3/4/5) showed the rock texture's 9 m repeat as an orange grid. Kstory3 and
  Rstory2 start at effects quality 2 (Quest measurements), where the rock was one dominant-axis sample, so the
  triplanar/strata work never reached them. Both paths now blend a second sample at ~1/4 the frequency (offset),
  by large-scale noise and more with distance (look 1: triplanar pair; look 2: one extra fetch). The grid is gone.
- Rock projection follows the true facet normal (cross of screen-space derivatives) instead of the smoothed vertex normal: sheer terrain walls (Rstory2's ice canyons) had a band of smeared streaks where the smoothed normal still leaned up. Both quality levels.
- Reapers: cover behind trees. findCover now also offers class-60 objects (trees) at their placement point (the
  trunk; their bounding radius is the fronds' 6-20 m, which had excluded them), the spot 0.75 m behind the trunk
  away from the player, with a tree flag; such a Reaper hides standing (TreeHide -> TreeBreathe, a TreePeek now and
  then; bodyHeight 0.25 x while hidden), rocks/walls keep the duck (RockInto -> RockBreathe). Unit test for both;
  1c_1: four Reapers take cover behind palms and fire round them. Sweep 56/56.

### 2026-10-26 morning — Rework: dismemberment fixed for the soldiers; heavy guns
- Regression check after the night's work: all 12 completion scripts reach their objectives; all 29 device-test
  scripts run clean in the harness with their mechanics firing.
- Bug: dismember() looked up "arm l"/"leg l", names from another skin; the Reaper soldier (rpman_l0) has l_arm,
  r_arm, l_thigh, r_thigh - so every cut could only ever take the head (and a chest blow took the head too, its
  centre being nearer than anything else found). Limbs now try each skin's names; the head only for a blow within
  0.22 m of its centre's height. Delphi's chest-height slash now takes the arm it crossed.
- Sniper head-shot kills take the head; a shotgun shell (it bursts: the direct hit goes through onHit, then the
  burst) that kills a Reaper within 8 m takes the struck limb 55 % of the time (in updateRockets, where the shell
  lands); blast deaths aim at the limb nearest the burst. Cutting the right arm drops the gun as a gib (Gib::base is
  now a rotation matrix, so it keeps the gun's in-hand orientation) and stops placing it (Enemy::gunArmOff).
- Test hooks: GVR_DISMEMBER_TRACE, GVR_DISMEMBER_ALWAYS, GVR_DISMEMBER_LIMB=<bone>. Tests pass; sweep 56/56.
- CPU check (GVR_PROF_SPIKES=<ms> logs each section over that, with its frame; GVR_PROF_BEGIN/END for non-scope sections; Reaper posing, guns and water now timed): across all 56 missions only two sections exceeded 3 ms after the loading frames (Kabuto's posing in kstory3_3 and the villagers in mission1, ~4.4 ms once each). The 21 ms first-frame Reaper gun load happens under the briefing.
- Reapers: locational hits while alive. A shot within 0.45 m of the middle of his gun (reaperGunCentre: the gun
  model's bounds centre in weaponXf - its origin is near the ground) is a gun hit, not a head shot (he holds it up
  to his face), does 12 instead of 25, and knocks the gun out 70 % of the time (a near-arm hit 40 %): it flies off
  as a gib (dropReaperGun), Enemy::disarmed - the director gives him no shoot range, a 1.1 m stand-off and no flank,
  so he runs straight in and punches. A leg hit (nearest limb a thigh, under 0.45 of his height) sets limpT 5 s:
  0.42 x speed and a walk clip. Unit tests (disarmed: never shoots, punches; limping: under 0.6 x the distance).
  Harness 1c_1 with GVR_AUTOAIM_PART=gun (new hook; =legs too): "his gun shot out of his hand", "punches you".
  Completion kill scripts and sweep 56/56 still pass. devtest reaper_disarm.

### 2026-10-26 — Rework: impact sounds; gun hits along the shot line
- Sounds (the game's own): a ragdoll records its hardest landing (Ragdoll::impact, downward speed at ground contact);
  the game layer plays sr_fall1/2 for a Reaper over 6 m/s, else ex_bdyl1/2, louder with speed, at most every 0.3 s;
  Smarties/Vimps/Rippers ex_bdys1/2. A cut limb: ex_bdyl1/2. Gibs landing over 3 m/s: guns me_drpwp/ex_metls, else
  ex_bdys1/2. sr_fall1/2, ex_bdys2 and me_drpwp added to the preload list. New test hook GVR_SFX_TRACE logs each
  sfx and whether it resolved; reaper_ragdolls and smartie_ragdolls: every one resolved, ~3 thuds per body.
- Bug found doing that: a dropped gun gib sat at the gun model's origin (near the Reaper's feet), so it "landed"
  at once without a sound and turned about a point on the ground. It now starts at the gun's middle
  (reaperGunCentre) with the model offset into Gib::base, so it falls and spins about itself.
- Bug: in 1c_1 shots aimed at the gun stopped on his capsule 0.46-0.51 m beyond it (the gun is held out in front)
  and counted as head shots (hit height 0.8 is above the head line). A gun hit is now also a shot whose line passes
  within 0.22 m of the gun's middle within 0.9 m of where it stopped. Harness: "drops his gun", the clatter plays.
- GVR_HIT_TRACE prints state, head/gun flags and the line distance. Tests pass; completion checks 12/12; sweep 56/56.

### 2026-10-26 — Rework: editor markers no longer drawn
- Look review (12 worlds, start views): red cones stood in the sea and floated over the ice (r4a, rstory2). A dump of
  every placed object whose parts are EFX_* across 15 missions: army_spline (308 in r4a alone: the armies' routes),
  marker_spline (race rivals' lines) - both from xx_markers.gzp, the editor's marker archive - mr_invisible (0.2 m, in
  11 of 15 missions) and rstory5_fake (a 3.4 m red stand-in). All four are now invisible helpers (not drawn, not
  solid); the game still reads army_spline/marker_spline from the mission data (base attacks, race rivals: "12 army
  paths lead to the base"). buoy_spline stays drawn - it is the race's only gate marker. r4a: 419 -> 111 placed.

### 2026-10-26 — Rework: taking hold of bodies (Meccaryn, Delphi)
- game_app_bodygrab.cpp: a grip edge with the hand within 0.42 m of any particle of an active ragdoll (dead Reapers;
  creatures, Smarties, Vimps) takes that particle: Ragdoll::nearest / pinParticle / movePin (the pinned particle
  follows the hand; the rest hangs and drags). Let go: if the hand went over 2 m/s in its last 8 frames (the fastest
  of them - a hand opens as the arm slows), the whole body gets that velocity x 0.55 (Reaper) .. 0.85 (Smartie,
  Vimp). The owner's position follows the body's centre. First person, not Kabuto, not on the Reaperski; the
  impaled body excluded; the move hand's sprint is off while it holds a body or a Smartie. Haptics: a pull that
  grows with the swing. GVR_GRAB_TRACE logs reach misses and the hold.
- Held by one point, the 8 constraint passes let a long chain stretch while lifted: 24 passes while pinned. A body
  picked up long after it fell had age > 10 s and fell asleep mid-air on release; pinning caps its age at 0.5 s.
- Test hook GVR_TEST_KILL=quiet: the script's Reaper kill is a plain kill instead of a blast (the body stays put).
  devtest body_grab (1c_1, left hand): "Took hold of a Reaper body (1) by part 43, 0.29 m from the hand", lifted,
  swung, "Threw the body (1) at 5.0 m/s". Unit test (stick figure taken by the hand, lifted 2 m: the hand point at
  the hand, the body hanging over 0.9 m below, the forearm 0.4 m; thrown at 6 m/s: lands over 1 m away). Sweep 56/56.
- Thrown bodies (updateThrownBody, 2 s after the throw): while the body's centre moves over 2.5 m/s, a living
  Reaper within 1 m across (and at body height) is shoved along the flight (1.2 x its speed, up to 8 m/s) and takes
  6 + 4 x speed damage, once each; the body stops against him. The throw speed is now the fastest hand speed over
  the last 12 frames (0.17 s; a scripted release 0.15 s after the swing stopped found none in 8). Test hook
  GVR_PASSIVE_REAPERS (they never notice the player). devtest body_throw_hit: "The thrown body hits Reaper 2 at
  9.0 m/s" (he moved 1 m). Sweep 56/56 clean.

### 2026-10-26 — Rework: ragdolls in water
- Ragdoll::waterLevel (the game sets the sea level): a particle under it is slowed 15 % per substep and buoyed by
  0.8 g - it sinks at ~0.2 m/s; going in through the surface records the speed and place (splash/splashAt);
  a body still sinking does not count as still (sleep waits; forced sleep at 10 s waits too, up to 60 s).
  GameApp::ragdollSplash plays waterSplash there (se_splsh over 3.5 m/s, else bi_watr1) for Reapers and NPCs.
- Unit test: dropped into water over a 5 m bed - splash over 4 m/s, under the surface but above -2 m after 2 s,
  sinking under 0.5 m in the next second. Harness r4a (shallow sea): three Reapers blasted - "a body splashes
  into the water at 6.1 m/s", se_splsh.

### 2026-10-26 — Rework: Kabuto picks up bodies
- updateBodyGrab now serves Kabuto too: his hand point and reach as his own grab (grip pose + (0, 0.02, -0.09) H,
  0.24 H), dead Reapers and creatures only (his own grab takes Smarties and Vimps), not while that hand holds or
  climbs or he is eating; his own grab skips while a body is held. A Reaper body brought within 0.22 H of the
  mouth: kabutoEat(index) - the bite now also takes a dead Reaper (removed, +25 health, growth as for a live one).
  His throw threshold scales with the world scale; the kick x0.9 capped at 40 m/s. The taken part eases into the
  hand over 0.15 s (his reach is 1.5 m and more - it jumped). devtest kabuto_body_eat (k1a): "Took hold of a Reaper
  body (8) by part 6, 1.52 m from the hand", "Kabuto: a body brought to his mouth", "Kabuto ate something".
  kabuto_grab_eat / smartie_throw / tree and the Meccaryn grab tests unchanged. Sweep 56/56 clean.

### 2026-10-26 — Rework: stones to pick up and throw
- The ground's stones were draw-only (a hash grid round the player). buildPebbles is split into pebbleAt (one cell's
  stone as laid out round a centre - its size fades toward the patch edge) and emitPebble (its faces, now with a
  rotation and position); the layout renders pixel-identical (compare: AE 0 on Rstory2's start view).
- game_app_stones.cpp: a grip edge (Meccaryn, Delphi; first person; hand free of a body, a Smartie, a stone) with
  the hand within 0.22 m of a hand-sized stone (under 0.34 m) - a loose one, or a ground one whose cell is then
  marked taken and the ground's stones rebuilt - puts it in the palm. Let go: the fastest hand speed of the last 12
  frames x1.15 if over 1.5 m/s, a random tumble; gravity, ground bounces (x0.55 across, x0.3 up; ex_stons by speed),
  a plop into the sea and slow sinking. Over 4 m/s it hits the first Reaper (capsule 0.42 m + its size; above
  0.82 of his height = head, x2), creature or Smartie it meets: 4 + 1.1 x speed (Smarties 2 + 0.6 x speed), a shove,
  bi_hit, and bounces off. 16 loose stones at most (the oldest lying one makes room); one lying a minute 40 m behind
  is forgotten. One dynamic mesh, rebuilt once a frame, drawn with the Stone material at any look.
- devtest stone_throw (1c_1, player placed by a stone, a passive Reaper 6 m ahead): "Picked up a stone (0.30 m)",
  "Threw a stone at 10.0 m/s", "A stone hits Reaper 1 at 10.5 m/s". Harness frames show the stone in the gauntlet.
  GVR_GRAB_TRACE lists the nearest hand-sized stone on a miss. Sweep 56/56 clean.

### 2026-10-26 — Rework: GROUND REACH option
- Picking things off the ground meant crouching to the floor. GameApp::reachPoint: with GROUND REACH: EASY (new
  Options row, settings key reach_assist, default on) a hand under 1.1 m above the ground and at least 0.22 m in
  front of the head (horizontally, along the view) reaches up to 0.55 m lower (fading in over the top 0.3 m and
  the first 0.1 m ahead) - used by the stone, body (Meccaryn/Delphi) and Smartie pickups, which also still take
  what is at the hand itself. Not for Kabuto. Arms hanging at the sides take nothing, so a grip squeezed to sprint
  doesn't pick up stones; sprint is off while the move hand holds a stone.
- Harness stone_throw with the hand stopping higher: tracking y -1.05 / -0.7 / -0.5 pick the stone up, -0.3
  (0.9 m above the ground) doesn't. Unit tests pass.

### 2026-10-26 — Rework: a disarmed Reaper goes back for his gun
- Gib::owner marks a dropped gun with its Reaper's uid. Each frame a disarmed Reaper (arm on, alive) whose gun has
  lain 1.5 s - 15 s within 10 m, with the player over 2.5 m away, runs for it (Enemy::fetchGun / gunAt: the Advance
  goal); within 0.9 m he picks it up (the gib goes, weaponOn, disarmed off). The player close: he punches instead.
  The gun now leaves the hand at 4.5 x (was 3.5 x) the shot's direction (+0.6 up). Harness 1c_1 (gun shot out,
  firing stopped): "runs for his gun (7.8 m)" then "punches you" as the player stood in the way; with the gun
  2.4 m away (first try, 0.8 s delay) he had it back at once - hence the delay and the longer throw. Sweep 56/56.

### 2026-10-26 — Rework: guns and limbs in the hand
- Gib::hand/heldRot/heldOff: a grip edge within 0.35 m (hand or GROUND REACH point) of a dropped gun's or a cut
  limb's middle (model bounds centre through its transform) takes it (before stones); it follows the hand with the
  rotation it had relative to the hand, its middle (a gun) or cut (a limb) in the palm; let go, it flies with the
  fastest hand speed of the last 12 frames and tumbles, and the gib physics takes over (life restarts: 18 s to
  sink). A gun in your hand never lies long enough for its Reaper to fetch it. Test hook GVR_TEST_KILL=disarm (the
  script's kill drops the gun at his feet instead). devtest gun_grab: "Picked up a dropped gun", "Let go of a gun
  (8.3 m/s)"; a harness frame shows the spear gun upright in the Meccaryn's fist.

### 2026-10-26 — Rework: swimming by arm strokes
- GameApp::updateSwimStrokes: while swimming (first person, not Kabuto), a hand with its grip over 0.6 (not holding
  a stone, gun, limb or body) moving through the water - its tracking-space movement turned into the world, so
  walking and snap turns don't count - pushes the player the other way (2.5 x the horizontal pull: a 0.6 m stroke
  is about 1.5 m/s); the push decays (e^-1.1 t) and is capped at 1.6 x swimSpeed. PlayerInput::stroke is added to
  the swim velocity, stick plus strokes capped at 1.7 x swimSpeed. GVR_SWIM_TRACE.
- devtest swim_strokes (r4a deep water at game (2525, 270), found with GVR_FIND_SEA): six breaststrokes - stroke
  speed 1.6 m/s after the first, 2.5-3.3 m/s at the peaks, the player's speed following it.

### 2026-10-26 — Rework: one-time hints for the hand mechanics
- game_app_hints.cpp: bodies (a dead Reaper within 4 m, 2 s after death), stones (after 45 s of play, a hand-sized
  one within 2.5 m), a dropped gun (within 10 m, lying 1 s), swimming (2 s) - each shown once as the line under the
  view (6 s), only after 3 s with no other line showing, Meccaryn/Delphi first person; settings key hints_shown
  (bits) keeps them from showing again. Harness: swim_strokes shows the swim hint; a 25 s 1c_1 run with a quiet
  kill shows the body hint after Timmy's line.

### 2026-10-26 — Rework: Smarties thrown at Reapers
- A flying Smartie over 5 m/s within 0.6 m across of a living Reaper (at body height) shoves him (0.8 x its speed,
  up to 6 m/s), does 5 + 2.5 x speed and bounces off. The Smartie throw now takes the fastest hand speed of the
  last 12 frames x1.2 (was the smoothed speed x1.4: a release just after the arm stopped threw at 4.9 m/s).
  GVR_GRAB_TRACE logs the nearest Smartie on a grip and the flight; test hook GVR_STILL_SMARTIES (they stand
  where they are). devtest smartie_throw_hit (5a): "Threw Smartie 5 (11.2 m/s)", "A thrown Smartie hits Reaper 1 at
  11.5 m/s".
- Thrown guns and limbs (Gib::thrown, let go over 4 m/s) hit the first living Reaper they meet: a gun 6 + 1.6 x speed,
  a limb 3 + 0.8 x speed, a shove, a clang or a thud, and bounce off. Harness (gun_grab with a second Reaper 4.5 m
  ahead): "A thrown gun hits Reaper 2 at 8.3 m/s". Sweep 56/56.

### 2026-10-26 — Rework: Reaperski handlebars
- updateRiding: both grips over 0.6 (first person) take the bars (haptic tick). Steer = the bar's yaw change since
  taken (tracking space, left hand to right hand) / 0.45 rad, clamped; throttle = the turn hand's pitch change since
  taken / 0.55 rad (-0.5..1: a wrist twisted back is gas); whichever of stick or bars is larger wins. Turbo is A while
  holding the bars (the move-hand grip is a bar). Hint once (hints_shown bit 16). New script command pitch_r/pitch_l
  <deg> t0 t1 (a cumulative wrist twist). devtest ski_handlebars (rstory3_race_1): "Handlebars taken", throttle
  1.00 after the twist, speed 4.4 -> 21 m/s after the start, steer 1.00 with the right grip pulled back. Sweep 56/56.
- Found while checking the PC: its devtest folder had 21 of the 39 test scripts (the rework's hand tests were never
  copied) and an older devtest.ps1 (sweep world mapping). All 39 scripts, devtest.ps1 and README copied to
  Documents\GiantsQuestVR\devtest. (swim_strokes, smartie_throw_hit, ski_handlebars need an APK after 3889d03.)

### 2026-10-26 — Rework: fixes from a review of the day's hand code
- A fresh-eyes review (a separate agent reading the diff 5a7e7ce..HEAD) found six issues; all fixed:
  1. Kills by thrown things (bodies, stones, guns, limbs, Smarties) skipped what every other kill does - the
     victim's mission objective (a "kill this Reaper" target killed with a stone stayed open for good), blood,
     comrades alerted. Now thrownHitEnemy / thrownHitCreature (as the shot path, incl. creature gore and sounds).
  2. Throw speeds came from world-space hand positions, so a snap turn (0.25 m jump in a frame, ~20 m/s) held in
     the 12-frame "fastest" window turned a gentle release into a lethal throw. handVelocity: the hand's
     tracking-space movement turned into the world, plus the player's velocity - for bodies, stones, guns/limbs
     and Smarties. Harness: stone held, snap turn, released 0.13 s later - no throw.
  3. A thrown Smartie could hit the same Reaper on consecutive frames (a fast upward lob kept its speed after the
     bounce): FlyingVillager::hit, once.
  4. Climbing and the hand pickups could take the same grip press: a hand holding something doesn't climb, a
     climbing hand doesn't pick up.
  5. A held/thrown/impaled body could be the "farthest corpse" whose model slot a new living Reaper takes (it
     vanished from the hand): those are excluded.
  6. The loose stones' vertex buffer was uploaded every frame: now only while one is held or flying, when the set
     changes, and once after the last one stops.
- All grab/throw/climb/impale devtests re-run (expected lines), unit tests pass, sweep 56/56 clean.

### 2026-10-26 — Rework: sky reflected in the water at the fallback quality
- At quality level 2 (Kstory3 and Rstory2 start there; any heavy scene can fall to it) there is no reflection
  pass and the water's reflection was the sky gradient: Kstory3's sea a flat purple slab. The water shader now
  samples the sky band (unit 10, already bound for the haze) in the reflected direction - the flat mirror
  direction bent 30 % by the waves (the full wave normal sparkled into noise far off), LOD 2.5, one fetch. Level 0
  unchanged (AE 0 against the earlier render). Sweep 56/56 clean.

### 2026-10-26 — Rework: a controls card in the options
- Options -> CONTROLS (second row, its value the hero) opens a card listing that hero's controls, written from
  docs/INSTALL.md and the rework's hand mechanics; any button, B or the stick closes it (row navigation and
  settings changes are suspended while it shows); reset whenever the menu opens. Harness: opened as the
  Meccaryn (1c), Delphi (r1_saveyan) and Kabuto (k1a) - the card renders legibly over the world.

### 2026-10-26 — Rework: scorch marks
- Found in the data: blastmark_1.tga (w_menus.gzp: a near-black starburst in its alpha; blastmark_2 is the Ripper
  hole we already use), Kabuto_footprint.tga (xx_kabutolev1.gzp) and mc_machinegun_shell.gbs - none used before.
- game_app_marks.cpp: GroundMark (kind 0 scorch, 1 footprint - next), up to 48, each drawn as a 3x3 grid whose
  corners sit on the terrain (+4 cm): a flat quad on uneven ground sank in or floated. Fades in 0.15 s, out over the
  last third of its life. explode() adds a scorch (0.8 + 0.55 x radius m, at most 7; 75 s) for fire, grenades and
  blasts with damage - not Kabuto's quakes, not under water or in the air. A fresh scorch glows (additive, a soft
  speckled disc made in code - the game's glow textures are coloured), fading over 4 s, flickering.
- Measured (harness, 5a, two blasts 3 m ahead): the ground inside the mark at half its brightness (58/45/14 against
  118/90/32). Test hooks GVR_MARK_TRACE, GVR_MARK_DEBUG (draws the marks opaque).
- Kabuto's footprints: each footstep (the existing stride-phase footstep in updateAudio - note it runs only with an
  audio mixer, so harness checks need --audio-out) adds a kind-1 mark at the foot, 0.2 H long, half as wide (the
  texture is 32x64), the left foot mirrored (negative width), 45 s, not on structures or in water. Harness k1a:
  8 marks, 72 cells after 4 s of walking. On Kstory1's dark moss they are subtle (the print is a dark texture).
- Spent casings (game_app_casings.cpp): mc_machinegun_shell.gbs (xx_mecc_flick.gzp, a cutscene prop: 7 cm) thrown
  out to the right and up from behind the muzzle with the player's velocity added, tumbling (14-24 rad/s); bounces
  x0.45 across / x0.35 up, a quiet high ex_metls "tink" within 12 m, rests after 3 bounces, gone after 9 s or in
  water; at most 40. Machine gun x1, sniper x1.8, shotgun x2.2. Weapon test indices (GVR_WEAPON): 3 handgun,
  4 shotgun, 5 machine gun, 6 sniper, 7 mortar. Harness 1c machine gun: casings fly out right of the gun.
- Arrows stick (StuckArrow): WeaponSystem::hitStyle() tells onHit a style-3 shot (an arrow) struck; it stays -
  in the world, or on a Reaper (his uid, the point and direction in his own yaw frame, following his position and
  sink; dropped when he goes ragdoll or is torn apart), 20 s, at most 30. The original's rparrow is only a glowing
  streak (two crossed quads of b_streak2): stuck, it was invisible, so a stuck arrow is drawn as a dark shaft
  (0.75 m, two crossed quads) with teal fletching. devtest arrows_stick (Rstory2): "arrow stuck ... in a Reaper"
  twice, once in the ground.
- Smarties: villagerFearT_ (40 s) set when the player kills one (hitVillager - shots, thrown stones, hard landings
  after a throw); while it runs the player's position is a threat (they flee within the fear radius, calm ones
  don't wave). Thrown bodies, fast stones and thrown Smarties are scares. devtest smartie_fear (5a, the shotgun):
  "Smartie 5 killed", "The Smarties are afraid of you now"; the others run off.
- Kabuto's footprints moved out of updateAudio (which returns without a mixer) into updateGroundMarks, on their
  own stride phase.

## 2026-10-26 (day): ambient birds (8c464bb)
- xx_birds.gzp holds Flak_l0/l1 (+ peck x2, hop, fly, glide, land clips), Lobird_l0 (+ fly), hitcher (idle, fly),
  Feather1 (+ float) and CR_CAW1/2, CR_DEAT1/2; no level we load places any of it. Measured: Flak 1.0 m long,
  Lobird 17.2 m across the wings, the feather 0.58 m (a Lobird's; drawn at 0.45 for the Flak).
- game_app_birds.cpp: a pool of 20 SkinnedModels (3 Lobirds + 17 Flak; each bird posed on its own; 71/111 vertices,
  negligible CPU). Flocks of 4-7 land every 20-35 s 16-28 m away in the half behind the body (x Kabuto's scale), on
  land that is above the sea and not steep. Scares: the player within 7 m (x scale), a shot within 35 m, a blast within
  40 m, a living Reaper within 8 m - the whole flock goes, staggered, away from it, climbs to ~20 m, flaps / glides,
  despawns past 95 m. Lobirds circle (r 55-90 m, 60-90 m up) round a centre that drifts after the player at 3 m/s and
  is re-placed if the player is suddenly >200 m away (first version: they started at the world origin, 950 m off).
- Kills: player projectile segments, shot impacts within ~0.55 m of a bird, and thrown stones (birdHitBy). The bird
  falls with gravity and a spin, lies face down 45 s; into the sea: a splash. Feathers drift down with a sway.
- Harness: birds_takeoff ("flock 0 takes off"), birds_shoot (2 Flak shot down; the first aim tried hit the ground
  short - aim tuned to 10/-6), birds_lobirds (turning, three Lobirds seen; GVR_BIRD_TRACE logs their bearing).
  ASan 0 reports on the three + stone_throw; sweep 56/56 clean; unit tests pass.
- The scratch disk filled up (25 GB of video frames) and ninja failed silently on the archive step; old frame PNGs
  in the scratchpad deleted. Note: check `ninja` output for "No space left" when a change seems to have no effect.
- B15, part of it a leak: SkinnedModel never freed its GPU meshes (one set per instance), and every world switch
  makes new Vimps, villagers, birds, character pools. ~SkinnedModel now destroys them (the renderer pointer is kept
  from load; not copyable). Renderer::shutdown clears its table first, so models destroyed after it do nothing.
  Soak 1a -> r1_turbo, 12000 frames, a mission every 500: 383 MB -> 341 MB at the end. ASan: soak 4000 frames,
  delphi_dismember, reaper_ragdolls, smartie_ragdolls, kabuto_grab_eat - 0 reports; sweep 56/56.

## 2026-10-26 (afternoon): fish (Shiners), catching them by hand
- xx_fish.gzp: Shiner.gbs/.skn + Shiner_swim.anm (0.46 m long), unused by the levels. game_app_fish.cpp: three
  schools of 7-12 in water 0.8-3 m deep, 8-35 m from the player (re-placed when >70 m behind), 0.12-0.45 m under
  the surface; 4 shared SkinnedModels posed at different phases (cheap). Scares: the player in the water (3.5 m),
  a hand moving >0.7 m/s in the water (1.4 m), new splashes (6 m), blasts (15 m). A hand held still under water
  draws its school (target = the hand).
- The sea is alpha-blended without depth writes and was drawn inside AssetWorld::render before the characters, so
  anything under it drawn later showed OVER the water. New AssetWorld::underwater hook, called just before the sea:
  the fish and the swimming piranhas (moved there) now show through the water, tinted by it.
- Catching: a grip's edge with the hand under water within 0.32 m of a fish (0.22 m if fleeing) - it leaves its
  school, wriggles in the hand (haptic pulses), counts as a held thing (stoneInHand: no climbing/other grabs with
  it). Let go: the fastest of the last 8 hand velocities; into the water = back to the nearest school (darting);
  on land = flops (hops, twists, weaker) for 8 s, lies, gone after 40 s; >5 m/s into a Reaper = a slap
  (thrownHitEnemy 1 + 0.3 v).
- Harness: fish_school (GVR_FISH_TEST: a school straight ahead; walking in scatters it), fish_catch
  (GVR_FISH_TEST=hand: a school round the right hand; "caught by hand 1", "let go at 3.2 m/s", "lands on the
  ground - it flops"). Meccaryn can only wade (swimming = piranhas, original rule); Delphi swims freely.
- Cloud shadows: cloudShade() multiplies the sun term of sunShadow() everywhere (terrain, objects, characters,
  water spec): the terrain's tileable noise (unit 9, bound for every draw) at 0.008/m, drifting (uTime), strength
  uCloud = 0.5 (GVR_CLOUDS=<s> to test; 0 off). Off at quality >= 2 and in the classic look. Found while tuning:
  the noise texture is uploaded SRGB8_ALPHA8, so the shader reads it gamma-decoded (median 0.56 stored -> ~0.27
  read); the first threshold (0.5-0.66) never triggered - now 0.24-0.40. Baked-light terrain mutes the effect.

## 2026-10-26 (afternoon): ambient markers -> ambience and weather
- Every level holds "a_..." objects from xx_ambient (hidden helpers): per level dozens of a_shoreline*, a_single_tree,
  a_high_winds(_light), a_wind_gusts, a_river, a_pools, a_volcano, a_smartie_village..., plus ONE level marker:
  a_m1_near_beach .. a_m5_beach_waves, a_r1_light_rain, a_r2_snow, a_r3_race, a_r4_darker, a_r5_mountain, a_k1..a_k4,
  a_k5_storm (full list per world in the harness log "Ambient markers:"). No archive has sounds for them (searched
  every .gzp: no ambient loops; Stream/Story1's F_AM_* are cutscene tracks). How the original used them (EAX zones?
  streamed ambience?) is unknown.
- audio/ambience.cpp: our procedural loops, 22 kHz mono, seamless (generated 0.75 s long and the tail cross-faded
  into the head): surf (two breakers per 16 s: swell, crash thump, foam wash, sea roar), wind (gusting brown noise +
  a sweeping whistle), leaves (bright noise in breaths + flutter), river (bed + rising-chirp bubbles), rumble,
  rain (hiss + 500 drop ticks/s). Unit test: finite, under full scale, rms > 0.02, seam step <= 6x the mean step.
  Spectrograms checked (the surf's foam first switched on abruptly - smoothed). ~70 ms to make on x86, done when the
  mixer is attached (startup), never in a frame of play.
- game_app_ambience.cpp: per kind the nearest 1-3 markers in reach (shore 70 m, wind 90, trees 28, river 40,
  volcano 160; x sqrt(hero scale)), each a looping voice placed 1-6 m off in the marker's direction with the mixer's
  roll-off undone (its 1/d from 2 m made 30 m inaudible), faded in/out, random pitch 0.92-1.08 so copies don't
  phase. Weather from the level marker (GVR_WEATHER to force): light rain 600 streaks, storm 1100 (faster, slanted),
  snow 900 flakes swaying, in a cylinder round the head (13 m, wrapped as you move), splashes near you, lit by the
  sky; the rain's hiss in 2D. Off at AMBIENCE OFF; weather only at SOUNDS + WEATHER (the default).
- Weather light: rain/snow/storm make the sky overcast (sun x0.6-0.8, haze greyer, faded in over ~2 s); the storm
  has lightning every 7-21 s (two or three flickers in 0.4 s: ambient, sky and haze brightened) and thunder 0.4-3 s
  later (three procedural claps: a crack and a roll of deep noise bursts, a one-shot). Harness Kstory5, 1500 frames:
  2 strikes; frame brightness 32.8 -> 50.5 at the flash; audio rms 0.057 -> 0.14 in the thunder.
- Independent review (an agent that had not seen the work) of birds / fish / ambience / ~SkinnedModel / cloud shadows:
  no crashes or double frees; fixed: (1) the weather's light changes were captured as the next level's base light on a
  switch (compounding; and the storm's timers weren't reset - thunder over a loading screen) - applyWeatherLight now
  records what it changed and updateSun undoes it first (a value only if nothing rewrote it since); timers reset per
  level; (2) ambient voices placed under 2 m were cut instead of compensated (the mixer has no roll-off within its
  2 m) - exact inverse of the mixer's curve now; (3) a fish let go while the hand was untracked appeared at the world
  origin, and the first sample after a tracking gap made a huge throw - last seen position / no sample after a gap;
  held fish dropped when the hero becomes Kabuto; (4) the cloud uniform now checks the noise texture is live (white
  would halve the sun everywhere); (5) the piranhas' swim pose once a frame (the underwater hook runs in every world
  pass); weather quads built once a frame; cached getenvs; dead birds not re-skinned; GVR_SKIP_TEX no longer leaks.
  Not changed: bird skins are re-made per mission (load-time cost only), rain falls inside caves (terrain-only ground).
- Smartie touch: an open hand (grip < 0.35) within 0.3 m of a calm Smartie's head: a pat (sm_laugh1, turns to you) or
  a high five (it was waving, or the hand moved > 1.3 m/s: sm_celebrate, a slap); 3 s per Smartie; not while they fear
  you. devtest smartie_pat.
- Kabuto and the Lobirds: round Kabuto the circle is 1.3-2.0 H up and 2.5-4.5 H out (H his height); every 22-42 s
  one swoops so its body's middle passes 0.45 H in front of his eyes, 0.15 H to a side (GVR_LOBIRD_SWOOP: every 6 s).
  A grip's edge within 0.3 H + 2 m of it (its 17 m span) catches it (the hand must be free: no kbHold_, stone, climb);
  held hanging from the fist; within 0.22 H of the mouth: eaten (+15 health, feathers, ka_vimp); opened: flung at the
  hand's speed, it flies back up. Eaten / shot-down Lobirds are re-made after 40 s far off. devtest kabuto_lobird
  (deterministic: caught at 5.43 s, eaten at ~6.6 s).
- Bird and fish skins are loaded once and kept across mission loads (they were re-parsed and re-uploaded at every
  mission: 22 skins); soak 1a -> 2a: Flak loaded once, Shiner once. Vimp meat can also be taken by hand (a grip's edge
  within 0.35 m of a piece, GROUND REACH applies) - BUILD only, no harness test for it yet.
- Hitchers: xx_birds' hitcher.gbs (+ hitcher_idle / hitcher_fly; 1.6 m tall, a perching bird) - unused by the levels;
  charsheet render showed what it is. game_app_hitchers.cpp: up to 6, one on every other Vimp, on its back (the
  Vimp's rest-pose top x 0.78 above its model origin), turning with it; off when the Vimp runs / is hit / dies or
  the player is within 3.5 m; circles 18 s, lands on the nearest calm Vimp > 12 m from you. birdHitBy also hits
  them. devtest hitchers (Story2 herd 0): two take off when the herd runs from a shot, both land again.
- **Bug found: sounds outside the preloaded list never played.** GameApp loads a fixed list of original sounds at
  start; sfx() of any other name failed silently (GVR_SFX_TRACE: "cr_caw2 MISSING"). A scan of the sources found
  ~15 such names in use: the birds' caws and death squawks, bi_watr1 (shots / stones into water, the fish), Kabuto's
  ka_bite1/2, ka_grnd1, ka_throw, me_flare, me_gtfr1 / me_rtfr1 / ph_enter (base), me_jtlp1, of_eat1 / of_grow /
  of_hatch (Kabuto's powers), sr_fswd1 (climbing). Fix: GameApp::ensureSound loads any sound from the archives the
  first time it is played (a negative cache for names that don't exist; logged once). Trace after: cr_caw2 and
  cr_deat2 "loaded on first use", played.
- Sea monster: xx_seamonster's RM_seamonster (LODs L1-L6; L1 used: 13 x 7 x 30 m) with RM_swim / RM_swimB /
  Rm_fire_turn (6.8 s, rearing up out of the water) and SE_FIRST / SE_ROAR1 / SE_SPLSH / SE_FIRND - no level places
  it. game_app_seamonster.cpp: every 80-160 s first, then 3-5.5 min, it picks deep water (> 7 m along a 60 m line)
  35-65 m ahead of the eyes, surfaces (2.5 s), swims across at 7 m/s (7 s), rears (the clip, splashes at 15/62/85 %,
  a roar), swims 4 s, dives (3 s). Drawn in the underwater hook so the submerged body shows through the water.
  Sounds are loud enough to carry (the mixer's 1/d undone: volume x d/6, capped). devtest sea_monster.
- Leftover bones: xx_bones' Bone_femur / Bone_rib / Bone_spine are 4 m / 9 m / 2.6 m (scenery-sized) and unused;
  spawnBones() makes them Gibs scaled to 0.45 / 0.35 / 0.3 m: 2-3 out of Kabuto's mouth after eating a Smartie or a
  Reaper (not a Vimp: there's meat), 3 where the Cleaners finish a body. kabuto_body_eat: "Bones: 3 left".
- Storm lightning now shows the original's own bolt: xx_efx EFX_lightning_1.gbs (a 226 m tall additive card,
  texture EFX_lightning_1; its rain-box part skipped), unused by the levels; drawn 150-400 m off in front during
  the flicker, turned to face you, the thunder's delay matching. Harness Kstory5: the bolt at the brightest frame.

## 2026-10-26 (evening): Delphi dives
- Player: `diveDepth` lowers the water's floor below the floating depth (the swimming code is otherwise unchanged).
  game_app_dive.cpp sets it for Delphi: stick forward x the view's pitch (beyond 14 degrees) x 1.2 swim speed, plus
  the arm strokes' vertical part (strokeDive_, a pull up = down); idle drift up 0.25 m/s; kept 0.3 m above the bed,
  at most 40 m.
- Under (eye below the surface): fog 0.5..18-28 m in the deep water's colour (darker deeper), sun dimmed, the sky
  haze off (fogMix uses the fog colour), and a new uUnder shader flag: no sky dome (the haze instead) and no
  "depth below the surface" tint (that tint fakes water seen from above; from inside, everything below the surface
  was painted flat water colour - Delphi's own arms included). Mixer: setMuffle (a glided one-pole low-pass on
  the master, ~460 Hz fully under). Bubbles from her mouth every 0.6-2 s. The env changes are undone at the next
  updateSun before anything else (as the weather light) so a level's base light is never captured hazed.
- Found: the original's terrain mesh ends a little off the coast - under the open sea there is no bed, only the
  haze; the terrain's edge shows as a straight drop-off where it ends.
- devtest delphi_dive (Rstory1 deep water): "Delphi dives (the bed 3.8 m below the surface)", "Under the water".
  ASan with audio 0 reports; sweep 56/56; swim_strokes / fish_catch / weather_rain unchanged.
- Caustics: everything under the sea level (rework look) gets moving bright lines - two sine interference fields,
  pow 8 - of sunlight, x exp(-depth/5 m) x shadow; visible in the clear shallows from above and on a diver's arms
  and the bed below. Only for fragments under the surface (the shallows' cost; deep water is opaque from above).
- Underwater look, second pass: the surface seen from below is a bright rippling ceiling (the sky through Snell's
  window straight up, the haze colour at a slant), and the haze (and the empty deep sea, drawn by the sky dome's
  pass) is lighter looking up, darker looking down.
- Shaken palms: WorldObject::shake (decays 0.6/s) widens a tree's wind swing (x1 + 2.5 shake) and jitters its clock
  (DrawParams::swayPhase: a fast shiver); set by blasts (6 + 4 x size m) and by Kabuto walking (1.3 x his height,
  with his speed). scorch_marks: "Trees: 3 shaking"; GVR_SHAKE_TRACE logs the nearest tree to Kabuto.
- Speed wind: the procedural wind loop played 2D at pitch 1.35, volume with speed (6 -> 22 m/s, at most 0.7; a
  giant's speed divided by sqrt of his scale; the Reaperski's speed counts) - the rush of air in a jetpack dive or
  a fall. Harness Story1 jetpack flight: audio rms 0.029 -> 0.045 in the fast part.
- Fish for divers: with Delphi, schools also go in water up to 14 m deep, swimming 1-5 m down where it is over
  4 m deep; a still hand draws its school to the hand's own depth. delphi_dive: a school 4.2 m deep at 3 m down.
- Night plankton: sparks (up to 400, glow texture, additive, unlit, no fog) at night >= 0.35 with AMBIENCE on: 14 + 30 x size per new splash, dt x (25 + 30 v) per frame while wading/swimming faster than 0.3 m/s, and behind fleeing fish; 0-12 cm under the surface, 0.8-2.2 s life. First tries were invisible: placed up to 25 cm down they sat under the bed of the 10-20 cm shallows and the terrain's depth test hid them (fixed: 12 cm at most, cyan 30/170/200 x tint 2.5). GVR_PLANKTON_TRACE logs the count (night 1.0: 13-58 sparks wading).
- Hands in the water (updateHandWater, game_app_water.cpp): per hand, in = under the surface (+-2 cm hysteresis) over water (bed below the surface). Entering faster than 1.6 m/s down (/ sqrt of hero scale) = a slap: waterSplash (0.18-0.48 x scale), ms_spla1/2, haptic 0.5-0.9; slower = a dip: ring 0.22 m+, bi_watr1 at pitch 1.6, haptic 0.15. Dragged within 35 cm of the surface faster than 0.4 m/s: rings every 0.3 m of travel, spray + drops above 1.4 m/s, a swish (bi_watr1 at pitch 0.75, halved while swimming) above 1 m/s, a 0.06-0.14 haptic hum. Out: a ring, 2-6 drops, then drips for 0.6-1.8 s. Plankton bursts round a moving hand at night. Harness hand_water: dip, slap (5.2 m/s), sounds in the --audio-out mix; ASan clean; sweep 56/56.
- Fish for a Smartie: a held fish within 0.45 m of a live Smartie's chest (pos + 0.5 m), not Kabuto, not during the fear: VillagerDirector::touched(.., highFive=true) (the dance), smartieLaugh (factored out of the pat code), the fish gone. Test hook GVR_FISH_TEST=gift (a fish in the right hand once the grip is held). smartie_fish: "Smartie 5: takes the fish" - log only: the right hand holds the gun, so the fish isn't clearly seen being handed over. Sweep 56/56; hand_water, fish_catch, smartie_pat unchanged.

## 2026-10-26 evening — comprehensive device run (20261026-204701, APK fccc02d) and its fixes
- 116/116 runs on the Quest finished, 0 crashes, 0 errors. Frame rate is the problem: the rework's look costs a median
  26 ms of GPU a frame (classic: 9.9 ms in the same village test). Not caused by today's additions (4514eda vs fccc02d:
  0-2 ms). B19; experiment set devtest/perf to price each feature next.
- A first reading blamed a regression since 4514eda: wrong - four tests had the system shell drawing alongside the game
  (VrApi lines of com.oculus.vrshell at 40-70 fps, 12-36 ms GPU), which doubled their GPU figures. Detection is now in
  devtest.ps1 (B20). Lesson: compare only tests without another drawing process.
- Functional checks against each script's expected log lines found 7 misses on the headset; four were frame-rate
  bugs in the game (bird hits, fish scare, Lobird grip flicker, a test target dodging) and four fragile scripts. The
  harness got `--hz <rate>` so scripts run at the headset's 24 fps; all fixed tests pass at 18/24/72 Hz.
- Bird hits: WeaponSystem::onSegment(from, to, dir) reports each projectile's path as it moves (before, birds looked
  at projectiles once a frame and a fast round could spawn and hit the ground between looks).

### 2026-10-27 — the whole test (harness verified; not yet run on the headset)
- The user asked for "a complete whole test" covering mechanics, cutscenes, progression, NPCs and weapons, started
  with one command. `devtest.ps1 -All` runs tests, missions, weapons, npcs, ui, cutscenes and campaign (191
  scripts), then the sweep: about 5 h 20 min. `-Skip campaign,sweep` and `-Set a,b` select sets.
- devtest.ps1 now streams the app's log to a file during each test (`adb logcat` in the background), because the
  55-minute campaign outgrows the 16 MB log buffer. It also prints the battery level before each test, waits to
  charge below 15 %, prints the planned length, and adds a done/total line per set to summary.txt. The PowerShell
  is reviewed by reading only (it can't run here).
- Script commands: `autoaim r|l t0 t1`, `complete t` (ab52691), `title` (start at the main menu).
  Test hooks: GVR_AUTOCOMPLETE=<s> (each mission completed after s seconds of play: the campaign run),
  GVR_AUTOAIM_VIMPS. The auto-aim no longer picks buried Rippers.
- Fixes: the Reaperski autopilot's stuck test (reversed for 1 km after a bump); the options menu opening from
  the main menu with the trigger still held closed at once (menuActHeld_ now set on opening).
- Kabuto's gates per mechanic (harness): giantwall1/2 by punches from 12 m (k1a, k1b, kstory3_1/2); giantwall3
  needs adrenaline (both grips when full; k2a, kstory4_2, kstory5_1/2/3); giantwall4 needs the glass (kstory3_3,
  kstory4_1). Bosses: GVR_KB_FACE_BOSS; kstory5_4's boss was 171 m behind the start without it.
- Development plan written: docs/ROADMAP.md (performance first, then the bugs this suite finds, comfort with the
  user, fidelity, content, infrastructure).

### 2026-10-28 — the whole test ran on the Quest (20261027-191542, APK bac967b)
- 247/247 runs finished in 248 min, 0 crashes, 0 errors. The campaign played all 55 missions to the_end in one
  session on the headset; memory 268 -> ~350 MB and flat (the harness soak had shown 174 -> 436 MB).
- Frame rate: median 71.8 fps (was 28 on 2026-10-26); 3 runs under 60. B19 largely resolved; the rest are
  effect-heavy moments.
- All 7 event misses explained: 6 stale scripts on the PC (my delivery gap - the fae230d fixes never reached
  `devtest\tests` there; CLAUDE.md now says to deliver every changed script with an APK) and one Guardian
  interruption. Found with a new harness option, `--frame-times`, which replays a device run's frame times:
  the current scripts pass with them, so the headset's uneven frames were not the cause.
- devtest.ps1: "FOCUS LOST" (session left FOCUSED before the script finished) flags and reruns a test; the
  per-set totals count each test once; the other-app check needs 6+ s (the shell draws 2-5 s round every
  test's start and end - 54 false flags).

### 2026-10-28 — after the whole test: point-blank Rippers, effect cost, the moored Reaperski
- Point blank: CreatureDirector::raycast's slab test treated a ray starting inside the box as a miss; a Ripper's box
  reaches 1.1 m in front of it, so the muzzle was often inside. Found with GVR_SHOT_TRACE (what each shot meets).
- Effect cost, measured: GVR_FX_COST sums each blended effect's screen cover (size/distance squared over the view)
  per kind. The headset's slow moments were smoke/dust (pop-up bombs: ~70 screens in one frame) and water
  (splash crowns, rings and spray at the player's feet: ~12). Smoke now has a budget (5/4/3/2.5 screens by quality
  level): over it, big puffs are thinned by a stable per-particle seed and the survivors' alpha raised to
  1-(1-a)^(1/keep) so the cloud keeps its density. The Ripper fight's 22-25 ms on the headset was the water case
  (the old script backed into the sea), not the fight.
- Reaperski: mountJetski(const Vec3& pos, ..) was called with ski_.pos and reset ski_ before reading it - getting
  on a moored ski put Delphi at (0, 0). Found by the new moored-ski autopilot test.
- Jet-ski "hop" rule: at speed it climbed any face in small steps (a hill to 38 m); now only up to 8 m above the
  sea (race 3's line crosses a 5.7 m islet).

### 2026-10-28 (afternoon) — Reaper guns, fire arrows, the wrist display, crash reports, the mic roar
- Picked-up Reaper guns fire (trigger on a held gib with rpKind): the spear through weapon_.spawnShot (style 3),
  the bazooka through rockets_. A held Reaper gun gates the hero's own gun and Delphi's bolt.
- Fire arrows: spells are off while a bow is out, so the light comes from a flame (fireNear) or 0.4 s of the bow
  hand's trigger; style 4 shots stick and start a blaze (startBlaze/updateBlazes).
- Wrist display: drawn at render time; its hold timer advances only on the first view of a frame, so the harness
  needs `--video <dir> 1` (every frame rendered) to see it - same for the fx-cost lines.
- Crash handler: sigaltstack + _Unwind_Backtrace + dladdr, async-signal-safe writes, then the previous handler
  (debuggerd still makes its tombstone). LOGI now feeds a 160-line ring for the report.
- Mic roar: AAudio mono float input, RMS in the callback (fast attack, slow release), opened only while the option
  is on, the hero is Kabuto and a mission is playing; RECORD_AUDIO asked once at run time (JNI
  checkSelfPermission/requestPermissions). Never opened in scripted runs; GVR_MIC_TEST stands in. Kabuto's own
  roar can't retrigger it (6 s roar cool-down).

### 2026-10-28 (evening) — GPU pass timer, base table map, teleport, seated, counters, NPC fixes
- GPU pass timer: GL_EXT_disjoint_timer_query timestamps only at the edges of whole framebuffer passes (shadow,
  haze cube, reflection, scope, cinema screen) and of the frame. Inside the eye pass a mark wouldn't split a
  tiler's work usefully, so "eyes" is the remainder; its parts are still priced with GVR_LOOK_OFF.
- Bush poke never landed: the Reaper walked until closer than 1.6 m, but bodies keep ~1.6 m apart - he walked on
  the spot. And the punch rule fired on a hidden player he hadn't seen through. Both fixed.
- Smarties' cheer reached every village on the map; now 90 m round the player.
- Base table map: the real building models drawn at 1:330 on a glass table left of the build signs; a free site's
  marker is touchable. The base spans ~180 m (walls at 86 m), so the miniatures are small.
- Kabuto feed autopilot: 50 -> 70 % in ~15 s in k1a; two meals behind a house were passed over (2 s no headway).
- Parry: the blade is swept from its last frame's place. The 18 Hz miss of delphi_parry is aim spread, measured:
  bolts passed 0.14-0.6 m from the blade.
- Seated: the tracking floor maps 0.45 m above the hero's feet (first person returns early in placeCamera - the lift
  had to go there too; found by comparing the eye height of the same script).
- Teleport: a ballistic arc (9 m/s x sqrt(scale)) from the move hand, landing by worldRay_; valid when the surface
  normal is up (> 0.75), not deep water, within 14 m x scale. Fade 0.08 s out, 0.15 s in.
- rstory2_jetski_3 (B21): a water map (terrain < -0.4 m) shows the moored ski's lagoon enclosed; a minimax path
  search over the map puts the lowest land crossing at 27 m.
- Memory eviction (B15) not done: the headset's campaign run stayed flat at ~350 MB; evicting the shared cache risks
  pointers kept across worlds for little gain.

### 2026-10-28 (night) — difficulty, slots, unlocks, hands, speed; the first 36 Hz run
- The first run of every script at 36 Hz (frame-rate independence) found one real bug: switching from the gun to
  the bush pack, a grenade or a picked-up Reaper gun left the aim laser drawn where it last was (laserOn_ was only
  updated on the gun's path). The same test's autoaim aimed from the controller, but past the arm's reach the drawn
  hand - and the Reaper gun in it - stops short of the controller: it now aims from the held gun.
- Water reflection: a second, half-size target used at quality 1 (sampling is by projected UV, size-independent).
- Shadow map reuse: the sampling matrix is the one it was drawn with, so a reused map stays put.
- The terrain far-ring change made no visible difference at full quality (9 pixels, <= 2 levels).
  Note: harness renders of 1a's village start at quality 2 (the per-world table): GVR_QUALITY=0 to see level 0.
- Save slots: slot 1 is the old save.txt (old checkpoints keep working); NEW GAME clears only the chosen slot.
- Unlocks: per hero story, up to the first unfinished mission; scripted device runs see every mission.
- Capacitive touch: if the runtime refuses the bindings with the touch paths, they are suggested again without
  them (a refused suggestion would have left the app with no input at all).

### 2026-10-28 (late) — an independent review of the day's changes, checkpoints, best times, menu scroll
- A separate agent reviewed dc94e76..HEAD without seeing my reasoning. Real defects it found and that are fixed
  (18140e9): the microphone stream outlived its object at app exit (callback into a dead stack object), stayed open
  while paused, had no error callback and was retried every frame on failure; arrows were credited to the bow in
  hand when they landed (switching bows mid-flight, the power-up bow's 5-arrow fan returning 5 for 1, Reaper spear
  bolts refilling a bow); seated mode doubled the lift in the third-person camera collision and broke the head
  gestures (leap never, ground pound always); the wrist display's hold ran at 1/72 s per drawn frame; mission
  numbers showed on every later loading screen; a slot switch kept the old mission list; blazes and a teleport
  blink carried into the next mission; fire arrows lit from their own flame's puffs at once.
- Not changed (judged acceptable): the crash handler calls dladdr/snprintf in the signal handler (not strictly
  async-signal-safe; a crash inside dlopen could deadlock instead of writing a report); progress.txt (unlocks) is
  shared by the three save slots; the victory stinger can play before a battle's ending phrase.
- Reach-walk survey over 49 missions (GVR_REACH_WALK, 420 s each at 24 Hz): the flood fill finds every reach
  objective reachable but the walk autopilot arrives only in 4 (k1b, r1_clusterspell, story3_borjoyzee_2 and one
  objective of others) - it gets stuck climbing out of water at shores and on slopes. An autopilot limit (the
  Meccaryn would jet), not a mission bug found; left as is.
- Checkpoints: dying returns you to where the last objective was done. Best time per mission in progress.txt.
- Options menu: 26 rows stood ~1.9 m tall; it now shows 16 around the choice.

### 2026-10-28 (late evening) — practice, back to the main menu, pointing, bare hands, the split
- PRACTICE reuses what was there: intro_island (the title's world) has one mission with no objectives, so the
  existing test waves (three Reapers ~35 m out whenever none are alive) already make a range. The hero is forced
  in loadMission (practiceHero_), the kit is every item the hero's story missions hand out (Kabuto: none - kstory5_5's
  guns are the Meccaryn's), the opening cutscene is skipped, and writeCheckpoint / markMissionDone /
  recordMissionStats return early. practice_ follows load_.practice at every world switch, so any later world
  (new game, continue, skirmish, quit) clears it.
- Options -> MAIN MENU: the title is a world like any other - the loading state switches to intro_island with
  title_ set before loadMission (whose title_ early-out skips the checkpoint), then enterTitle. CONTINUE reads the
  checkpoint back from disk (the in-memory kit is dropped: CONTINUE hands out the checkpoint's).
- Dynamic foveation: XR_FB_foveation's dynamic flag makes the given level a ceiling the runtime moves under with GPU
  load. Offered as a fifth FOVEATED RENDERING value; the default stays MEDIUM (measured).
- game_app.cpp (5,200 lines) split mechanically into audio, gore, mission, cutscene, combat and render files by a
  script that moves whole function blocks with the comments above them; a multiset comparison of lines confirmed
  nothing was lost or duplicated, and the full regression is identical.
- Pointing: one ray-panel test (panelRay) for both menus; the main hand wins when both rays are on the panel and
  neither trigger is pulled. hoverRow_ == -2 on opening adopts the row under the pointer without selecting it (the
  harness's resting controllers already point at the main menu's OPTIONS row - selecting it would have broken every
  stick-driven menu test, and on the headset a resting hand would do the same). Found in testing: the rays of both
  hands were on the panel and the left one won; after closing Options the title saw a "new" row under the pointer.
- Bare hands: Meta's aim extension gives a ready aim ray, index pinch and the menu gesture; the platform layer fills
  ControllerState only for a hand whose controller isn't tracked and strips it outside the menus (the hero's hands
  stay controller-driven). Device-only.
- Independent review of 80c96d1 + 9cd5b0f (a separate agent, read-only). Fixed: (1) a trigger held from play (or
  from the last hold) could finish NEW GAME / MAIN MENU once the pointer drifted onto them - a hold now counts only
  from a press begun on that row; (2) quitting while riding the Reaperski kept its engine loop and the ski drawn,
  and the jetpack/slide/swim/wind loops and the last world's music played on the main menu - the music set and
  initJetski now run for the title island too, quitToTitle stops the loops and cutscene voices; (3) a quit inside
  the 5.5 s death kept dying_ (death song on the menu, third person saved) - loadMission ends a death in progress;
  (4) the Options window scrolling under a still pointer counted as pointing - hoverRow_ keeps the window slot;
  (5) a press acted on the chosen row while the dot rested on another: kept by design, but made visible - the dot is
  grey off the chosen row, and only moving the dot onto a row chooses it (the highlighted row, the menu's own or the
  stick's, stays the choice until then; the opposite rule made ui_title_newgame pick OPTIONS, where the resting
  controller pointed). Found on the way: before the main menu's view is placed the panel isn't under the rays, so
  the first row seen counted as a move - pointing now starts once the view is set; (6) bare-hand pinch got hysteresis (on 0.8 / the runtime's
  pinch, off 0.4). Checked fine by the review: nothing written in practice, practice_ cleared by every world switch,
  Kabuto's scale on the title, OpenXR struct chaining and tracker lifetime, hit-test maths, side/scroll zones.
- Delphi's spells, more of the original's list (GText iconrshail / iconrstime and the effect models
  EFX_hail_missile, EFX_slow_spell are in the data; how they behave is native code, so ours): SUMMON HAIL - the
  casting hand above the head then brought down faster than 2.5 m/s, trigger held: 16 ice blocks (the model,
  ~3 m long, drawn at 0.55) fall over 2.7 s within 5 m of where she aims, each an ice blast (3 m, 70; no fireball
  or scorch - explode() gained a no-fire flag), 10 s between casts. SLOW TIME - both grips within 0.2 m before the
  chest for 0.5 s, trigger held: Reapers, creatures and the Reapers' shots update at 0.35x for 8 s, 25 s between.
  Owned from Rstory3 / Rstory4, in the battles and in practice.
- Skirmish balance survey (harness, computer players only - GVR_SK_SPECTATE takes the player out and gives his side
  a full set): 12 maps x 15 min. Kabuto's side won every 3-way round by captures (his side has no flag to guard,
  so a capture never waits for its own flag to come home - 14 of 28 captures with one bot against three a side);
  most 2-side rounds didn't finish (kill limit 30; ~15-60 kills per 15 min on the big maps). Changes (ours): kill
  limit 20; Kabuto drops a flag after 60 damage since he took it (4 s before he can take one again) and carries it
  without sprinting at 80 % speed (bots). After: 12 maps x 20 min, 13 rounds finished (was 6); 3-way rounds Blue 1,
  Green 1, Red 1, one map unfinished. The large maps (L3, Lw1) stay slow with bots alone: they rarely meet.
- Weapon wheel (Baz): the right stick held forward 0.4 s (three or more things carried) opens a ring of the original
  HUD icons 0.5 m before the eyes, 18 cm low; the gun hand picks by moving 4 cm or more from where it was (the
  angle chooses the cell; a dot shows the hand's move), letting the stick go takes it. First tried at the hand: it
  sat under the gun model and at the edge of the view. A short push still steps on, now on release. Found in
  testing: the hold timer stopped after one frame (the "armed" flag cleared on the push itself).
- Independent review of the spells / wheel / balance commits (read-only agent). Fixed: (1, high) an overhand
  fire-bolt throw with the trigger let go late cast SUMMON HAIL - hail now wants a drop of 0.4 m or more from the
  raised hand's highest point to below the chin, mostly straight down (sideways travel under 0.6x the drop), and aims
  where she looks (pitch held 10-35 degrees down, at least 7 m out) instead of the chopping hand; tests/
  delphi_throw_overhand checks the throw stays a bolt; (2) the wheel was anchored in the world - walking, the
  jetpack or a turn moved the "hand" - it is kept in tracking space now, and turning is off while it is open or the
  stick is held forward; (3) hail stones, the storm, slow time and the cooldowns carried into the next mission or
  hero - reset in loadMission and setHero; (4) slow time now also slows bazooka rockets and their cooldown, Reaper
  grenades, SAM missiles and the bosses; (5) no spell gestures with a bow in the casting hand (first person: its
  trigger lights fire arrows, and the string hand near the bow looked like "hands together"); (6) gesture motion
  isn't measured across frames when gestures didn't run (dying, riding, the menu); (7) the wheel closes whenever
  the weapon update stops (menu, death, ride). Kept: hail and slow time come with the tornado (the battles, a
  finished base) as intended; GVR_SK_SPECTATE is a harness-only hook.
- Delphi's CLOAK and CLUSTER: r4a_buildbase's item list (decoded earlier, 23 22 25 44 45 46) hands her IconRSCloak and
  IconRSCluster along with the fire wall and her bows - the only story mission that gives spells beyond the bubble
  and fire wall. Rules ours: CLOAK - the casting hand held over the eyes 0.6 s, trigger held: 12 s unseen (the bush
  pack's hidden-player rules: unaware Reapers notice only a dash nearby; aware ones lose her), see-through body
  (renderGhost), ends when she shoots; 30 s between. CLUSTER - a fire bolt let go while the hand flicks up
  (> 2.2 m/s): six bolts in a 50-degree spread, 4 s between. From Rstory4 on and in practice. Found in testing: the
  cloak's timeout never logged (endCloak returned early once the timer was already at zero).
- Baz's Repair Gun: weapondefs.bin has it (item 19 IconRepair, object mc_repairgun; weapon 19: -50 damage every
  1.5 s, range 50, fx fxb_repair / fxb_repair_yourself) and the art is in the archives (MC_repairgun with Ref_handle /
  Ref_nozzle01, EFX_repair_ring, i_repair), and GText has its lines ("You cannot repair a building until it is fully
  built!"), but no mission's item list gives it. Ours: weapon slot 10, carried in the base missions and in practice;
  the beam (green fire along it, the ring at the hit) mends a standing building it points at within 50 m, +50 every
  1.5 s; a building still going up gets the game's own line. Not saved with the checkpoint (handed out per mission).
  "Repair yourself" (the second fx) left out - it would make the injections pointless.
- Homing rockets for Baz's launcher (weapondefs item 15 IconHoming: 10 carried, 6 a pickup; sold in a level's Shop
  once he has the launcher - ours): fired before plain rockets; each turns (7 rad/s blend) toward the nearest live
  Reaper or creature within 60 degrees of its flight and 100 m, then bursts as a rocket. Control run: with plain
  rockets the 35-degree-off Reaper survives, with homing it dies (72 and 24 Hz). The Shop's counter gained a
  sixth item, so ui_shop's reach was moved (the ammo box now stands at 17 degrees).
- Review of 8442401 / 8e9fb9c / 846b3c8 (a read-only reviewer agent; fixed in 5db4e40): (1) HIGH - the cloak only
  cleared `aware`: Reapers already advancing or firing kept seeing her (in range, line of sight still set) and kept
  shooting - the new tests/delphi_cloak_fight measured 21 hits in 12 s before, 0 after. castCloak now drops every
  fighting Reaper back to idle (alert, poke, lost, los cleared) and enemy.cpp never lets an unaware Reaper "see" a
  hidden player (cloak or bush); (2) RPG / homing arrows, sword strikes, hail / tornado / fire-wall casts, riding
  the Reaperski and dying now end the cloak (and riding / dying clear the hidden flag); a Reaper groping for her
  doesn't play the bush sounds and finding her ends it; (3) the repair beam stayed drawn after switching to the bush
  or bombs, riding or a new mission; (4) the Repair Gun stayed in the cycle after the first base mission - now set
  per mission; (5) the base Giftshop didn't sell homing rockets; (6) the cluster flew along the upward flick (60+
  degrees up) - it uses the aim from before the throw blend, and waits for the bolt cooldown; low: homing rockets
  are saved at checkpoints, counted on the HUD counters and the wrist line (which also names the Repair Gun, bush
  and pop-up bomb), get the original FX_homing trail without a doubled smoke puff; a 7-item shop counter closes up
  (at most 3 rad across) instead of wrapping behind him. Not changed: creatures, Vimps, SAMs and bosses ignore the
  cloak; the repair ray has no line-of-sight check (cosmetic).
- Baz's portable turret: weapondefs item 11 (IconTurret1, object 60 mc_turret, "max 1", category 3 gadget; item 12
  IconTurret2 the same object) - no mission's item list (dumped all 56) hands it out. The art: MC_turret.gbs (a gun
  head on a spike, 1.1 m tall at 0.2 m/unit; nodes center / Turret / gun / Ref_handle / Ref_nozzle01, the clip
  static) and i_mecc_turret. Ours: weapon slot 11 (between the Repair Gun and the bush in the switching order, now
  one table kWeaponOrder with selHudLayer / selCount instead of four copies), carried from Story 3 on and in
  practice. In hand by Ref_handle; the trigger plants it where the hand's ray meets the ground (within 6 m, never
  in water), and the hand goes back to the weapon held before (that trigger pull doesn't fire it). Planted, the head
  turns (pose modifier on "center", game-Z) to the nearest Reaper it can see within 40 m (the one it is on keeps it
  unless another is 20 % nearer) and fires 12-damage rounds every 0.45 s from its nozzle (the shot code shared with
  the base turrets: stepTurretShots / drawTurretShots). 60 rounds, then it sinks away and is back in the pack 20 s
  later; a grip squeeze with the hand on its head takes it back at once. Checked: the drawn nozzle (the posed skin's
  Ref_nozzle01) is 0.00 m from where its rounds leave; devtests w_mecc_turret (plant, take back, replant, kills)
  and w_mecc_turret_spent (runs dry, folds, recharges) at 72 and 24 Hz. Reapers don't target it (it can't be hurt).
- Review of 1d70b8b (the turret; read-only reviewer agent), fixed: (1) third person, any grip within 1.6 m took it
  back - the move hand's grip is sprint - and a grip already held when it was planted counted as fresh: the grips'
  state is now tracked every frame, third person takes only the gun hand, first person skips a hand holding a stone,
  a body or a two-handed gun's barrel; (2) planting with grenades as the last weapon cocked one with the trigger still
  held (and the bush put him in it): it goes back to the handgun from those; the trigger block now clears at the top
  of updateWeapon whatever is in hand, and the plant is a fresh pull measured there (it went stale in the launcher /
  bomb branches); (4) a planted turret was lost for the mission once he moved on: more than 60 m away, or a respawn,
  sends it back to the pack (rounds kept), and the weapon strip shows it dimmed while planted / recharging; (5) a
  roof, bridge or walkway the ray meets is planted on (it went to the terrain underneath, inside the building), and
  "stand on the ground" while airborne; (6) it fires on through Baz's death; (7) taking it back stops the machine
  gun's loop and gives the switch feedback. Not changed: it shoots Reapers only (not creatures, bosses, Vimps).
- Delphi's FOLLOW: weapondefs item 30 (IconRSFollow) and clips for its victims - rpman_followspell_caught (2.6 s: a
  Reaper lifted off his feet, toes down) and _loop (hanging limp), also for Rippers, Vimps, Smarties and Meccaryns;
  no mission hands it out. Ours: trigger held, the casting hand pointing at a Reaper (within 7 degrees, 30 m) is pulled
  back toward her (> 1.4 m/s): he is caught (Falling state, as Kabuto's grab, lifted 0.6 m so it doesn't end in a
  landing), floats where her hand points at the distance he was caught at (3-10 m), and follows it (at most 22 m/s);
  letting go throws him on with the swing: he bowls over Reapers he meets (3 x speed) and takes 6 x speed where he
  lands. 6 s at most, 12 s between; Rstory5 and practice; a blue thread of fire from the hand. Found in testing: the
  default left-hand aim in the harness points 17 degrees under a Reaper's middle at 10 m (the script pitches it up).
- Delphi's SUMMON FIRE CIRCLE: weapondefs item 31 (IconRSFireCircle), its effect model EFX_circle_spell (a flared
  column, ~1.1 m round at the foot, 2.9 m at the top, 6.5 m tall, textured with her blue fire; the clip static); no
  mission hands it out. Ours: trigger held, both arms stretched out to the sides (hands over 1.1 m apart, between
  chest and hip) 0.4 s: the column (scaled 1.8 across: a 2 m ring) stands where she is for 8 s, turning; Reaper bolts
  crossing its wall burn up, grenades and bazooka rockets burst in it (not hurting her), whatever stands in the wall
  takes 40 a second; 20 s between; with slow time (story 4 on, the battles, practice). Checked in a frame from
  inside (drawn at 0.45 additive: 0.8 washed the view out). The test run: 14-15 bolts / grenades stopped in 8 s.
- Baz's shield: weapondefs item 17 (IconShield, object 902 mc_shield: a back pack with an antenna, Ref_back /
  Ref_shield), st_mec_shield and textures EFX_shield / 1 / 2; no mission hands it out. EFX_shield.tga turned out
  to be an artist's placeholder (plain red with a scrawled note) - it tinted the whole first-person view magenta in
  the first test frame; EFX_shield1 (a blue-violet energy web) is used instead, on EFX_prx_sphere (a 5.4 m sphere,
  drawn 2.5 m across). Ours: weapon-wheel slot 12, a gadget - chosen, it is raised and the gun stays (the stick's
  steps skip it); 12 s, takes up to 150 of shots / bites / blasts (not falls or drowning), flares where hits land,
  40 s between; Story 4 on and practice. Also: the HUD now uses the original st_mec_repair / st_mec_turret1 icons
  for the Repair Gun and the turret (the st_ art exists; the i_ icons were the inventory screen's).
- Review of e2d07c5 (FOLLOW; read-only reviewer agent), fixed: (1) any hit that didn't kill a Reaper in the air
  (held, thrown, Kabuto's, a knocked-off tower guard) put him in the Hit state - snapped to the ground from any
  height: damage() now leaves a Falling Reaper falling; (2) a hover-pack ambusher could be caught but kept hovering
  (knockOff now clears `dropping`, and he isn't a candidate while dropping; it also clears `postObject`); (3) a
  trigger easing to 0.4-0.6 mid-hold let the other gestures and a charged fire wall / tornado through on release:
  the hold keeps them off every frame; (4) a thrown fire bolt's wind-up could catch a Reaper: the catch now needs the
  Reaper pointed at steadily 0.2 s, a pull of 12 cm or more measured relative to the head (stepping back isn't a
  pull), and no other gesture cast on that pull; (5) caught through walls / dragged through them: line of sight
  for the catch, the hold stops short of what's in the way (thrown bodies can still fly through walls - open);
  (6) trailing up a slope landed him: kept 0.3 m off the ground; (7) a false pull after a gap (menu, death): the
  hand speed resets; (8) dropped into deep sea he walked on the Reapers' invisible shelf: drowned (a splash);
  (10) released on a switch to third person or a bow, and thrown Reapers are watched from stepSpells; (12) the
  enemy update's `landed` / `punches` index lists went stale when corpses were erased the same frame (an older bug):
  carried across by uid; (13) Reapers in the air no longer shove the ones under them or slide down slopes; a fall now
  counts from the top of the arc. The test's pull needed recalibrating (the harness hand's pull reads 1.3-2 m/s).
- Review of e5e8fed / 469ae93 (fire circle, shield; read-only reviewer agent), fixed: (1) HIGH - a grenade or bazooka
  rocket stopped in the circle's wall was marked spent but updateRockets didn't skip spent entries, so next frame it
  burst again - hurting Delphi (hidden by GVR_GOD in the test); the same old fault re-burst rockets fused by the
  Reaperski. updateRockets now skips them (checked without god mode: no blast damage after the bursts); (2) flinging
  the arms out also added up as the low sweep - a fire wall instead of the circle: no sweep while the hands are over
  0.9 m apart; (3) both bubbles surround the first-person eye: the circle's column turned 46 deg/s and the shield
  flickered at ~4 Hz in its last seconds - now a slow drift and a 1.5 Hz pulse (comfort; device check pending);
  (4) the shield picked on the wheel was announced as the gun in hand; (5) the circle's burn went through the gun-hit
  path (could knock a gun from his hand, blood, haptics): a plain burn now; (6) a half-held arms-out pose carried
  over a trigger release / gap; (7) bolts jumping the 1.2 m band at low frame rates: a crossing test; (8) the circle
  is put on the ground if cast mid-jump; (9) the shield's 150 counts the hit as the difficulty makes it, and an
  absorbed hit buzzes the hand; (10) GVR_RAISE_SHIELD counts mission time (it used the app clock); (11) the shield
  recharges through Baz's death.
- 2026-10-29: Delphi's spells on her wrist HUD (the data's st_rp_hail / slowtime / cloak / cluster / follow / circle /
  tornado icons): lit when ready, faint while recharging, pulsing while one works - the reviews' "no feedback while
  a gesture recharges". The jetpack upgrade (weapondefs item 18 IconJetpack2, "Jetpack Upgrade", object 892
  mc_jetpack_2; no mission hands it out - ours: Story 5 on and practice): an 11 s tank (8) and 14.5 m/s flight (12),
  set every mission; "Jetpack burnt dry" now logs the tank size (devtest weapons/w_mecc_jetpack2, 72 / 24 Hz).
- Proximity rockets (weapondefs item 16 IconProximity "Proximity Missile Launcher": 25 carried, 15 a pickup; model
  mc_rocket_proximity, 624 tris; no mission hands them out - ours: the Shops from Story 4 on, the base Giftshop,
  practice): rocket kind 10, bursts (as a rocket: 4 m, 120) when its step passes within 3 m of a live Reaper or
  creature - tested over the frame's whole step (at 24 Hz a single-point test missed). With three loads the launcher
  no longer fires homing rockets first by itself: the load is chosen on the weapon wheel (cells 1 / 13 / 14 - plain,
  homing tinted green, proximity orange; the stick's steps skip 13 / 14), bought rockets are loaded at once, an empty
  load falls to the next with any ("NOW FIRING ..."). Saved at checkpoints. Devtest weapons/w_mecc_prox (the same shot
  with a plain rocket flies on and bursts ~50 m beyond).
- 2026-10-29, the data's own sounds for the new items (all resolve - no "unavailable" in the runs): the Repair Gun
  me_repst / me_replp (loop while the beam is on) / me_repnd (stopped on a switch, death, riding, a new mission);
  homing rockets me_homsh / me_homex; proximity rockets me_prxsh / me_prxex1; the turret me_place + me_trrbd when
  planted, me_trrdn when it folds; the shield me_sld1 / me_slden. (There are gyrocopter sounds too - me_gyin / gyon
  / gylp / gyrev / gylnd / gyout / gydmg - and a flyable Gyro_l0 with a seat, joystick and prop clip, built by the
  Gyrocopter Pad base building, s_mecc_gpad: not placed by any mission; a candidate.)
- Review of a548755 / c20ba4e / b66bcd2 (read-only reviewer agent), fixed: a proximity rocket armed 2.7-3.6 m out
  could burst at Baz's elbow with a Reaper beside him (4 m blast): armed only beyond 5 m from him, and it bursts at
  the closest point of its step; bosses (in chars_, not enemies_) now trip the proximity fuse and draw homing
  rockets; a load that runs out moves on to the next at once (the counters read 0 with rockets left); after
  CONTINUE the Shop's homing / proximity items were decided before the launcher came back from the checkpoint - now
  after the restore; GVR_WEAPON 13 / 14 map to the launcher's loads; the wrist line names the load and the others;
  the wheel widens past 12 cells; Delphi's spell icons sit in two rows within the plate, the tornado's pulses while
  one turns.
- Baz's gyrocopter (2026-10-29): the data has a flyable one - Gyro_l0 (1273 tris; nodes Ref_seat, Ref_nozzle, joystick,
  a prop whose 0.3 s clip spins it), gyro_breathe / gyro_scan (the gyrocopter with a Meccaryn seated in it: its
  Meccaryn bones pose Baz's own model as the pilot), the Gyrocopter Pad base building (s_mecc_gpad), its sounds
  (me_gyin / gyon / gylp / gylnd / gyout / gydmg) and lines ("Gyropad ready!"); no mission places one. Ours: parked
  9 m ahead of the start in practice (Baz). In: a fresh grip squeeze within 3.5 m. Flying (ours): left stick forward /
  back (22 / 6 m/s) and turn (1.3 rad/s), right stick climb / descend (6 m/s), up to 90 m over the ground or the sea's
  surface (over water it skims, never sinks); or the joystick - the right grip held, the controller's pitch and roll
  from where it was taken; its guns on the right trigger along its heading and the head's pitch; walls stop it (a
  fast one hurts); the engine loop's pitch follows the speed (AudioMixer::setPitch, new). Level horizon (the model
  banks and pitches, the view only turns with it). First person the eyes are pinned at the pilot's (the seat + 0.55 m)
  whatever the user's real height; riding_ covers both vehicles (onSki() for the Reaperski's own code). Found in
  testing: off the land the floor lookup failed and the ceiling clamp pulled it down 10 m a frame; without
  placeCamera in the ride the view stayed at the start. Devtest weapons/w_mecc_gyro (in, 20 m up, 130 m out over the
  sea, down, out; 72 / 24 Hz); frames checked (third person: the pilot in his seat; first person: the cockpit).
  The Gyrocopter Pad in the bases: not yet.
- The Gyrocopter Pad in Baz's base (s_mecc_gpad on the template's free "teleport" slot; 8 meat, 20 s; ours): built,
  a gyrocopter stands on its top and a Smartie says the data's "Gyropad ready!" (smchgyropaddone). The pad's mesh
  starts 2.5 m above its origin - lowered onto the ground; its top (4.3 m) is the gyrocopter's floor there. Devtest
  npcs/npc_base_gyropad (GVR_BASE_ORDER now also takes one building's key); frame checked.
- Review of b68f19d (the gyrocopter; read-only reviewer agent), fixed: (1) the boarding line never timed out in
  flight (the line's timer only ran on foot - the Reaperski's handlebars hint too); fixing it would have shown that
  hint for Baz's gyrocopter and marked it shown: the hint now tests onSki(); (2) first person the eye was pinned
  every frame - leaning and ducking didn't move it, and it took the model's bank / tilt (sliding the eye 0.3-0.5 m):
  now from its position and heading, plus the head's movement since boarding, less the seated lift; (3) in flight the
  turret, shield, mines and the base stopped (the riding branch returned before them): they run there now, and Timmy
  gets off; (4) left in the air (the pilot dead), it hung there for the rest of the mission: a parked gyrocopter
  comes down on its own; (5) any fresh grip within 3.5 m boarded (sprinting, a holster draw): first person a hand
  within 1.8 m of the seat, third person the gun hand only; the machine gun's loop stops on boarding; (6) Reapers
  punched him at any height (horizontal distance only): not over 2 m apart vertically; (7) the engine and Repair Gun
  loops played on through quit-to-menu, cutscenes and the free camera: stopVehicleLoops(); (8) a crash that killed
  him kept running the ride; (9) getting out used teleport (onto the terrain under a pad, a free refuel): set beside
  it at its floor, and refused over the sea; (10) first person the seated body showed its head and bent its arms to
  stale targets; (11) the view turns with it in snap steps under snap turning, and boards with the gyrocopter's
  forward where the user is looking; the holster draw doesn't fire on the joystick grip; a Reaper gun picked up with
  the beam on stops the repair loop; a wrecked pad drops its gyrocopter. Open: no turn-rate vignette in flight; the
  gyrocopter casts no shadow.
- Comfort in vehicles (the gyrocopter review's open point): the vignette now also closes with the view's turn rate
  in a vehicle (snap steps aside) and the gyrocopter's climb / descent - checked with GVR_VIGNETTE_TRACE (0.53-0.55 at
  the low setting while turning and climbing). The gyrocopter casts a shadow.
- 2026-10-29: ASan/UBSan (build/asan) over the 20 devtests added since 2026-10-28 (turret, shield, rockets, gyrocopter,
  jetpack, Repair Gun, wheels, FOLLOW, fire circle, cloak, cluster, hail, slow time, the gyro pad, practice) at 36 Hz:
  0 reports, all exit 0, the features exercised (checked in the logs). Options -> KABUTO VIEW: GIANT / LOWER (the view
  scale x0.6 - his chest's height; the roadmap's "giant's view" setting); "View scale N" logged as it settles
  (devtest tests/kabuto_view_lower: 5.0 against 8.1-8.4).
- Review of e9c45a9 / 400892b / e37614c (read-only reviewer agent), fixed: (1) a Gyrocopter Pad rebuilt after a wreck
  left its parked gyrocopter on the ground inside the new pad's mesh (it couldn't be boarded again): it goes back on
  top, or one in the air gets its landing pad back; (2) a cutscene in flight stopped the engine loop for good (only
  boarding started it): it restarts in the ride; (3) KABUTO VIEW: LOWER shrank the tracking scale, which also made
  his throws 40 % slower and his reach 60 % (hand velocity, grab and pickup radii use the scale): gameScale() divides
  the view factor back out there - the view (and his body as seen) is lower, the giant isn't weaker; (4) changing the
  option in the menu dropped the view metres in one frame on resume: a fade through black then; (5) a header comment
  on the wrong member. Checked fine by the reviewer: placeGyro can't make a second gyrocopter or move a ridden one,
  restart / quit / death / free camera reset it; the vignette's yaw history (wrap, snap steps, teleports); the
  setting's save, load, clamp and menu rows.
- Thrown bodies and walls (open since FOLLOW): a Reaper in the air (thrown by FOLLOW, blown off a tower, knocked off)
  had no building collision - he flew through walls and fell through roofs to the terrain. Falling now sweeps a ray
  at chest height along the horizontal motion (a 25 m/s throw at 24 Hz can't skip a 5 cm wall), stops at the wall,
  bounces back a little and records the impact speed (Enemy::wallHit); FOLLOW's thrown Reaper takes 5 x that speed in
  damage over 6 m/s. One coming down on a roof / ledge (CollisionWorld::groundAt below where he was) stays there,
  posted as a sentry (his body too - the ragdoll and terrain-following would drop him through it); if that roof goes
  (wrecked, burnt: no floor under him) he falls. FOLLOW's hold keeps him above roofs as well as the terrain. Unit test
  testFallingCollision (thin wall at 24 Hz, a hut roof, the hut removed).
- Baz's portable turret shoots Rippers too (it only looked for Reapers): the nearest living Reaper or Ripper in
  range and in sight - not one underground (Buried / Tunnel / Dive), not Vimps. A Ripper's id in the turret's
  target / shot carries a high bit (kTurretCreature); stepTurretShots hits it through thrownHitCreature (gore,
  objectives, sounds). Devtest weapons/w_mecc_turret_rippers (Story2 2c_1_clearrippers by the Ripper barracks):
  planted, "on Ripper 3/4/5/6", four shot.
- Delphi's cloak and Rippers (open since CLOAK): Rippers saw her as ever. CreatureDirector::playerHidden (set while
  cloaked): one only notices her right next to it (2.5 m x its size); one already chasing runs to where it last saw
  her (Creature::lastSeen) and casts about there - burrowers dig back in after loseAfter as when they lose anyone.
  Buried Rippers still feel her steps. Unit tests (in testCreatures): a Ripper in front of her at 15 m never
  notices; a chasing one, she moves 20 m away cloaked - it goes to the old spot and never bites.
- Delphi's SHRINK (the data's item 29 IconRSShrinker, GText "Shrink", HUD art st_rp_shrink; no effect model, no
  rules in the data - its multiplayer line "%k shrunk %v to death"). Ours: FOLLOW's steady aim (trigger held, 0.2 s
  within 7 degrees, 30 m, in sight), then the hand closes - a grip squeeze: he shrinks to a quarter over half a
  second for 15 s (EnemyDirector: Enemy::shrink / shrinkT); small, he runs from her on short legs, never stops to
  fire, can't punch, his hit capsule and height shrink with him, any hit kills him; she can tread on him. A shrunk
  Reaper killed any way is squashed (gibbed at his size, no full-size ragdoll). 14 s between; Rstory4, 5, practice.
  Rendering through GameApp::enemyXf (body, shadow, reflection, his gun). Devtest tests/delphi_shrink (cast at 10 m,
  GVR_SHRINK_CHASE runs her at him, trodden on, squashed) 72 Hz; frames checked (a quarter-size Reaper running; the
  reflection follows); unit test testShrink. Controls card row; a HUD icon in her spell row (8 now: two rows of four).
- r4a's base: the second Hellfire turret site found no level ground since the Gyrocopter Pad took a site (a sweep
  warning): the search goes round to 2.85 rad and 16 m further out.
- Review of d125b40 / 7914447 (read-only reviewer agent), fixed: (1) shrunk Reapers still threw grenades (the
  grenade code counts fleeing Advance as fighting): not while shrunk; (2) SHRINK's grip is also Delphi's run (turbo):
  closing the hand only casts with the stick near centre, and on cooldown it does nothing (it had eaten a charged
  spell with an error buzz) - devtest tests/delphi_shrink_run (stick pushed: "not cast"; stick let go: cast), 72 / 24
  Hz; (3) a thrown Reaper stopped by a wall was hurt again on landing at his throw speed: the landing counts the speed
  he has left; (4) any floor 0.4 m up (a rock, a fence, a step) made a landing Reaper a sentry for good: roofs from
  1.5 m up only; (5) one bounced onto his own tower's platform lost his link to it: re-linked to the tower he stands
  on; (6) Rippers chasing a cloaked Delphi could run to the world's origin (lastSeen unset for a hunter born then, and
  only kept in Chase): set at spawn and in every state; (7) SHRINK's cooldown carried over respawn / missions:
  reset with the other spells; (8) turrets, homing arrows / rockets aimed 1.1 m up at a quarter-size Reaper (rounds
  passing over his head, blood in mid-air): enemyChest() scales with him, the turret's hit radius too; the
  gyrocopter's engine loop isn't asked for every frame if the sound is missing; a header comment. Checked fine by
  the reviewer: uids (never the turret's creature bit, never reused), a fresh Enemy on spawn, slots, objectives
  completed once, FOLLOW's hold over roofs, roofed bodies, raiders unstuck, gameScale's frame order.
- Delphi's SUMMON SEA MONSTER (the data's item 32 IconRSSeaMonster, GText "Summon Sea Monster"; the Reapers' base
  special "Sea Monster Spell"; the serpent's weapon 48: fx_seamonster_fireball, 150 damage; the multiplayer lines "%v
  was blasted by %k's sea monster!" and "killed by her own monster!"). Ours: the casting hand points at deep water
  (over 5 m deep, 12-70 m out, nothing in the way) with the trigger held for 0.4 s, hand low; then a sharp lift (over
  1.4 m/s) to above the head. The unused RM_seamonster (the ambient serpent's model) surfaces there, turns toward the
  nearest Reaper or Ripper within 70 m (not within 9 m of her), comes in until the water under its head is 6 m deep
  or it is 25 m off, rears (Rm_fire_turn) and at 45 % of the clip spits a fireball from its nose node (nose_1),
  lobbed onto the target: rocket kind 11, gravity, fire puffs (the data's FX_seamonster_fireball trail with
  original effects on), a 5 m burst of 150 that burns her too. Five shots or 6 s with nobody in reach, then it
  dives. 45 s between; her battles (as the tornado) and practice; HUD icon st_rp_monster; a controls card row.
  Devtest tests/delphi_sea_monster (Rstory2 battle 1, by the sea, three Reapers on the shore: summoned 67 m out, one
  fireball, all three thrown and killed) 72 / 24 Hz; frames checked (rearing behind them, the fireball's arc, the
  burst). GVR_SPELL_TRACE prints the nearest deep-water bearing and the aim / lift for writing tests.
- Delphi's REPAIR (the data's item 73 IconRSRepair, HUD art st_rp_repair - the Reapers' counterpart of Baz's Repair
  Gun). Ours: with her base up, the casting hand on one of her damaged buildings within 50 m, trigger held 0.5 s:
  the Repair Gun's beam code (updateRepairGun, its building ray now a helper, bldgInRay) in a spell variant - no gun
  loop, a breath of her fire each mend, the original repair ring on the building - while the trigger is held and
  the hand on a building; it uses up the trigger pull (no fire wall on letting go). Devtest tests/delphi_repair
  (r4a, the Persuader taken to 40 %: 160 -> 360 / 400 in 10 s, no fire wall) 72 / 24 Hz.
- Review of d45f7ed / fcab4bb (read-only reviewer agent), fixed: (1) REPAIR took over a fire wall / tornado charged at
  Reapers near her damaged buildings during a raid, and then blocked FOLLOW / SHRINK: it only starts when the pull
  begins on the building (a charge over 0.15 s begun elsewhere blocks it until the trigger is let go) and not through
  a Reaper or Ripper standing in front of it; (2) SUMMON SEA MONSTER could take over hail or the tornado (raising the
  hand after pointing out to sea) or an overhand bolt: the pull must begin aimed at the sea, and the lift must end
  over the head (+0.15 m) and be held still there a quarter second - brought straight down (hail's stroke) it is not
  cast (devtest tests/delphi_sea_monster_hail); (3) no hand speed across a gap of frames (dying, the menu, riding);
  (4) REPAIR's flag outlived a death / a mission (its HUD pulse; a beam restarting without the dwell): reset with the
  spells and on death; the serpent can't be summoned while the ambient one is out (it jumped to the new spot); it
  spits at nobody within 12 m of her (was 9); the mend's haptic pulse goes to the casting hand; the heading picks a
  live Reaper within 80 m. Checked fine: the serpent's reset per mission, every summoned path ends in a dive, the
  turn with the target behind, kind 11 against every other rocket rule, rockets not saved, bldgInRay as before.
- Progress video 20 (2 min 7 s; scripts/video_clips_20.txt, clip scripts trimmed in scripts/video20/; frames checked
  clip by clip before joining): FOLLOW, SHRINK, SUMMON SEA MONSTER, FIRE CIRCLE, HAIL, SLOW TIME, the portable
  turret, the gyrocopter (third person), the shield (third person), proximity rockets, the wrist display, the
  weapon wheel. Dropped after checking: the Reaper-gun clip (the camera inside a Reaper) and the fire-arrow clip (no
  grass on that shore). GVR_NO_HINTS (test hook) keeps the one-time hints out of videos. Found and fixed making it:
  (1) the gyrocopter's rotor is a flat disc with a soft motion-blur texture (gyro_blade: 68 % clear, 31 % partly
  see-through) - alpha-tested, as every alpha texture is, it showed as hard white blades sweeping through the view
  (strobing on the headset): SkinnedModel::setSoftTexture draws that part blended, no depth write, no shadow;
  (2) riding the gyrocopter, the third-person camera sat inside the rotor's disc: 6.5 m back and 2.6 m up then;
  (3) over the sea the third-person camera could dip under the surface: kept above it unless he is under water.
- Delphi's TELEPORT (the data's item 24 IconRSTeleport, GText "Teleport"; no st_rp_ art for it). Ours: FOLLOW's
  pull (1.2 m/s over 12 cm, relative to the head) aimed at open ground instead of a Reaper - she pulls herself
  there. The aim ray must meet ground she could stand on 3-25 m away (normal y over 0.75, not deep water, room
  overhead), held steady 0.3 s (a blue flame marks the spot; the pull drags the aim, so the spot is the steady one);
  the blink is the teleport movement's (fade, move, fade back). A Reaper in FOLLOW's cone takes precedence; on foot,
  not riding, not in races; 5 s between; Reaper story 3 on and practice; the trigger's release is used up. Devtest
  tests/delphi_teleport (7.8 m, 72 / 24 Hz); delphi_follow / delphi_shrink / delphi_shrink_run unchanged (no
  teleports, their casts as before). Controls card row.
- delphi_cloak_fight missed after TELEPORT ("hit 1 times while cloaked"): a Reaper's bolt already in flight landed
  in the frame she cast (the aim marker's particles had shifted the random sequence, so the volleys fell
  differently). The cloak's hit count now starts half a second after the cast (bolts in flight land, as they
  would); GVR_SPELL_TRACE logs each hit while cloaked. 72 / 24 Hz: 0 hits.
- Review of 8d69abd / 7931a72 (read-only reviewer agent), fixed: (1) a blink (TELEPORT or Options teleport) to a
  roof or rock top put her on the terrain under it - inside the building: PlayerController::blinkTo keeps the
  aimed spot's height (the floors take over), and no longer refills the jetpack or clears a hard landing's stun as
  teleport() did; (2) TELEPORT's aimed spot outlived the trigger, a death, a Reaper in the aim: cleared then, and
  the distance re-checked at the cast; (3) a blink begun as she died froze half dark and finished after the
  respawn: reset on death and respawn; (4) TELEPORT's pull against a throw's wind-up from a ground aim: it must
  point down at the ground, stay pointing as it was (within 30 degrees) and be level (the hand not rising over
  0.8 m/s) - devtest tests/delphi_teleport_windup (the hand back and up: "not cast"); a rule against a long
  charge was tried and dropped (a careful aim takes a second); (5) the third-person camera's sea clamp never acted
  (it ran before the collision pull-in, which is what took the eyes under): now after it, unless diving; (6) soft
  parts drawn after the opaque ones, skipped in renderShadowFull too, and the gyrocopter's rotor blur deferred until
  after the characters (renderGyroSoft) so they show through it blurred. Checked fine: texName for every part,
  the reflection pass, getting in / out of the gyrocopter's camera, TELEPORT blocked when dying / in menus /
  cutscenes / riding / third person / climbing / during a blink / in races, the hand's ray can't pass walls, the
  cloak grace's 12 s.
- Delphi's SUMMON SHADOW (the data's item 26 IconRSShadow, GText "Summon Shadow"; nothing else in the data: no
  effect, art or rules). Ours: first person, sword in hand, the right trigger held (unused with the sword), the
  blade driven point-first into the ground within 6 m (the tip going under at over 1.2 m/s; not in water over
  1.2 m deep): a Reaper warrior (rpman_l0 with rpman_stand_scan / rpman_bzka_run / rpman_punch) rises out of dark
  smoke at least 2.5 m out (a first try rose 0.5 m away and filled the view - frames checked), facing away from
  her, drawn near black with a faint violet glow (SkinnedModel::renderGhost). 20 s: it runs at the nearest Reaper
  within 30 m (of it or of her) and strikes, 40 a blow every 0.9 s (the hit a third into the punch, shoving him);
  with nobody to fight it keeps 2.5 m behind her; then it sinks back. The Reapers don't fight it. 45 s between;
  Reaper story 5 and practice. Devtest tests/delphi_shadow (rises, kills one Reaper in three blows and goes for the
  next, 72 / 24 Hz).
- Review of ba48871 (SUMMON SHADOW; read-only reviewer agent), fixed: (1) "point first" was never checked and the
  tip's speed was the world's (walking downhill, a low slash): the blade must point down (within 45 degrees) and
  the tip's speed is taken relative to her - devtest tests/delphi_shadow_slash (a level blade into the ground:
  "not cast"); (2) the deep-water block never fired (the NPCs' ground stops at the sea floor): the terrain itself;
  (3) the cooldown only ran with the sword out: counted always; (4) she died and it fought on, then ran to the
  checkpoint: it sinks when she dies; (5) it struck Reapers above or below it (a ledge, an ambush dropping from the
  sky): within 1.5 m in height only, droppers skipped; (6) nothing kept it out of her body: kept 1.2 m off; a spawn
  pushed out to 2.5 m isn't through a wall; a Reaper it can't get to (no headway in 3 s) is left for 8 s; a failed
  model load sets the cooldown (not retried every frame) and gives no haptics. Checked fine: the right trigger is
  free with the sword drawn, the tip is the drawn blade's, the model across worlds, posing once a frame,
  enemyFootOffset_, objectives counted once.
- ASan/UBSan over every devtest script (186: tests, weapons, ui, npcs, missions) from a snapshot worktree of ba48871:
  all exit 0, no reports. (An earlier attempt compiled from the working tree while headers were being edited - the
  binary would have mixed struct layouts; stopped and redone from the snapshot; CLAUDE.md notes the practice.)
- Controls card in pages (2026-10-29): Delphi's card had grown to 27 rows (Meccaryn 22) - a panel ~1.7 m tall
  filling the view, small packed text. Cards over 16 rows now show 14 at a time with "1/2" in the title; stick
  left / right turns the page (it used to close the card), up / down or any button still closes it. The row tables
  moved to file scope so the input code knows the page count without depending on a drawn frame (the harness draws
  only its output frame - the first try never flipped there). New devtest ui/ui_controls_pages; ui 20/20 (HARNESS).
- Pause on focus loss (2026-10-29): with the headset's system menu or the Guardian set-up open (session VISIBLE,
  not FOCUSED) the game ran on with the controllers zeroed - Reapers could shoot a hero who couldn't move (B20's
  Guardian interruption ran a race on for 40 s this way). InputFrame.focused now carries xr.focused(); in play the
  game opens the options menu (paused, RESUME chosen). Scripts: `hold unfocused t0 t1`. Found while testing it: the
  harness reuses one InputFrame, so inputAt now sets focused = true each frame. New devtest ui/ui_focus_pause (HARNESS;
  BUILD on the headset path - the device's own focus events are untested).
- Reaper weapon models load with the mission's characters, not on the first play frame (a 25-30 ms hitch on the host
  as 1a's opening cutscene ended; GVR_PROF_SPIKES). All sets at 24 Hz: tests 85 + the 3 known, weapons 29, ui 21,
  npcs 12, missions 31.

### 2026-10-29 (evening) — toward "finished": multiplayer, a release review, the checklist
- The user asked for as much as can be done toward a finished game. docs/RELEASE_CHECKLIST.md now defines
  "finished" (content, quality gates, the headset run, the release package) with each item's honest status.
- Multiplayer after the rework: two harnesses still join and agree on positions within ~1 m. The rework's spells and
  fists only hurt the campaign's Reapers and creatures - a tornado passed through skirmish players.
  skirmishHitWhere() now applies the fire wall, fire circle, tornado and punches to them (hits go through the
  existing host-authoritative path). In a skirmish Delphi has her base spells from the start (the bases stand
  from the start) except SLOW TIME (other headsets' time runs on) and the SEA MONSTER (its targets are Reapers).
  Protocol 3: spells are sent as events (a joiner's resent until the host takes them; the host logs them for
  everyone) and drawn on the other headsets without harm there. Online, the options menu no longer pauses (the hero
  idles, the netcode runs: a paused headset was dropped after 15 s); silence timers ignore one long frame.
- Release review (an independent agent, read-only; 9 findings, all fixed): saves and settings written through
  writeFileAtomic (temp, fsync, rename - an I/O error used to rename an empty file over the save); the free camera
  (B: X skips missions, held X deletes the save) only with developer=1 in settings.txt; switchWorld checks a world
  can load before tearing the current one down, a new game deletes the save only once the new world has loaded, and
  a failed load from the main menu's island returns to the main menu; the options menu stops the looping sounds;
  paused time isn't mission time; a cutscene and the mixer wait behind the headset's menu; a trigger held as focus
  returns doesn't press RESUME; the crash handler installs once per process; the sword trail's batch per GameApp.
- Third-party notices (docs/THIRD_PARTY_NOTICES.txt: Apache-2.0 for the OpenXR loader and native_app_glue, stb's
  MIT) go next to the APK.
- Reach objectives (19 in the campaign): every one now walked in play by the autopilot except three with known
  reasons. Changes: Baz jets up banks when stuck (1.3 s holds) and flies up into a place high above the ground; the
  planner's goal is a cell at the box's height (+2 m round it: 1a's 2.3 m box fell between grid points). Found: two
  boxes are far above the ground under them - 4d_fortress (35 m: flown into, as the briefing's JETPACK UPGRADE hint
  suggests) and story3_prison_6 (80 m: on the clifftop). prison_6's briefing reads "Get to the LIFT and ride it up
  to the GIFT SHOP" and we had no lift: **the mission could not be finished by hand**. New: the Guards' lift
  (game_app_lift.cpp) - the strut's rope (lift_strut03, 16 m out, 122 m down), the data's Guard_lift car on it, a
  standing platform in the player controller; standing on it 1 s at the bottom takes you up at 4 m/s (ours: no rule
  for it in the data we read). New set devtest/reach (14 scripts, opt-in: ~2 h on the headset). Not walked: k2d
  (Kabuto; walked earlier, slow and variable), kstory3_4 (the 68-degree climb - Kabuto's climbing, done on the
  headset in B18), rstory2_jetski_3 (B21), mission1 (an unused copy of 1a).
- Briefings read against the game (every mo_* text): one more mechanic missing - story3_launcher_5's horn guards
  ("Once they sound an alert, Guard reinforcements will be on their way. Watch the skies!"). New (game_app_horn.cpp;
  rules ours, the data has Rpm_Horn and RN_HORN.wav): the nearest guard who has seen you blows the horn, 4 s later
  each standing log_launcher_base sends two guards down on hover packs near you; 45 s between alarms, three at most,
  none once the launchers are down. npcs/npc_guard_horn. Everything else the briefings name is in the game (bush
  pack, nitro, bubble spell, regenerating in deep water, ice walls and ski pickups, weak spots, boulders, crystals).
