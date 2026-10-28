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

**Health injections:** green syringes float over the ground in some missions (1c, 1d...) and some Reapers drop one. Others drop a box of ammo for a weapon you carry that isn't full (rockets, shells, clips) - walk over it.
Walk over one to take it (up to 3, the game's own limit). First person: press **X** to take one into your left hand, then **push it into
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
| Right grip (hold) | Near a Smartie: pick it up (release = let go). First person: **reach to your right thigh (handgun) or over your shoulder (launcher) and grip to draw the holstered weapon** | — |
| Right stick left/right | Snap turn 30° | Snap turn 30° |
| Right stick forward | Next weapon you carry: handgun → shotgun → machine gun → sniper gun → Millenium mortar → grenades → rocket launcher → bush pack → pop-up bomb; shows the weapon strip | — |
| **Right trigger** | **Fire**: handgun (hold, 5 shots/s), shotgun (one exploding shell per pull), machine gun (hold, 10/s), sniper gun (one heavy shot per pull, kills a Reaper), mortar (lobs a bouncing bomb - big blast, keep your distance), rocket launcher (one rocket per pull) | Haptic test pulse |
| **A** (hold) | **Jetpack**: lifts you; steer with the left stick only while it fires (otherwise you drift and fall). Fuel lasts ~8 s and refills only on the ground; burn it dry and it overheats until partly refilled. **Dry in the air, keep holding A**: it sputters and brakes your fall so you land safely | — |
| Right stick click | First ↔ third person (third: camera behind him) | — |
| **B** | Switch to the free camera | Back to the Meccaryn (he drops in below you) |
| **Y** | **Map**: a holographic table of the island in front of you (dark until you've been there); Y again closes it | Debug fly/noclip on/off (strong buzz = on) |
| X | Health injection (FP: into the left hand, push into the chest; TP: inject) | Tap: next mission. Hold 3 s: **new game** (forgets the checkpoint, back to 1a) |
| **Left trigger** | **Flare** (once you have some from a Shop): thrown up off the jetpack; homing missiles go for it | — |
| **Menu button (left)** | **Options** (game paused): difficulty, turning, movement (smooth / teleport), seated play, main hand, first/third person, HUD size, volume, music, subtitles, new game. Stick up/down to choose, left/right to change; trigger or A on RESUME | same |
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
- **Options added 2026-10-28:** DIFFICULTY (easy: half the damage, wilder Reaper aim; hard: x1.5, sharper aim),
  MOVEMENT: TELEPORT (push the move stick forward to aim an arc, let go to blink there; on foot only), PLAY SEATED
  (the view and hands raised 45 cm), KABUTO MIC ROAR (shout and he roars; asks for the microphone).
- **Checkpoints within a mission:** dying brings you back where you finished the last objective.
- After each mission the next loading screen shows its numbers: time, kills, hit rate, deaths.
- **FOVEATED RENDERING** (Options): the edges of each eye are drawn at lower resolution, which you mostly can't see
  but saves a lot of graphics work. MEDIUM (the default) held 72 fps in every measured scene; OFF is the sharpest
  but drops to 62-68 fps with Kabuto walking or in flight; HIGH is the fastest.
- **Day and night:** a 20-minute cycle (about 12 minutes of day, warm dusk and dawn, a short moonlit night with stars).
  Options → DAY AND NIGHT → ALWAYS DAY turns it off.
- **Rumble:** hits (stronger on the side they come from), the jetpack (a kick at take-off, a hum while it fires,
  harder on turbo), landings, and a heartbeat below 30 health.
- **Story:** finishing a chapter's last mission continues into the next chapter (Story 1 -> 2 -> ... -> the Reaper and
  Kabuto stories). Every mission has its objectives from the game's data.

## Base building (Story 5, "The Battle!" - mission 5c)
- The base is laid out round Borjoyzee from the game's own base template: Build Pad, meat drop-off and fire pit
  stand from the start; Pub, Workshop, Giftshop, Party House, Wood Gate and three Wood Walls are yours to order.
- Hunt Vimps and carry the meat to the **MEAT DROP-OFF** (walk past it): it goes into the base's store.
- Stand on the **BUILD PAD**: two rows of signs float round you within arm's reach, one per building with its cost
  in meat (ordered ones show BUILDING... / COMPLETED!). **Touch a sign with either hand** to order it. The Smarties
  build two at a time; the building rises out of the ground (the game's own build animation) with a progress sign.
- **Smarties do the work.** You start with three; each building going up needs two, so with three only one goes
  up at a time (others show AWAITING MORE SMARTIES). Three more wait under each of the level's **Smartie hatches**:
  walk up to a hatch and they climb out and head for your base. At the sites they hammer, saw and dig (Delphi's cast
  the build spell); idle ones sit by the middle of the base.
- **Protect Borjoyzee / Yan** (as the briefings say): he stands by the Build Pad and waves when you come close. One
  Reaper in each raid goes for him, slings him over its shoulder and heads back the way it came. **Kill it** and he
  walks home; if it gets him back to where the raid set out, the mission is lost and starts again from its briefing.
- Finished buildings are solid. The **Giftshop** opens a shop counter in front of it (launcher, injections, flares,
  health pool); after the **Workshop** the Smarties build 1.5x faster.
- Pub + Workshop + Giftshop + Party House = "BASE COMPLETE!" and the Smarties give you a **POP-UP BOMB**.
- **Delphi's base (Reaper missions r4a "Sappho's Fleet", rstory2_battle_1 and rstory5_battle_2 - their briefings
  all say BUILD A BASE)** works the same way with the Reaper buildings: Persuader,
  Spell School, Spellshop, Party House (these four complete the base), Shield Generator, Reaper Gate, Forcefields.
  Vimps now roam that island: their **souls** go to the **SOUL DROP-OFF**. As the briefing says, the **TORNADO** spell
  comes once the base is complete.
- **Raids:** a minute after your first building stands, and then every 80 s while the enemy army barracks stands, a
  few Reapers march on the base along the level's own army paths from the barracks. Near a building they wreck it (it burns; a red
  "UNDER ATTACK" sign shows its health; Borjoyzee / Yan call for help). They stand and shoot at the building, and
  your Smarties cower and stop work while Reapers are close. Kill them or draw them off. A wrecked
  building falls back to its site: order it again on the Build Pad. Destroying the barracks stops the raids.
- **Upgrades:** once a building stands, its sign on the Build Pad turns into **UPGRADE TO ...** with a cost; touch it
  and the Smarties rebuild it as the game's second-level model: Stone Workshop, Giftshop 2 (adds a pop-up bomb to its
  counter), Stone Gate, Stone Walls; Delphi: Persuader 2, Spellshop 2, Reaper Gate 2, Forcefields 2. Upgraded
  buildings take 60% more punishment from raiders. (The Pub and Party Houses have no second level in the data.)

## Original effects (test)
Options → **EFFECTS: ORIGINAL (TEST)** plays explosions and cutscene effects from the game's own effect file
(fx.bin) instead of the port's simple particles. Our reading of that file is partly guesswork, so it may look odd
(sizes, timing) - tell us how it compares. **SIMPLE** is the default.

## Meccaryn weapons (from the game's weapondefs.bin)
- The missions hand out the guns as in the original's item lists: **shotgun** from 1c, **sniper gun** from Story 3's
  bunker mission, **rocket launcher** from Story 3's launcher mission (or a Shop), **Millenium mortar** from 4a,
  **machine gun** and **grenades** from 5a. You keep them from mission to mission (the checkpoint saves them and their
  ammo).
- **Rocket launcher:** now counts its rockets (8 with it, 12 at most); the Shop's AMMO box refills them.
- **Mines:** buy them at a Shop (5 at a time). Pick MINES on the strip, trigger sets one down in front of you; after
  2 s its red light blinks and the first Reaper, Ripper or Vimp to step close sets it off. Yours don't go off
  under you - but don't stand next to one when it blows.
- **Grenades:** pick them on the strip; one sits in your right hand. Hold the trigger, swing your arm and let go - it
  flies as fast as you threw it, bounces and blows up 2.5 s later (or when it hits someone). Let go without swinging
  and it is lobbed a few metres ahead.
- The new guns have ammo (the number under the icon; dim = empty, a click when you pull with none): shotgun 25 (35
  max), sniper 8 (12), machine gun 800 (2500), mortar 6 (10). Shotgun shells lie around in Story 1-2 (the boxes
  that used to give health injections were shell boxes): walk over them. Every Shop now also has an **AMMO** box
  that fills all your guns (once per level).
- **Sniper zoom:** bring the sniper gun up to your eye - the little screen on its side shows the view down the
  barrel, magnified about 4x, with a red cross where the shot goes.

## Pop-up bomb (Meccaryn: story3_prison_7, and 5c once the base is complete)
- **Right stick forward** cycles the weapon strip to the bomb (it appears in your hand). **Right trigger** sets it
  down in front of you; it beeps for 5 s (faster at the end), pops up and blows - get clear (about 14 m).
- As the briefings say, only the bomb breaks the **prison gates** in story3_prison_7, the **Torture Square gate** in
  4a and the **army barracks** in 5c. Reg gives you one at the start of story3_prison_7; the Shops in prison_7 and
  4a sell them.

## Delphi (Reaper story)
Options → NEW GAME: choose **REAPER STORY (DELPHI)** with the stick, hold the trigger 2 s. Delphi has no jetpack:
- **A**: jump (she steers in the air). **Left grip**: turbo run. Deep water: she swims fast.
- **Sword (right hand):** first person - **swing the controller**; the faster the swing, the harder the hit. Third
  person - **right trigger**: a three-swipe combo, the third hits hardest.
- **Fire bolt: left trigger** (first person: from your left hand where it points; third person: toward the crosshair or
  straight ahead), about one a second. The HUD's fire icon is lit when it's ready.
- In first person you see her body and arms (her head is hidden so it doesn't block the view).
- **Bows** (from the game's data: the **RPG bow** in "Smartie grab", the **bow**, RPG bow and **sniper bow** in
  Sappho's Fleet): **right stick forward** switches sword → bow → RPG bow → sniper bow → sword. First person
  (rework): the bow is in your **left hand**; **grab the string with your right hand (grip), pull back and let go** -
  the arrow flies when you let go, harder the further you pull. Third person: the right trigger. The plain bow never runs out; RPG arrows (they
  explode) and sniper arrows are counted under the sword icon; a Shop's AMMO box refills them.

**Delphi's spells** (move-hand trigger): a **tap** throws her fire bolt - or, aimed at a Smartie, a **bubble** that catches
it and floats it to you. **Hold and release** after about a second: a **fire wall** rolls forward, burning everything in
its path. In the battles (from Sappho's Fleet on), hold longer (the glow in your hand turns blue): a **tornado** - the
only thing that brings down an army barracks.

**Spells by gesture** (first person, the move-hand trigger held; Options → CONTROLS lists them): sweep the hand
low across you - fire wall; circle it over your head - tornado; raise it high and chop straight down - **hail** where
you look (Reaper story 3 on); both hands together before your chest - **slow time** (story 4 on); hand over your
eyes - **cloak**, unseen until you shoot (story 4 on); let a fire bolt go as the hand flicks up - **cluster**, six
bolts (story 4 on); point at a Reaper and pull the hand back - **follow**: he hangs in the air where you point;
swing and let go to throw him (story 5 on); both arms stretched out to the sides - **fire circle**: a ring of
blue fire round you for 8 s that burns up bolts and grenades and burns Reapers walking into it (story 4 on).

## Weapon wheel, Repair Gun, homing rockets, turret
- **Hold the right stick forward**: a ring of what you carry appears before you; move the gun hand toward one and
  let the stick go (Baz and Delphi). A short push still steps to the next.
- **Repair Gun** (Baz, base missions): aim at a damaged building and hold the trigger.
- **Homing rockets** (a level's Shop, once Baz has the launcher): they turn onto a foe. **Proximity rockets** (Shops,
  Story 4 on): they burst as they pass a foe. Bought rockets are loaded at once; the weapon wheel shows a launcher
  cell per load (homing tinted green, proximity orange) to change it.
- **Portable turret** (Baz, Story 3 on): in hand, point at the ground within 6 m and pull the trigger to plant it;
  it shoots the Reapers it can see (60 rounds), folds away when empty and is back 20 s later. Put a hand on its
  head and squeeze the grip to take it back.
- **Shield** (Baz, Story 4 on): choose it on the weapon wheel - a bubble takes up to 150 of the hits meant for you for
  12 s (not falls or water); 40 s before it can be raised again. The gun in hand stays.
- **Jetpack upgrade** (Baz, Story 5 on): an 11 s tank instead of 8, and faster flight.

## Main menu
- **PRACTICE** < BAZ | DELPHI | KABUTO >: the menu's island with every weapon of that hero's story, Reaper waves and
  Vimp herds; nothing is saved. **Options → MAIN MENU** (hold the trigger 2 s) leaves practice or a mission.
- The menus follow your controller's ray (and bare hands: point, pinch; the left hand's menu gesture opens Options).

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

## Hands (rework) - also in Options → CONTROLS
- **Pick things up** (Meccaryn, Delphi): bend down and squeeze a grip with your hand on a stone, a dead body (it
  hangs from your hand by the arm, leg or head you took), a Smartie, a gun shot out of a Reaper's hand or a cut-off
  limb. **Swing and let go to throw** - a thrown stone, body, gun or Smartie hurts the Reaper it hits. Options →
  GROUND REACH: EASY (default) lets you reach the ground by bending down with your hand out in front; NATURAL means
  crouching all the way.
- **Kabuto** reaches down and grips Smarties, Reapers, trees, boulders and dead bodies; bring them to his mouth to
  eat, swing and let go to throw. Beat your chest three times to roar.
- **Swimming** (Meccaryn, Delphi): in deep water hold the grips and pull your hands back through the water.
- **Reaperski**: hold both grips to take the handlebars - turn them to steer, twist your right wrist back for gas,
  A for turbo.
- **Shooting a Reaper's gun** out of his hand disarms him (he comes at you with his fists, or runs back for the gun);
  leg shots make him limp.
- **Catch a fish** (Meccaryn wading, Delphi swimming): fish swim in the clear shallows. Hold a hand still in the
  water and they come to it; squeeze the grip to catch one. Let go in the water and it swims off; throw it and it
  flops about on land. A moving hand, a splash or you wading in scares them away.
- **Birds** peck about on open ground: walk up, shoot or blast nearby and the flock takes off. Shoot one, or hit it
  with a stone, and it falls in a puff of feathers.

## World life (rework)
- **Options → AMBIENCE**: OFF / SOUNDS / SOUNDS + WEATHER (default). SOUNDS: surf along the shore, gusts of wind,
  leaves in the trees, running water, the volcanoes' rumble - placed where each level's own ambient markers are
  (the sounds are ours, the original has none for them). WEATHER: light rain on Delphi's first island, snow in the
  Reapers' cold world, a storm with lightning and thunder in Kabuto's last level.
- **Cloud shadows** drift over the land (off at the reduced quality levels and in LIGHTING: CLASSIC).

## Main menu
The game now starts at the main menu: night over the intro island with the GIANTS logo in the sky.
- **Stick**: choose. **Trigger or A**: select.
- **CONTINUE** goes back to your last checkpoint (the mission's name is shown). Left/right on this row picks one
  of **three save slots** (SLOT 1 is the original `save.txt`; 2 and 3 are `save2.txt` / `save3.txt`). NEW GAME and
  MISSION replace only the chosen slot's checkpoint.
- **NEW GAME**: pick the story with the stick left/right (its loading art shows beside the menu). If you have a
  checkpoint, hold the trigger 1.5 s, because a new game replaces it.
- **MISSION**: jump straight to a mission you have reached (left/right, in campaign order), trigger to start it.
  Each story offers its missions up to its first unfinished one (finished missions are listed in `progress.txt`). Like NEW GAME it replaces your checkpoint, so with one it needs a 1.5 s
  hold. See docs/PLAYTEST.md for what to look at in each.
- **OPTIONS**: the usual options. The left menu button opens them too.
- Loading screens show the original art of the story's race next to the mission briefing.
- Delphi regains health while swimming in deep water, as her briefing says.

## Story 2 onwards
- **Vimps** graze in herds from Story 2. They run when you come close or shoot. Kill one and it drops pieces of Vimp
  meat; walk over them to pick them up (the HUD counts them). In 2a, bring 20 pieces to Timmy's father, at the
  floating marker.
- **Destroy missions:** barracks, prisons, doors and launcher bases named by a mission now take damage and collapse
  (rockets work best).
- **Sonak groups, bosses:** see "Bosses, Sonaks, SAMs" below.

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
