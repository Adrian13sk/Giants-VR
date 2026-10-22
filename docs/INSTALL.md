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

Optional but recommended: the game's **music** (`Music`, about 280 MB), **cutscene soundtracks and effects**
(`Stream`) and **spoken dialogue** (`StreamEnglish`, about 100 MB: every voice line in the cutscenes) live next to
`Bin`. Copy them beside it so the app finds them:
```
adb push "C:\GOG Games\Giants - Citizen Kabuto\Music" /sdcard/Android/data/io.github.giantsvr.dev/files/
adb push "C:\GOG Games\Giants - Citizen Kabuto\Stream" /sdcard/Android/data/io.github.giantsvr.dev/files/
adb push "C:\GOG Games\Giants - Citizen Kabuto\StreamEnglish" /sdcard/Android/data/io.github.giantsvr.dev/files/
```
Without them the game runs silently except for sound effects.

## 3. Run and collect logs
```
adb logcat -c
adb shell am start -n io.github.giantsvr.dev/android.app.NativeActivity
adb logcat -s GiantsVR OpenXR
```
Please send the log. It shows the OpenXR runtime, extensions, swapchain sizes, the loaded world
and an fps line every 5 seconds.

## Controls (current build)
Story1 plays its missions in order, as in the original:
1. **1a "A Boy Called Timmy"** opens with the intro cutscene (Baz's lander drop). You start without the jetpack;
   Rippers guard Timmy's tower at the north end of the island. Kill them and reach the tower; the wrist shows one pip
   per objective. The jetpack cutscene follows and Timmy gives you the jetpack.
2. **1b "The Wax Smarties"**: Smarties hang from the rock pillars and can only hold on for a while. Fly over,
   **reach for a Smartie and squeeze the grip** to pick it up (either hand). Carry it to the Smartie pad and
   **let go of the grip** over the pad. **Four saved** completes the mission (as the briefing says). One that falls
   in the water is eaten by the piranhas within about five seconds: grab it out quickly. Another hangs there later.
3. **1c** and later: the mission's objectives (reach a place, kill marked enemies) work; story events in between
   are not implemented yet.

Cutscenes: watch from the shot's camera (you travel with it, the view never turns by itself); subtitles follow
your head, just below the line of sight. **Either trigger skips** (release it first if you were holding it when the cutscene started).
In the free camera (B), **X skips to the next mission** (handy for testing 1b without replaying 1a).
Worlds whose mission has no enemies or objectives fall back to waves of Reaper soldiers. Your health bar is on your
left wrist; red flash = you were hit. When you die you see your body (fallen, gone under in the water, or blown
apart), then the screen fades and you start again at the mission's start.

Deep water floats you (no diving) and the piranhas come after a moment; get out or jetpack away. Bodies stay where
they fall until the Cleaners come to eat them; shoot a body to burst it or tear it apart.

You start as the **Meccaryn in first person**: his body is under you, his arms follow your
controllers (grip = make a fist, trigger = curl the leading finger); the original Meccaryn gun is in your right hand. Walking around your room
moves him too.

| Input | Play mode (Meccaryn) | Free camera |
|---|---|---|
| Left stick | Run (5 m/s) relative to where you look; sideways = sidestep, back = backpedal | Move |
| Left grip (hold) | Sprint ×1.5; near a Smartie: pick it up (release = let go) | Speed boost ×5 |
| Right grip (hold) | Near a Smartie: pick it up (release = let go) | — |
| Right stick left/right | Snap turn 30° | Snap turn 30° |
| Right stick forward | Switch gun ↔ rocket launcher (once you have it: from 1c on, Timmy's "present" at the end of 1b) | — |
| **Right trigger** | **Fire**: gun (hold for auto-fire, 5 shots/s) or rocket launcher (one rocket per pull, 1 s reload, splash damage; keep your distance) | Haptic test pulse |
| **A** (hold) | **Jetpack**: thrust builds up speed gradually (up and forward); fuel lasts ~4 s and refills shortly after you stop (blue bar on the wrist) | — |
| Right stick click | First ↔ third person (third: camera behind him) | — |
| **B** | Switch to the free camera | Back to the Meccaryn (he drops in below you) |
| Y | — | Debug fly/noclip on/off (strong buzz = on) |
| X | — | Next mission (loads it and returns to play) |
| **Left stick click (hold)** | **Mission briefing** over the world while held (title, objectives, progress) | Toggle the engineering test geometry |
| Long-press Oculus button | System recentre | same |

Comfort defaults: snap turn, 3 m/s movement, no head bob, no camera shake. A smooth-turn and
left-handed mode exist in code (left-handed mode moves the fly toggle to B and the boost to the right grip) (`ComfortSettings`). There is no in-headset menu for them yet.

## Uninstall
`adb uninstall io.github.giantsvr.dev`
