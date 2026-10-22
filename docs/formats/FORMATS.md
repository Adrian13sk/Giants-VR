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

Status: 9/33 world streams walk cleanly to 0xFF; the other 24 stop at an unknown or mis-sized
opcode (B5). The fog colour byte order is unverified.

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
    op 0x25 fade; op 5 yield.
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
