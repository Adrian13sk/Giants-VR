# The rework (branch `rework`)

Started 2026-10-25 after the user's play-test: "boring graphics-wise and gameplay-wise, not VR-ish
enough, cutscenes have misplaced actors, the Kabuto story starts in the wrong place, can't be
completed, and he is too slow." The classic version is kept: tag `classic-final` (2e3cf55, branch
`main`), and a bundle of the whole repository is on the user's PC (`giants-vr-classic-2e3cf55.gitbundle`).

Status labels: **HARNESS** = checked in the PC test renderer (Mesa); **BUILD** = compiles into the
APK; **DEVICE** = run on the Quest. Nothing here is DEVICE yet.

## Done so far

| Area | Change | Status |
|---|---|---|
| Kabuto | Cutscenes after Kabuto's scale was set put the viewer 5 m underground (the "misplaced actors"): the eyes now stay put when the world scale changes. | HARNESS |
| Kabuto, Delphi | Every Kabuto and Delphi mission now starts at its own start marker (Kabuto id 3, Delphi id 2; the Meccaryn's id 1). All of them began at the terrain's default point before; in k1a that was an empty canyon with no Smarties to eat, so Kabuto could never grow enough for the wooden gate. Harness: all 15 Kabuto and 18 Delphi missions start on the ground at their marker; k1a's gate breaks at 70 % size and k1b follows. | HARNESS |
| Kabuto | Faster: 14 m/s walking, 23 m/s charging at full size; speed goes with the square root of his size (71 % when half grown). Grows faster from what he eats. | HARNESS |
| Looks | 4x MSAA (multiview, resolved on the tile). | BUILD |
| Looks | Lighting: relief from the textures themselves (bump mapping without tangents), sun highlights, sun glow in the haze, soft highlight roll-off, two more octaves of ground detail up close, brighter ambient. `GVR_LOOK=0` restores the classic look for comparisons. | HARNESS |
| VR feel | No debug controller boxes or laser lines in play: your hero's own hands. Slim glowing pointers in menus only. | HARNESS |
| VR feel | Guns kick up and back in the hand on every shot (per-weapon strength, spring back). | HARNESS |
| VR feel | Two-handed aim: squeeze the other hand's grip near the barrel of a long gun and it aims along the line between your hands (half the kick). | HARNESS |
| VR feel | Body holsters: right hip = handgun, over the right shoulder = long guns (again for the next), left hip = grenades; a tick in the controller at each. Right stick still cycles. | HARNESS |
| VR feel | Punches: the free hand swung into a Reaper hurts him (harder the faster), knocks him back. | HARNESS |
| Comfort | Vignette while the stick moves or turns you (Options: off / low / high; default low). | HARNESS |
| Combat | Reapers press in while firing (walk-fire), run faster, react sooner, pause less between bursts. | HARNESS |
| Combat | Cover works: your real head sets your hit box (duck behind a rock and bolts pass over). Reapers who lose sight of you lob grenades (warned by the throw sound). | HARNESS |
| Effects | Muzzle flashes and explosions light up their surroundings for a moment; explosions get a swelling fireball and thrown embers. | HARNESS |
| Atmosphere | Floating motes: dust in the light by day, pulsing fireflies at night. | HARNESS |
| Looks | Textures up to 256 px are doubled and sharpened when loaded (crisper up close; +~1 s level load on the PC). | HARNESS |
| Combat | A hit Reaper's (or Ripper's) body flashes hot for a moment. Low health: a pulsing dark red rim and a heartbeat in both hands. | HARNESS |
| Pacing | Ambushes: after 70-110 s on the move without a fight, a squad of 2-3 Reapers comes at you from ahead (Options: AMBUSHES). Not in base missions, races, multiplayer or as Kabuto. | HARNESS |
| VR feel | Aim laser from the gun's muzzle to what it would hit (Options: AIM LASER, default on). Reach out with a hand to take a pickup. | HARNESS |
| Guidance | Objective marker: a gold diamond toward the nearest unfinished objective with its distance (beyond 20 m). | HARNESS |
| Performance | The adaptive quality fallback (level 2+) drops the per-pixel relief and highlights. | HARNESS |
| Tools | `scripts/make_video.sh`: progress videos from the harness (spectator view, captions). | HARNESS |

Harness regression: all 56 missions exit 0 with no errors (latest run after ambushes and Reaper grenades).

## Trying it on the Quest
Install the newest `GiantsVR-rework-<commit>.apk` like the others. New in Options: COMFORT VIGNETTE, AMBUSHES,
AIM LASER. With the headset on USB, the usual tests plus the rework's cost comparisons:

```powershell
cd $env:USERPROFILE\Documents\GiantsQuestVR\devtest
powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -Apk ..\GiantsVR-rework-<commit>.apk
powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -NoInstall -Set experiments -Tests x_village_base,x_village_look0,x_village_msaa0,x_village_up0,x_k3_base,x_k3_look0,x_k3_msaa0,x_k3_up0
```

The second compares the rework's look (`_base`) with the classic lighting (`_look0`), no anti-aliasing
(`_msaa0`) and no texture sharpening (`_up0`) in two demanding scenes, so the cost of each shows.

## Next
- Delphi and Kabuto hands: VR-native sword / bow / grab feel; Kabuto's slam and throw.
- More combat rework: hit reactions, enemy variety in behaviour, cover, grenades from Reapers.
- Effects: bigger explosions with light flashes, impact sparks, muzzle light.
- Cutscenes: check every one for misplaced actors (contact sheets per shot).
- Missions: play each objective chain in the harness (Kabuto k1a gate first).
