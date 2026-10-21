# Installing the development build on a Quest 2

This is an **unofficial engineering build**. It is not a finished game. It shows original
Giants worlds and models in VR, but has no gameplay yet. It has **not been tested on a
headset**.

## Requirements
- A Quest 2 (or Quest Pro/3/3S) with **Developer Mode** enabled in the Meta Horizon phone app.
- A PC with `adb` (Android platform-tools) or SideQuest, and a USB-C cable.
- Your own legally obtained copy of Giants: Citizen Kabuto (GOG or Steam). The APK contains
  **no game data**.

## 1. Install the APK
```
adb devices                      # accept the USB debugging prompt inside the headset
adb install -r GiantsVR-dev.apk
```
The app appears under **Library → Unknown Sources** as "Giants VR (unofficial dev)".

## 2. Copy your game data
Copy the *contents* of your game's `Bin` folder (`*.gzp`, `*.bin`, `*.abx`, about 440 MB) to
the app's data folder:
```
adb shell mkdir -p /sdcard/Android/data/io.github.giantsvr.dev/files/Bin
adb push "C:\GOG Games\Giants - Citizen Kabuto\Bin\." /sdcard/Android/data/io.github.giantsvr.dev/files/Bin/
```
Launch the app once before pushing if Android refuses to create the folder.

## 3. Run and collect logs
```
adb logcat -c
adb shell am start -n io.github.giantsvr.dev/android.app.NativeActivity
adb logcat -s GiantsVR OpenXR
```
Please send the log. It shows the OpenXR runtime, extensions, swapchain sizes, the loaded world
and an fps line every 5 seconds.

## Controls (current build)
You start as the **Meccaryn in first person**: his body is under you, his arms follow your
controllers (grip = make a fist, trigger = curl the leading finger); the original Meccaryn gun is in your right hand. Walking around your room
moves him too.

| Input | Play mode (Meccaryn) | Free camera |
|---|---|---|
| Left stick | Run (5 m/s) relative to where you look; sideways = sidestep, back = backpedal | Move |
| Left grip (hold) | Sprint ×1.5 | Speed boost ×5 |
| Right stick left/right | Snap turn 30° | Snap turn 30° |
| **Right trigger** | **Fire the Meccaryn gun** (hold for auto-fire, 5 shots/s) | Haptic test pulse |
| **A** (hold) | **Jetpack** thrust; release to hover down and land (you can land on walls and roofs) | — |
| Right stick click | First ↔ third person (third: camera behind him) | — |
| **B** | Switch to the free camera | Back to the Meccaryn (he drops in below you) |
| Y | — | Debug fly/noclip on/off (strong buzz = on) |
| Left stick click | Toggle the engineering test geometry | same |
| Long-press Oculus button | System recentre | same |

Comfort defaults: snap turn, 3 m/s movement, no head bob, no camera shake. A smooth-turn and
left-handed mode exist in code (left-handed mode moves the fly toggle to B and the boost to the right grip) (`ComfortSettings`). There is no in-headset menu for them yet.

## Uninstall
`adb uninstall io.github.giantsvr.dev`
