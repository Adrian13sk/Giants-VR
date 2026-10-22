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
