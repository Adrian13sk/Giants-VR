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
Worlds whose mission has no enemies or objectives fall back to waves of Reaper soldiers.

**HUD (original Meccaryn art):** first person: over your left forearm (raise the wrist to read it); third person: lower
left of the view. Heart = health (fills from the bottom, pulses red when low), arrow = jetpack fuel (flashes red when
burnt dry), syringe + number = health injections carried, the weapon in hand, and one pip per objective (gold = done).
Red flash = you were hit; a red wedge around the middle of the view points to where the hit came from. Below 35 health
you trail smoke. Switching weapons drops the weapon strip in at the top of the view.

**Health injections:** green syringes float over the ground in some missions (1c, 1d...) and some Reapers drop one.
Walk over one to take it (up to 5). First person: press **X** to take one into your left hand, then **push it into
your chest**; it heals 50 over a second (green flash). Press X again to put it away. Third person: **X** plays the
Meccaryn's injection animation and heals.

**Damage:** headshots on Reapers do 2.5× damage; a Ripper's mouth is its weak spot. Your own rockets throw you (rocket jumps)
and hurt you the closer they burst (up to 40 at point blank, nothing beyond 4 m). Falls of about 10 m or more hurt and stun you briefly. Trees, rocks, houses and guard towers can be
destroyed (rockets work best) and char and smoulder as they take damage; a sentry rides his tower down when it collapses. When you die you see your body (fallen, gone under in the water, or blown
apart), then the screen fades and you start again at the mission's start.

Deep water floats you (no diving) and the piranhas come after a moment; get out or jetpack away.

**Rippers** mostly lie buried: the ground ahead bursts with dirt as one tunnels at you, then it springs out and bites.
Shots and blasts nearby wake them. Small ones can latch onto your legs for a few seconds. Dead Rippers leave their spit
glowing on the ground: walk over it to heal (orange a little, blue a lot). Look at a Smartie or a creature (or go near)
to see its name.

**Bush pack (Story 2 from 2b):** push the right stick forward to cycle the weapon strip to the bush. You crouch in a
bush: no flying, no shooting, slow; it uses up jetpack fuel. Reapers that haven't spotted you won't, unless you're right
next to one or move in front of it - then it walks over and pokes the bush. Cycle back to the gun to come out.

**Kabuto** starts his first levels weakened (half size in the first, then 65% and 80%); each Smartie he eats makes
him bigger (Vimps and others a little), up to full size.

**The Shop (1d, and levels in Stories 2-4 that have one; it also sells flares):** walk into the rusty bunker: the rocket launcher (if you don't have it) and injections float in front
of you - walk into one or reach for it to take it (once each per level). Stand in the glowing pool to heal until it runs dry. Bodies stay where
they fall until the Cleaners come to eat them; shoot a body to burst it or tear it apart.

You start as the **Meccaryn in first person**: his body is under you, his arms follow your
controllers (grip = make a fist, trigger = curl the leading finger); the original Meccaryn gun is in your right hand. Walking around your room
moves him too.

| Input | Play mode (Meccaryn) | Free camera |
|---|---|---|
| Left stick | Run (5 m/s) relative to where you look; sideways = sidestep, back = backpedal | Move |
| Left grip (hold) | Sprint ×1.5 (in the air with the jetpack: turbo, double fuel use); near a Smartie: pick it up (release = let go) | Speed boost ×5 |
| Right grip (hold) | Near a Smartie: pick it up (release = let go). First person: **reach to your right thigh (gun) or over your shoulder (launcher) and grip to draw the holstered weapon** | — |
| Right stick left/right | Snap turn 30° | Snap turn 30° |
| Right stick forward | Switch gun ↔ rocket launcher (once you have it: from 1c on, Timmy's "present" at the end of 1b); shows the weapon strip | — |
| **Right trigger** | **Fire**: gun (hold for auto-fire, 5 shots/s) or rocket launcher (one rocket per pull, 1 s reload, splash damage; keep your distance) | Haptic test pulse |
| **A** (hold) | **Jetpack**: lifts you; steer with the left stick only while it fires (otherwise you drift and fall). Fuel lasts ~8 s and refills only on the ground; burn it dry and it overheats until partly refilled. **Dry in the air, keep holding A**: it sputters and brakes your fall so you land safely | — |
| Right stick click | First ↔ third person (third: camera behind him) | — |
| **B** | Switch to the free camera | Back to the Meccaryn (he drops in below you) |
| **Y** | **Map**: a holographic table of the island in front of you (dark until you've been there); Y again closes it | Debug fly/noclip on/off (strong buzz = on) |
| X | Health injection (FP: into the left hand, push into the chest; TP: inject) | Tap: next mission. Hold 3 s: **new game** (forgets the checkpoint, back to 1a) |
| **Left trigger** | **Flare** (once you have some from a Shop): thrown up off the jetpack; homing missiles go for it | — |
| **Menu button (left)** | **Options** (game paused): turning, main hand, first/third person, HUD size, volume, music, subtitles, new game. Stick up/down to choose, left/right to change; trigger or A on RESUME | same |
| **Left stick click (hold)** | **Mission briefing** over the world while held (title, objectives, progress) | Toggle the engineering test geometry |
| Long-press Oculus button | System recentre | same |

Comfort defaults: snap turn, 3 m/s movement, no head bob, no camera shake. A smooth-turn and
left-handed mode exist in code (left-handed mode moves the fly toggle to B and the boost to the right grip) (`ComfortSettings`). There is no in-headset menu for them yet.

## Uninstall
`adb uninstall io.github.giantsvr.dev`

## Progress and options
- **Checkpoints:** progress is saved when each mission starts (`save.txt` next to your `Bin` folder: world, mission,
  injections carried, rocket launcher). Starting the app continues from there. New game: Options → NEW GAME (hold the
  trigger 2 s), or in the free camera hold X for 3 s.
- **Options** are kept in `settings.txt` next to `save.txt`. FRAME RATE: SHOWN puts a frame-rate counter at the top
  left of the view (green at 70+; please note it for the performance pass).
- **FOVEATED RENDERING** (Options): the edges of each eye are drawn at lower resolution, which you mostly can't see
  but saves a lot of graphics work. MEDIUM (the default) held 72 fps in every measured scene; OFF is the sharpest
  but drops to 62-68 fps with Kabuto walking or in flight; HIGH is the fastest.
- **Day and night:** a 20-minute cycle (about 12 minutes of day, warm dusk and dawn, a short moonlit night with stars).
  Options → DAY AND NIGHT → ALWAYS DAY turns it off.
- **Rumble:** hits (stronger on the side they come from), the jetpack (a kick at take-off, a hum while it fires,
  harder on turbo), landings, and a heartbeat below 30 health.
- **Story:** finishing 1d now continues into Story 2 (2a). Story 2's objectives aren't implemented yet (2a can't be
  completed).

## Delphi (Reaper story)
Options → NEW GAME: choose **REAPER STORY (DELPHI)** with the stick, hold the trigger 2 s. Delphi has no jetpack:
- **A**: jump (she steers in the air). **Left grip**: turbo run. Deep water: she swims fast.
- **Sword (right hand):** first person - **swing the controller**; the faster the swing, the harder the hit. Third
  person - **right trigger**: a three-swipe combo, the third hits hardest.
- **Fire bolt: left trigger** (first person: from your left hand where it points; third person: toward the crosshair or
  straight ahead), about one a second. The HUD's fire icon is lit when it's ready.
- In first person her body isn't drawn (you see the controllers and the sword).

**Delphi's spells** (move-hand trigger): a **tap** throws her fire bolt - or, aimed at a Smartie, a **bubble** that catches
it and floats it to you. **Hold and release** after about a second: a **fire wall** rolls forward, burning everything in
its path. In the battles (from Sappho's Fleet on), hold longer (the glow in your hand turns blue): a **tornado** - the
only thing that brings down an army barracks.

## Kabuto (Kabuto story)
Options → NEW GAME: choose **KABUTO STORY** with the stick, hold the trigger 2 s. You are the giant: the world is
scaled so your eyes are at his height (about 13 m). The HUD and menu stay their normal size.
- **Left stick**: walk. **Left grip**: run. **A**: jump. Landing from a jump shakes the ground.
- **Right trigger** (third person): punch, alternating fists.
- **First person punches:** swing your fists, since the controllers are his hands. Slam a fist down to the floor for a
  ground pound.
- **Left trigger**: stomp, which hurts everything around his foot.
- **X**: grab and eat the nearest Smartie, Reaper or small Ripper in front of you. It heals you, and it's the only way
  to heal.
- **Throwing:** keep **X** held on a Reaper and he holds it instead of eating it; let go to throw it. Thrown Reapers
  bowl over the ones they hit and don't get up. In first person, throw with your left hand (swing it and let go of X;
  letting go without a swing just drops it); in third person he throws at the nearest building or Reaper in front.
- **Boulders** (the volcano levels): punch or stomp the top of the pillar of volcanic rock and a boulder breaks off
  (or walk up to it and press X to pull it off). Hold **X** to carry it, let go to throw it. A boulder bursts where it
  lands and wrecks what's near it - two bring down the Reaper barracks. The pillar grows a new top after a few
  seconds.
- **Y**: roar, which knocks Reapers back.
- Trees and Smartie houses smaller than him are trampled when you walk into them. Smarties run from you.
- **Y held** (0.6 s): lay an egg. An Offspring hatches and hunts nearby Reapers, Vimps and Smarties for you, growing as
  it eats (up to four at a time; the HUD shows how many).
- **Both grips** when the adrenaline gauge (where the jetpack gauge would be) is full: 10 seconds of fury - a red glow,
  much harder hits and bigger ground pounds. It fills slowly once he's full size.
- **Giant walls:** the first only falls once he's grown to about 70% size, the second at 85%, the third at full size and
  under adrenaline. A message tells you when he's too small.
- **The magnifying glass** (the crystal levels): smash the GIANT CRYSTAL, walk over the glass it leaves and he picks it
  up. Hold the **right trigger** to focus the sun through it - in first person it burns wherever your right hand
  points, in third person in front of him. It sets enemies alight and is the only thing that burns through the
  CRYSTAL GATE.
- In first person you see his arms and hands following the controllers.

## Main menu
The game now starts at the main menu: night over the intro island with the GIANTS logo in the sky.
- **Stick**: choose. **Trigger or A**: select.
- **CONTINUE** goes back to your last checkpoint (the mission's name is shown).
- **NEW GAME**: pick the story with the stick left/right (its loading art shows beside the menu). If you have a
  checkpoint, hold the trigger 1.5 s, because a new game replaces it.
- **OPTIONS**: the usual options. The left menu button opens them too.
- Loading screens show the original art of the story's race next to the mission briefing.
- Delphi regains health while swimming in deep water, as her briefing says.

## Story 2 onwards
- **Vimps** graze in herds from Story 2. They run when you come close or shoot. Kill one and it drops pieces of Vimp
  meat; walk over them to pick them up (the HUD counts them). In 2a, bring 20 pieces to Timmy's father, at the
  floating marker.
- **Destroy missions:** barracks, prisons, doors and launcher bases named by a mission now take damage and collapse
  (rockets work best).
- **Sonak groups:** the guards appear round them; kill the guards. The Sonak itself isn't in yet.
- **Bosses:** the Offspring in 4b and the Charger in 5a can be shot down, but they don't attack yet.

## Reaperski (Delphi)
- **Getting on:** walk into a moored Reaperski. In races you start on one.
- **Riding:** **left stick** forward to go, back to brake or reverse, left/right to steer.
- **Turbo:** hold the **left grip** (from turbo pickups).
- **Missiles:** **right trigger** (from missile pickups). A hit spins a rival out.
- **Y**: get off, outside races.
- **Races:** pass close to every buoy in order. Missing three loses the race; only first place wins, otherwise the
  race restarts.
- **Ice walls:** break them with missiles, or ram them on turbo.

## Kabuto vs Kabuto
In Kabuto's last chapter another giant comes for you. As the Meccaryn (the very last mission), shoot Kabuto's belly:
it is his weak spot.

## Bosses, Sonaks, SAMs
- The Offspring, the Charger, Sonaks and the rival Kabuto now attack you. The Charger charges from a distance:
  sidestep it.
- Sonaks are huge horned beasts that come with their Reaper guards. Kill all of them.
- SAM turrets fire homing missiles at you while you fly. Stay low near them, or blow them up (200 hp).
