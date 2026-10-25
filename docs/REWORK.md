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
| Looks | Textures up to 256 px are doubled and sharpened when loaded (crisper up close; +~1 s level load on the PC). **Fixed after APK 68003c5:** a tap-offset bug in the doubling made every other row and column come from the wrong place, so magnified ground looked blocky - in every rework APK until then. Now smooth (unit test added). | HARNESS |
| Combat | A hit Reaper's (or Ripper's) body flashes hot for a moment. Low health: a pulsing dark red rim (the heartbeat in both hands was already there - a second one the rework had added is gone). | HARNESS |
| Pacing | Ambushes: after 70-110 s on the move without a fight, a squad of 2-3 Reapers comes at you from ahead (Options: AMBUSHES). Not in base missions, races, multiplayer or as Kabuto. | HARNESS |
| VR feel | Aim laser from the gun's muzzle to what it would hit (Options: AIM LASER, default on). Reach out with a hand to take a pickup. | HARNESS |
| Guidance | Objective marker: a gold diamond toward the nearest unfinished objective with its distance (beyond 20 m). | HARNESS |
| Performance | The adaptive quality fallback (level 2+) drops the per-pixel relief and highlights. | HARNESS |
| Tools | `scripts/make_video.sh`: progress videos from the harness (spectator view, captions). | HARNESS |
| Combat | A Reaper who sees your aim laser resting on him dives aside (now and then; not posted sentries). | HARNESS |
| Combat | Explosions throw nearby Reapers back. Reaper grenades leave from over the thrower's head (they used to burst on him at once). | HARNESS |
| Combat | Shots shove Reapers back (more for head hits and heavier guns); a killing shot sends the body sliding. A bolt flying close past a Reaper makes him flinch, often diving aside. | HARNESS |
| Pacing | Ambushes only in missions that have Reapers of their own. | HARNESS |
| VR feel | Holsters follow your body's direction (your head's yaw within a neck's turn), not just the room. | HARNESS |
| Options | LIGHTING: REWORK / CLASSIC (the same as `GVR_LOOK=0`). | HARNESS |
| Looks | Grass: tufts on gentle grassy ground within 16 m of you, coloured and lit like the ground under them, swaying in gusts. Not on red earth, sand, rock or as Kabuto; off with the classic lighting and the adaptive fallback. | HARNESS |
| Kabuto | Hands: in first person, reach down and squeeze a grip on what is at your feet. A Reaper or a boulder stays in that hand - swing and open it to throw, or bring it up to your mouth to eat it; a Smartie, a Vimp or a small creature is scooped up and eaten. (X still grabs as before.) | HARNESS |
| Delphi | Sword parry: a Reaper bolt meeting the blade is knocked away with a spark and a ping; met with a swing, it goes back at the Reaper it came from. | HARNESS |
| Delphi | Throw your fire bolt: let go of the trigger in a throwing motion and the bolt flies along your throw, faster (40 m/s instead of 28), with a kick in the hand. A still tap fires along your aim as before. | HARNESS |
| Effects | Shots striking scenery throw a soft glow and quick sparks (the old flash sprite was blocky up close). | HARNESS |
| Cutscenes | Two shots that stayed black for 10 s (Delphi's jetski arrival at the volcano, the Story 3 prison lift) now show. Shot cameras looking steeply down or up at their actors swing round them to a comfortable angle (your view never tilts by itself). All 30 opening cutscenes and the 17 other cutscenes reviewed on contact sheets (a flyover's far look-at point is left alone). | HARNESS |
| Kabuto | Story check: every gate objective of his campaign breaks with its own mechanic (punches; adrenaline - both grips; the magnifying glass), and the other Kabuto in kstory5_4 falls to punches. Not yet checked: walking to the portals. | HARNESS |
| Kabuto | Climbs like a giant: slopes up to 68 degrees (a man's limit is 50), judged over his stride, so banks and ledges small beside him are steps. kstory3_4's portal stands on an island ringed by cliffs that he could never climb before. | HARNESS |
| Guidance | One-time hints the first time Kabuto has something grabbable at his feet and the first time a bolt flies close by Delphi with her sword out. | HARNESS |

Harness regression: all 56 missions exit 0 with no errors (latest run after the grass).

## Trying it on the Quest
Install the newest `GiantsVR-rework-<commit>.apk` like the others. New in Options: COMFORT VIGNETTE, AMBUSHES,
AIM LASER, LIGHTING. With the headset on USB, the usual tests plus the rework's cost comparisons:

```powershell
cd $env:USERPROFILE\Documents\GiantsQuestVR\devtest
powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -Apk ..\GiantsVR-rework-<commit>.apk
powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -NoInstall -Set experiments -Tests x_village_base,x_village_look0,x_village_msaa0,x_village_up0,x_k3_base,x_k3_look0,x_k3_msaa0,x_k3_up0,x_village_nograss
powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -NoInstall -Tests kabuto_grab_eat,kabuto_grab_throw,delphi_parry
```

The second compares the rework's look (`_base`) with the classic lighting (`_look0`), no anti-aliasing
(`_msaa0`), no texture sharpening (`_up0`) and no grass (`_nograss`) in demanding scenes, so the cost of
each shows. The last line runs the new hand mechanics (Kabuto's grab, Delphi's parry) on the headset.

## Next
- More combat rework: hit reactions, enemy variety in behaviour.
- Effects: impact sparks.
- Cutscenes: the later (mid-mission) cutscenes; compare camera framing with the original on video.
- Missions: play each objective chain in the harness (k1a → k1b done).
