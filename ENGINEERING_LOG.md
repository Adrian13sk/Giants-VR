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
  VEC3_TCB value at +48, QUAT_TCB value at +24. Header `{?, 0xB, 0, ptr, start, end, tpf, fps}`
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
