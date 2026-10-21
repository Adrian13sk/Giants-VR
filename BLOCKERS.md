# Blockers and external dependencies

| ID | Blocker | Impact | Needed action |
|---|---|---|---|
| B1 | No Quest 2 is reachable. The cloud dev environment has no USB/ADB, and the linked PC gives file access only (no shell). | Nothing can be device verified (M4/M5/M11/M13). | **User:** install `GiantsVR-dev.apk` with `adb install -r`, copy `Bin/` (see docs/INSTALL.md), run it, and send `adb logcat -d -s GiantsVR OpenXR`. |
| B2 | Google SDK hosts (dl.google.com, maven.google.com) are blocked from the dev environment. | Can't use the official NDK, build-tools or Gradle/AGP. | Worked around with GitHub-hosted, checksum-pinned equivalents (docs/DEPENDENCIES.md). Before any release, rebuild with Google's official NDK r27c (only `GVR_NDK` needs to change). |
| B3 | No original source code exists. | Gameplay, AI and missions must be reimplemented. | Ongoing reverse engineering, validated against the original data files. Long-term effort. |
| B4 | Object placement format is not fully decoded: world op 42 types map to object definitions in `xx_*.bin`; the static structures' placement source isn't confirmed (`.abx` sets, `wm_*` op 0x03, GTI trailing data). | Levels show terrain only. | Reverse engineering (next priority). |
| B5 | The world opcode table is incomplete: 24/33 world streams stop early (ops 0, 44, 50, 109, 110, 128, 35). | Later world records (lights, objects) are lost for those levels. Terrain still loads. | Fix the sizes against the data (as done for ops 41/42/88). |
| B6 | Rendering rules are provisional: unit scale (0.3 m/unit), lightmap modulation (2x plus a floor), terrain UV tiling, fog colour byte order, sky band placement, transparency flags. | Visual fidelity. | Compare against original-game screenshots. Screenshots from the user's PC would help. |
| B7 | Legal: the decompilation and GiantsTools repos have no licence; GiantZiP is GPL-2.0. | Their code can't be copied into this project. | Use them only as factual references (current practice). |
