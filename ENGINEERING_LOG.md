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
