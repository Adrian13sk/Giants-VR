# Dependencies and licensing

| Component | Version | Source | Licence | Shipped in APK |
|---|---|---|---|---|
| Khronos OpenXR loader for Android | 1.1.53 | github.com/KhronosGroup/OpenXR-SDK-Source releases (AAR) | Apache-2.0 / MIT | yes (`libopenxr_loader.so`) |
| OpenXR headers | release-1.1.53 | github.com/KhronosGroup/OpenXR-SDK | Apache-2.0 | headers only |
| Khronos EGL/GLES headers | NDK r27c copies | Android NDK | MIT/Khronos | headers only |
| stb_image / stb_image_write | commit a7d4c21 | github.com/nothings/stb | MIT / public domain | yes (compiled in) |
| android_native_app_glue | NDK r27c | Android NDK | Apache-2.0 | yes (compiled in) |
| Android NDK r27c (27.2.12479018) | custom build | github.com/HomuHomu833/android-ndk-custom | Apache-2.0 / LLVM | toolchain only |
| aapt2 + framework res | from Apktool 2.12.1 | github.com/iBotPeaches/Apktool | Apache-2.0 | toolchain only |
| apksig + zipalign | uber-apk-signer 1.3.0 | github.com/patrickfav/uber-apk-signer | Apache-2.0 | toolchain only |

Checksums: see `scripts/fetch_deps.sh`.

## Original game content
*Giants: Citizen Kabuto* © Planet Moon Studios / Interplay. Game data is **never** included in
this repository or APK. Users supply their own copy. This project is not affiliated with or
endorsed by the rights holders, GOG, or Meta.

## Reference material (not linked, not copied)
- GiantZiP (GPL-2.0+) — GZP format reference.
- GiantsTools (no licence) — GBS importer used to understand the format.
- vibe-giants-decomp (no licence, contains decompiled proprietary code) — facts only. Every
  claim is re-verified against the user's data before use.
