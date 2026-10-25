# Original data formats — verified notes

Status key: **V** = verified against the user's GOG data by our parsers; **P** = partially
verified; **U** = unverified (taken from external notes).

## GZP archive — V
```
u32 magic 0x6608F101 | u32 indexOffset
@indexOffset: u32 0 | u32 count | count × { u32 storedSize, u32 size, u32 dosDate, u32 offset,
                                            u8 compression, u8 nameLen, char name[nameLen] }
@offset: { u32 storedSize, u32 size, u32 date, u32 compression } + (storedSize-16) payload
compression: 1 = LZSS, 2 = stored
LZSS: 4096-byte window filled with 0x20, write index starts at 0xFEE, flag byte LSB-first
      (1 = literal), reference = b1 | (b2&0xF0)<<4, length = (b2&0x0F)+3
```
Evidence: 5291/5291 entries across 89 archives decompress to the declared size.

## GBS mesh — V
```
u32 0xAA0100BE | u32 flags (1 normals, 2 UVs, 4 RGBs, bit31 maxlit)
u32 nPos | nPos × f32[3]                      (Z-up game units)
[flags&1] u32 nNormDefs | u32 nWords | u16[nWords]
u32 nDisp | u16 posIndex[nDisp] | [flags&1] u16 normIndex[nDisp]
[flags&2] f32 uv[nDisp][2] | [flags&4] u8 rgb[nDisp][3]
u32 nMax | nMax × { i32 vstart, vcount, nstart, ncount, noffset }   (Σvcount == nPos)
u32 nSub | nSub × { char name[32], u32 maxIndex, i32 totalTris, i32 nWords,
                    u16 words[nWords]  -> runs of {count, (a,b,c)×count},
                    i32 vstart, i32 vcount, [flags&2] char tex[32], char tex2[32],
                    f32 falloff, [flags&4] f32 blend, u32 flags,
                    u32 emissive, ambient, diffuse, specular (0x00BBGGRR), f32 power }
```
Evidence: 1561/1561 GBS files parse with strict index checks and zero trailing bytes.
Rendering facts found here:
- Character meshes have all-zero RGB and need dynamic lighting.
- Sub-flag 0x10 appears on opaque meshes, so it is not an additive-blend switch.
- Winding needs no flip after (-x, z, y).
- Baked RGB applies **per sub-object**. All-zero sub-objects (raft planks, canopy tops, palm leaves, characters) need dynamic lighting.
- Skinned static objects (with `.skn` + `.anm`, e.g. smw_* Smartie walls and houses):
  - The GBS positions are distorted. The real shape is the skin evaluated with the ANM default node transforms (keys ignored); verified by smw_curvelow vs smw_curvelow_m.
  - Their RGB is parametric skin data (R = position along the piece, B = 0/254 side), not lighting.
- **Scale:** 1 unit = 0.2 m is provisional. In animated poses this gives Meccaryn 2.17 m,
  Smartie 0.81 m, Reaper 2.26 m and Kabuto 16.2 m. Unconfirmed.
- UV: stored v is used as-is with top-down texture rows; textures look correct (P).

## GTI terrain — V
```
u32 magic 0x9496500N | [N>=2] u32 nChunks + nChunks×0x34
f32 minX, minY, minH, maxH | u32 tilesX, tilesY | f32 stretch | f32 0.5, 0.5 | u32 c1, c2
u8 flags (bit1 RGB per cell, bit2 extra byte) | u8 mat1, mat2, pad | u32 0 | f32 1.0 | char name[32]
cell stream over a (tilesX+2)×(tilesY+2) grid starting at (1,1):
  i8 run; run<0: skip -run cells (wrap at x>=tilesX+1 → x += 2-(tilesX+2), y++)
          run>=0: run+1 × { f32 height, u8 tri, [rgb], [extra] } (wrap x>=tilesX+1 → x=1, y++)
  until y == tilesY+1
vertex (x,y) world position = (minX + (x-1)*stretch, minY + (y-1)*stretch, height)
```
Evidence: 32/32 terrains parse. Story1's decoded lightmap shows the island shapes.
Open questions:
- 5 files have 164–796 unexplained trailing bytes.
- Meaning of the `tri` bits (diagonal choice is currently a guess).
- How the lightmap is applied: averages ~70/255, so MODULATE2X is assumed.

## World definition w_*.bin — P
```
u32 0x1A0002E5 | u32 sectionOffsets[7]
section 0: u32 size | strz header1 ("box01") | strz terrain file ("story1.gti") | records
record: u8 op + fixed payload (table in src/assets/world_bin.cpp); 0xFF ends the stream
```
Verified ops:
- 30 sky texture, 37 sea texture, 38 glow texture (16-char names)
- 52/53/54 ground / wall / wall terrain textures (44-byte payload)
- 41 fog: f32 near, f32 far, u8[3] colour (12 bytes in total)
- 88 water fog (same layout as 41)
- 42 object: u32 type, f32 x, y, z (game units, Z-up), f32 yaw in DEGREES (21 bytes in total)
- 92 scenario: u8 kind, u32 ?, char name[32]: mission files `wm_<name>.bin` (kind 2 = mission)

- Sizes fixed 2026-10-23 against all 33 worlds (each fix verified by the streams then ending exactly on 0xFF):
  - 50: 3 bytes in total (op + u16). Was 6, which swallowed the start of the next record; this fix alone completes
    19 worlds.
  - 35: 17 bytes (a 16-char name, like 29-34 / 36-38).
  - 44: 33 bytes (a 32-char name: "kabuto" in Kstory1/3/4).
  - 108 / 109 / 110 / 111: 65 bytes, two 32-char names. In w_Story2 these are music set pairs: msu/msuend, msb/msbend,
    mwb/mwbend, mlb/mlbend.

Status: **33/33 world streams walk to the 0xFF terminator** (w_menus ends 2 bytes before its stated length). The fog
colour byte order is unverified.

## Object definitions (section 3 of any 0x1A0002E5 .bin) — V
Container: section offset table index 3 → `u32 size; blob[size]`; offsets below are relative to the blob.
```
u32 nMesh,  u32 off[nMesh]   -> {u32 meshId, strz name}
u32 nX,     u32 off[nX]      (0 in every shipped file)
u32 nGroup, u32 off[nGroup]  -> {u32 groupId, group ops..., 0xFF}
u32 nType,  u32 off[nType]   -> {u32 typeId, type ops...}
```
Group ops: 0x51 LOD `{u32 mesh, f32 angle, f32 dist}`; 0x59/0x5A/0x5B `u32 mesh` (special LODs);
0x52 animation `{u8 slot, strz file.anm}` (slot 1 = idle/default, 2 = walk, … per creature);
0x53 +17; 0x54 +22; 0x55 flag; 0x56 +8; 0x57/0x58/0x5C +12; 0x7F/0x80 timed fx `{u32 tick, 5 strz, u32}`.
Type ops: 0x01 `u32 groupId` (usually first, not always: type 210), 0x1E `u8` object class
("ospec": 23 Ripper, 45 Dactyl, 47 Ripper nest, 60 palm tree, 61 Smartie building), 0x17 `f32` default scale.
Other type-op sizes: see `typeOpSize()` in src/assets/object_defs.cpp; walking stops at an op without a fixed size.
Sizes come from the original loader (decompilation used as a factual reference).
Evidence: every shipped file parses (1570 meshes, 799 groups, 807 types). The old heuristic found 629 types and
missed palm trees, Smartie houses and Ripper nests, because their LOD record doesn't follow the mesh name.

## Mission setup stream wm_*.bin — V (structure), P (meanings)
`u32 version = 4`, then `{u8 op; payload}` until op 2. No control flow: the original runs it once at mission start.
| op | payload | meaning |
|----|---------|---------|
| 01 | – | no-op (stream start) | 
| 02 | – | end |
| 03 | u32 type, f32 x,y,z, f32 yaw° | create object (becomes current) |
| 04 | as 03 + f32 pitch°, f32 roll° | create tilted object |
| 05 | u8 | current object +0xE8 (team?) |
| 06 | 12 B | current +0xF4..0xFC |
| 07 | u32 | current +0x174 |
| 08 | u8 | flags (low 4 bits) |
| 09 | f32 | scale |
| 0A | u8 n, n×u32 | AI list |
| 0B | 4×u32 | mission setup ([0] player race: 1 Kabuto, 2 Mecc, 3 Reaper) |
| 0C/0D/0F/10/11/12/21/23 | u32 | mission or AI values |
| 0E | u32 n, n×u32 | starting items |
| 13/15/19 | char[32] | flick (cutscene) names; 15 = start flick |
| 14 | char[32] | current object takes part in this flick |
| 16 | u32 type, u8, u8 | create object mounted on current (push) |
| 17 | – | pop mount |
| 18 | 3×f32 | yaw, pitch, roll of current |
| 1A/1B/26 | 2×u32 | AI / mission values |
| 1C/1D/22/24/25 | – | flags |
| 1E/1F | u32 n, n×i32 | AI lists |
| 20 | u8 | AI value |
Evidence: all 56 shipped mission files walk exactly to EOF. 1a_1_timmy has 343 top-level objects + 1 mounted
(Timmy's jetpack), versus 170 found by the old op-3 scan.

**Mount points (op 16) — V for position, P for the index rule.**
- The first u8 selects a hardpoint: a `Ref_*` node in the parent model's own `.anm`, evaluated at t = 0 and
  transformed by the parent's placement. 255 = the first `Ref_` node; k = the k-th (only 255 and 0 occur in Story1).
- Verified:
  - `guard_tower1.anm` `Ref_guard01` (0.4, 11.3, 67.3) is the crow's nest (1c_2/1d_1 Reaper sentries, mount 255);
  - `mecc_crash1.anm` `Ref_Lhand` (−2.7, 66.5, 18.3) is on the plane's nose (Tel, 1c_1, mount 0); Tel hangs from it
    by his left hand (`mc_hang_onehand_yell`).

### Vertex RGB that is not lighting (B8, provisional)
Some sub-objects (sm_dome_l0, bw_bunker1) carry strongly coloured RGB that decodes (`c*2/255-1`) to near-unit
vectors. Rule used: mean saturation > 0.4 and ≥ 10 % unit vectors → light dynamically instead. Every checked
correct model has saturation < 0.26, except the raft (0.70, no unit vectors).

## SKN skin / ANM animation — V (structure), P (interpolation)
Serialized images; pointers = file offsets. NAM tables `{u16 size, u16 type(4), u32 ptr, name}`.
- SKN object (NAM type 4): `{u32 0, u32 tree, u32 0, u32 nverts, u32 nclusters, u32 clusterPtr[]}`.
  Objects map to GBS max-objects in order.
- SKN cluster: `{u32 0, u32 owner, u32 bone, f32 bbox[6], u32 count, {f32 x,y,z, u32 localIndex}[count]}`.
  Rigid: one bone per vertex, in bone-local game units.
- SKN bone names: `{u16 size, u16 8, u32, u32 0, char name[], u16 kind, u16 boneIndex}`.
- ANM header (pointed to by node+0x0C): `{u32 ?, u32 0xB, u32 0, u32 ptr, i32 start, i32 end,
  u32 ticksPerFrame, u32 fps, ptr, ptr, u32 numNodes, …, path}`. Duration = (end-start)/(tpf*fps).
- ANM node: +0x10 parent ptr; +0x6C pos; +0x78 quaternion (stored x,y,z negated); +0x88 scale;
  +0x98/+0x9C/+0xA0 pos/rot/scale track ptrs.
- Track: `{u8 keyType, u8, u8, u8, u32, u32 count, u32 keyPtr[count]}`. Key types: 4 VEC3_TCB
  (60 B; tick +0, tcb/ease +4, tangents +24/+36, value +48), 7 QUAT_TCB (72 B; tangents +24/+40,
  **value +56**). Other types are assumed (value at +4).
- Node flag word +0x94: bit 0x08 = mirrored (negative determinant). Local = T·R·(−S).
  Verified on the Kabuto left leg and on Smartie.
- World = Wparent × Local. Vertex = BoneWorld × local. Verified exactly (gzptool skincheck).
- Interpolation: linear/slerp between keys. The TCB tangents aren't used yet.

## MusicSet.bin — V (structure)
`u32 nSongs, u32 nPairs, u32 nSets, u32 songOff[nSongs]`, pairs `{char from[16], char to[16]}`, sets
`{char set[16], char song[16]}`; song: `char name[16]`, `char segment[16]...`, `u8 end` (0/1 loop, 2 + `char[16]` chain).
Two songs share one offset (use the next larger offset as the end). Segment `X` = `Music/<prefix>/<X>.raw`, 16-bit
stereo PCM at 22050 Hz with no header. A pair means "leaving segment `from`, enter at `to`"; the target mood is the set
containing `to`'s song.

## GText<Language>.bin — V
`u32 n, u32 keyBytes, u32 valueBytes, {u32 keyOffset, u32 valueOffset}[n]`, key strings, value strings (NUL-terminated).

## Mission triggers — V (from the decompiled mission code, checked against the data)
Mission op 8 low 4 bits: 1 proximity (10 game units of the bounds), 2 death, 3 destination arrival, 4 Vimp-meat proximity.

## Cutscene scripts flk_*.bin — V (structure) / P (some op meanings)
One file per shot; shots chain into a cutscene through the "next shot" name.
- Header: `u32 version=3, u32 totalSize, u32 nDirectors, u32 nActors, u32 nBlocks, u32 nextShotNameOffset` (0 = last shot).
- Records from 0x18: `{u32 op, u32 sizeInDwords, payload}`; strings are file offsets into a pool after the bytecode.
  - op 1 director: word[1] = offset of an embedded ANM image (starts with its u32 size).
  - op 2 actor: `{director, nodeNameOff, actorIndex, objectType}`; director −1 = not on the director (a prop).
  - op 3 attach: `{carrierActorIndex, carrierBoneOff, actorIndex, nameOff}`; the name is the prop's own clip
    (`mcjetpacka1` → `mcjetpacka1.anm`) or its node aligned to the bone (`ref_handle`, `center`).
    Examples: jetpack (254) in Timmy's `ref_lhand`, gun (21) in Baz's `ref_rhand`, Timmy (583) on Baz's `ref_holsterb`,
    lander engine flames (135) on `ref_engine1/2`.
  - op 4 camera `{director, animOff, nodeOff}`; op 0x10 subtitle GText key; op 0x34 soundtrack stream (`f_start`);
    op 5 yield. op 0x25 (named "fade" in the decompilation) never occurs in the 80 shots.
  - op 0x1F `{f32 level, f32 seconds}` is the screen fade (V, from the data, 2026-10-24): 29 uses; every opening
    shot (intro_island_1, story1_start_1, save_sm_1, save_sm_11) begins `{0,0}{1,2}` - snap to black, fade in over
    2 s - and every last shot ends `{0,2}` / `{0,0.5}` / `{0,1}` at its final wait (fade out). Some put two pairs at
    the same moment (`{0,0}{1,2}{0,0}{1,0.25}`); we use the first pair.
  - op 0x0B / 0x0C `{actorIndex}`: show / hide an actor at that moment (V: story1_jetpack_6 hides Timmy-on-Baz's-back
    (actor 6) at the start, then at frame 61 shows him and hides the standing Timmy (actor 2) - the climb onto the back).
  - op 0x27 `{slot, fxNameOff, mode, actorIndex, nodeNameOff}`: start an effect on an actor's node (V, census of all
    545 shots, 2026-10-24: 208 uses in 72 shots; names are effect definitions - fx_shotgun_flk 77, test3 32,
    fx_sm_glow_sapho 26, fx_stone_explode_medium 25, fxb_splash, fx_flick_millenium_spear/bazooka...; nodes are the
    actor's reference nodes - ref_nozzle on carried weapons, ref_lhand, center, tongue3). Rstory1_fire_4a: five
    guards' spears/bazookas each get one at ref_nozzle 0.1 s apart. mode: 1 in 167, 0 in 41 (meaning unknown).
  - op 3's 4th field, when not an .anm, names the prop's own node aligned to the carrier bone (`ref_handle`); for
    static .gbs props it comes from the prop's .anm at rest (V: Rstory1_fire_4a weapons end up in the hands).
  - Full census (545 shots, all parse with our reader): ops 1-7, 9, 0x0B-0x17, 0x19, 0x1B, 0x1F-0x22, 0x25-0x2F,
    0x31, 0x33-0x35 occur; still unknown: 0x0D (one shot), 0x21, 0x26 {f32}, 0x28, 0x29, 0x2A, 0x2D, 0x2E, 0x31, 0x33, 0x35.
  - op 9 `{frames}`: wait. The waits of a shot add up to its director length at 70 frames/s (story1_hatch_dilemma:
    91 + 70 + 210 = 371 frames = 5.3 s). Provisional rate.
  - op 0x2C `{nameOff}`: start a stream at that point (after the preceding waits). The streams are files in
    `Stream/<World>/`, e.g. `f_ms_msfto` = `Stream/Story1/F_MS_MSFTO.wav` (a sound effect), or voice clips
    (`baz006`), presumably in `StreamEnglish/`.
- The director image holds several NAM node tables sharing one header (one per performance tree):
  - actor trees, usually parented to a mover node named after the actor, in varying spellings:
    `Timmy_mover`, `lander_Moover`, `mover_Baz`, `Flack_mover` (actor `flak`), or anonymous (`mover`, `Moover`);
  - a "Default" tree with `Camera01`, `Camera01.Target`, movers and props.
  - Some typed actors have no tree in a given shot; some trees have no typed actor (the Mecc crew: `Baz_mover`,
    `gordon_mover`, 47 nodes = the Meccaryn skeleton).
- Key types seen in shots (tally over all story1/intro shots): position 4 (TCB, value at +48) and **5 (Bezier, value at
  +32, verified: the first key equals the node default in every shot)**; rotation 7 (TCB quat) and 2 (look-at camera,
  no keys); scale 5 (layout differs; ignored).
- Camera01 positions are world coordinates (e.g. `story1_jetpack_1` near Timmy's tower). The opening shots fly the lander
  at about 1400 units altitude toward the island. Target cameras face `Camera01.Target`.
- Soundtrack `Stream/<World>/F_*.wav` (22050 Hz stereo PCM) carries music and dialogue for the whole cutscene
  (F_Start.wav 64.7 s vs 62.2 s of shots).

## Mission starting items (op 0x0E) - readings so far (2026-10-24)
Lists per mission: 1b `1`; 1c/1d `1 3`; 2a `3`; 2b_1 `20`; story3 `1 3` (bunker_3 `1 3 6`, launcher_5 `1 14`,
prison_7 `1 10`); 4a `1 7`; 5a `1 3 5 0 8`; kstory5_5 `1 3 8 0`; Reaper: r1_smartiegrab `45`, r1_turbo `21`,
r1_saveyan `25`, r4a `23 22 25 44 45 46`. **Superseded by weapondefs.bin's item table (below):** the ids are that
table's key-36 item ids. 1 handgun (not the jetpack), 3 shotgun (not the rocket launcher), 5 machine gun, 6 sniper gun,
7 mortar = the "MILLENIUM GUN" of 4a's briefing (its weapon fires mc_lbomb with fx_splash_millenium), 8 grenade,
0 health syringe, 10 pop-up bomb, 14 missile (rocket) launcher, 20 bush pack; Reaper 21-33 spells (21 smartie grab,
22 cluster, 23 cloak, 25 firewall, 33 tornado...), 44 bow, 45 RPG, 46 sniper bow. The earlier guesses that matched
(6, 10, 20) are confirmed; 1 and 3 were wrong.

## weapondefs.bin (loose in Bin/) — V (structure), P (key meanings) (2026-10-24)
`u32 version=1, u32 fileSize`, then records of `{u32 key, u32 type, (type-2) x u32}`: type 2 = flag (no value),
3 = one value, 4 = two, 6 = four. `{key 2, type 2}` ends a record. Strings are offsets to NUL-terminated names in
the pool at the end of the file (from offset 22180 in the GOG file). Parser: `src/assets/weapondefs.cpp`; dump:
`gzptool weapons <Bin>`. 53 weapon records, then one record holding 73 items.
- **Weapon record** (key 1 = weapon id): 3 projectile object type (793 mc_sniper_bullit, 800 mc_rocket, 1114
  mc_shotgun_shell, 808 mc_lbomb, 38 mc_zip...), 10 damage, 13 seconds between shots, 24 projectile speed (game
  units/s), 34 range (game units; 5 and 6 repeat it in most), 9 four floats on blast weapons (first = splash damage),
  22/23/53/29 effect names (projectile, impact, splash, nozzle), 4 repeated flag bits, 57 flag. Meanings read from
  the values: the sniper record (fx_sniper_bullit) has 80 damage every 1.5 s at 2500, the handgun-like fx_gun records
  12 every 0.2 s (16), 12 every 0.5 s (17), 8 every 0.1 s (42, read as the machine gun), the rocket (3) 80 every 1.8 s,
  the shotgun (6, mc_shotgun_shell, fx_nozzle_shotgun) 40 every 1.3 s, the popup bomb (12) 900 with a 300-unit blast.
- **Item** (a run from key 36 = item id to key 37 = flag): 39 the object type held (21 mc_gun_l0, 22 mc_shotgun_l0,
  300 mc_machinegun_l0, 787 mc_snipergun_l0, 790 mc_mortar_l0, 785 mc_launcher_l0, 822 mc_popupbomb, 920
  mc_bushpack...), 41 its pickup object (647 mc_pickup_shells for item 37 "IconShells"), 42 its ammo item, 45 most
  carried, 46 amount given (weapon: with it; ammo item: per pickup), 47 category (1 gun, 2 thrown, 3 gadget/spell, 4
  ammo, 5 consumable, 6 building), 54 race (1 any, 2 Meccaryn, 3 Reaper), 48 HUD icon name. Check: the items' ids
  agree with the missions' op 0x0E lists and their briefings (6 sniper in bunker_3, 10 pop-up in prison_7, 20 bush in
  2b), and 1c-2c, which list item 3, are the missions that place mc_pickup_shells (the shotgun's ammo pickup).

## fx.bin (loose in Bin/; fx_world_dust.bin is byte-identical) — V (structure), P (meanings) (2026-10-24)
`u32 version=1, u32 size-12, u32 count (679)`, then a directory of `count x {NUL-terminated name, u32 offset}` with
offsets from file offset 12, then the effects. Each effect is a byte-coded stream `{u8 op, fixed arguments}` ending
with op 0x02 (the last effect, FX_nozzle_handgun, ends at the end of the file after its 0x07). The argument layout of
all 91 ops (`src/assets/fx_defs.cpp`, `gzptool fx <Bin> [names]`) was derived from the data: with it all 679
effects parse to exactly the next effect's start - a wrong length anywhere desyncs the stream. Arguments are u32,
f32, one byte (0x76, 0x78) or NUL-terminated names.
Readings so far (from values, not confirmed by behaviour):
- `03 {type}` starts a particle emitter of a GameObj type - the efx_* models (794 efx_flash, 815 efx_fire_trail,
  826 efx_smoke_blue, 796 efx_debri1, 818 efx_ref_sphere...); `06` opens its parameters, `07` closes it. `38 {type}`
  is the weather's emitter (839 efx_rain).
- `24 {object type, emitter type, k}`: an emitter riding a game object. With k = 0 the object is a projectile or a
  model part (FX_rocket = 800 mc_rocket with 821 efx_ref_line_back; FX_sniper_bullit = 793 mc_sniper_bullit;
  FXB_bow_RPG = 14 mc_bullet; FX_Lbomb = 808 mc_lbomb with 823 efx_ref_line_up; also nozzles, smartie glows,
  buildings' smoke); k = -1 (0xFFFFFFFF) effects name the same type twice and are self-contained (foot dust
  efx_dust_ball, portal, smoke). 111 of the 679 effects carry one. In such an emitter's block (projectile trails):
  `20 {r, g, b, r, g, b}` colours from/to (rocket 0.4 0.8 0.9 -> 0.3 0.7 0.8; shotgun 1 0.9 0.8 -> 1 0.8 0.6),
  `22 {from, to}` (rocket 2 -> 0.001, shotgun 0.75 -> 0.001), `25 {s}` (rocket 1.2, sniper / shotgun 0.5),
  `26 {x}` (rocket 0.1, homing 0.05, shotgun 0.75, sniper 0.01) - read as a streak behind the projectile: colour,
  width in game units and lifetime (P); 26 unread (we use it as opacity, floored at 0.25). `0a {0, "fx_rocket_fire",
  0}` - n = 0: the effect rides the object itself (the rocket's blue flame efx_bfire_trail, life 3 s, kept while it
  flies - ours). `27 {"effect", u, f}` (fxb_bow_screamer_trail) read as a trail like 5b. Played: `FxPlayer::attach`
  (V: synthetic unit test; harness: the launcher rocket's blue flame and pale streak).
- `0a / 10 / 70 {n, "effect", 0}`: a named sub-effect, several times with rising n inside an emitter whose model is
  a reference shape (efx_ref_sphere, efx_ref_ground_hit) - read as "start this effect at point/frame n" of that shape.
- `2b {sound, sound}`, `2c {sound}`, `87 {sound, sound}` (foot steps), `83`: sound ids, resolved by the sound table
  below: FX_EXPLOSION_ROCKET 1475 = me_mslex, fx_bullet_sound 1786/1787 = bi_hit1/bi_hit2, FX_Lbomb_bounce 1479 =
  me_mtrht, FX_rocket's 2c 1594 = pr_wsroc (the rocket's whoosh).
- `5b / 1d / 5c / 28 / 56 / 57 {"effect", numbers}`: trail-like sub-effects (FX_RKT_Debri leaves FX_RKT_Debri_fire
  every 0.05).
- Emitter parameters (guesses from typical ranges): `05` / `09` / `35` life and size ranges, `08` size growth,
  `0e` gravity (1 for debris), `0f` vertical drift (negative for rising smoke), `12` / `1c` speed ranges,
  `19` bounce, `33` rotation ranges (0-180 degrees), `1b` scale, `04` timing.
- `7a {"effect", 6 floats}` (3599 uses) places weather effects over rectangles in the fx_weather_* entries.
Played behind Options -> EFFECTS: ORIGINAL (TEST) (FxPlayer; explosions, cutscene effects, projectile trails); the
default is still our stand-in particles (B9).

## Sound table (section 2 of the 0x1A0002E5 .bin files) — V (2026-10-24)
At section offset 2: `u32 -1, u32 magic 0x33221102, u32 count, u32 record size (40), u32 pool size`, then
`count x {u32 id, u32 flags, f32[6], u32 ?, u32 name offset}` and the NUL-terminated .wav names; name offsets are
from the section start. 91 files carry one: 775 records, 660 distinct ids, 5 ids naming different files in two
files (sr_swsh1/2 swapped between xx_raik / xx_reaperlev1 / xx_rpqueen). The floats read as volume/distance
settings (typically 25, 100, 0, d, 0, d). Cross-checks: fx.bin's sound ids land on fitting names (rocket explosion
me_mslex, bullet hits bi_hit1/2, the Millenium bomb's bounce me_mtrht), and weapondefs' key 56 pairs are
{ground, water} impact sounds (1536 bi_dirt1 / 1539 bi_watr1 for bullets; 1549 ex_dirts, 1550 ex_dirtl for
blasts). Parser: ObjectRegistry::addSoundTable; `gzptool fx` prints the names.

## Multiplayer data (2026-10-24) — V (what the files hold), P (how the original used it)
- 12 multiplayer worlds `w_M_*.bin/.gzp/.abx`, no `wm_` mission files: M_3WAY_L1/L2/L3/Lw1 (one
  mecc_base_template_1, one rp_base_template_1, one kabutopad: Meccaryns vs Reapers vs Kabuto), M_Mecc_L1/L2/L3/Lw1
  (two mecc_base_template_1), M_Reaper_L1/L2/L3/Lw1 (two rp_base_template_1). Each has `marker` objects (type 679, 14-63
  per map; on M_Mecc_L1 they cluster round each base and the villages between) and 1-5 sm_hatch_l0. All load and render
  in the harness (`gzptool worldobjs <Bin> <World> [all]` lists them).
- GTextEnglish.bin's multiplayer strings: teams 1 Blue/Mecc, 2 Green/Reaper, 3 Red/Kabuto; kills/deaths
  ("Kills: %d   Deaths: %d"), team and individual score limits, flag captures ("%s has captured %s!", returned,
  taken), base options ("first team to complete a base wins", "first to destroy a base wins", starting base level),
  a capture-prevention timer at the start, team damage on/off, TCP host IP/port and join. Kabuto: "Stand on your EAT
  PAD while you eat the flag-Smartie!"; the Comm Center building "protects your build pad and your flag holder".
- Flag assets: the flag holder is a Smartie, smborjoyzee_l0 (type 584, class 18) with Smborjoyzee_flag / _flag_idle1 /
  _flag_kidnap / _flag_fly (carried by a Meccaryn) / _flag_fly_rp (by a Reaper); flags sm_flag1/2/3 (types 952-954,
  animated sm_flag.anm), base_flag_red/blue (835/836), Mcflag (+ Mcflag_flap.anm), kabutopad (1334, kabutopad.anm).
  How the original's game logic used them is native code (not decoded).
- Measured (V): RP_command (the Reaper command centre, on the build pad) is 42 m across and 56 m high with its
  walkable floor about 8 m up (collision profile along the pad) - the pad's centre is inside it. Ours: the flag
  stands at the nearest open spot in front (18 m). The Meccaryn pad (wood_command) is a 3 m pad.
- Ammo pickups (V, weapondefs + GameObj): ammo item -> pickup type -> model: 37 shells -> 647 mc_pickup_shells,
  38 bullet clip -> 648 mc_pickup_clips, 39 sniper clip -> 787 (the sniper gun's own type, mc_snipergun_l0),
  40-43 rockets / homing / proximity / mortar -> 801 mc_pickup_rockets, grenades -> 650 mc_pickup_grenades,
  mines -> 1123 mc_pickup_mines. Unused by the data's items: MC_pickup_sn_Clips, mc_pickup_homing / _proximity /
  _lbombs.
