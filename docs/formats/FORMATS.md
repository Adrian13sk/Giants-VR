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
- **Scale:** 1 unit = 0.3 m is provisional. It gives Smartie 1.23 m, Reaper 3.4 m and
  Kabuto ~26 m tall. This is consistent with "units are feet" but unconfirmed.
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
- 42 object: u32 type, f32 x, y, z, f32 angle (21 bytes in total)

Status: 9/33 world streams walk cleanly to 0xFF; the other 24 stop at an unknown or mis-sized
opcode (B5). The fog colour byte order is unverified.

## Object definitions xx_*.bin section 3 — P (investigation)
- Mesh name table: `{u32 meshId, char name[] NUL-padded to 4 bytes}`.
- LOD entries: `0x51 u32 meshId f32 distance f32 0`.
- Animation entry: `0x52 u8 n strz "x.anm"`.
- Object type: `u32 typeId, u8 1, u32 modelGroupId, property ops…, 0xFF`.
- Type IDs used by world op 42 are found this way (e.g. 1298 → xx_ambient.bin).
- Another block (`02 11 22 33` marker, version 0x0E, …) defines creature types
  (1290 Smartie, 1379 Reaper).
