# GiantsVR 1.0 RC2 (368d43d)

**Standalone VR port of Giants: Citizen Kabuto for Meta Quest 2.** Unofficial. **Contains no game data**: you need your own copy of the game (GOG or Steam).

### Install
1. Enable Developer Mode on the headset (Meta Horizon app).
2. Install the APK: `adb install -r GiantsVR-FINAL-368d43d.apk`
3. Copy your game's `Bin` folder (and optionally `Music`, `Stream` and `StreamEnglish`) to `/sdcard/Android/data/io.github.giantsvr.dev/files/`.

The full guide is in [docs/INSTALL.md](https://github.com/Adrian13sk/Giants-VR/blob/main/docs/INSTALL.md).

### What's in it
- **All three campaigns** (Meccaryn, Sea Reaper, Kabuto) and the ending: 56 missions from the original data, with cutscenes, music and voice.
- **VR-native play:** your hands are the hero's (punch, grab, throw, swim, climb), holsters and a wrist display, bows drawn by hand, and every one of Delphi's spells cast by gesture.
- **Kabuto at true giant scale:** eating, growing, throwing, roaring.
- **More modes:** base building, skirmish against computer players, LAN play, and a practice range.
- **Comfort options:** teleport or smooth movement, snap turning, a vignette, seated mode, left-handed mode.

The [README](https://github.com/Adrian13sk/Giants-VR#readme) lists every feature and the known limits.

### Status
- **Ran on a Quest 2** (whole automated test, 2026-10-27): 247 runs, all three campaigns to the end, 0 crashes, median 71.8 fps.
- **Desktop harness only so far:** the fixes since that run.
  - The Guards' lift; Delphi's first rescue missions; r4a's ships; the horn guards.
  - Sword angle; turbo with a bow; invisible Reapers; jetpack particles in first person.
  - Pause on the Meta button; safe saves; multiplayer spells.

### Files
- `GiantsVR-FINAL-368d43d.apk`: sha256 `8b6ae5e8514c42929829a08bcf8489a1b67744ad5cb84a74538f3cd4e75c19a4`
- `THIRD_PARTY_NOTICES.txt`: licences of the bundled OpenXR loader (Apache-2.0) and stb_image (MIT).
