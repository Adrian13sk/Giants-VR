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

The headless harness (`tools/headless`) drives the identical `GameApp` with scripted poses on
Mesa llvmpipe, so rendering and asset code is verified on every build without a headset.

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
