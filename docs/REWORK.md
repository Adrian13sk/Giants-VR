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
| Comfort | Getting hit flashes a red rim round the view instead of washing the whole view red. | HARNESS |
| Comfort | Vignette while the stick moves or turns you (Options: off / low / high; default low); a lighter one while a cutscene camera travels. | HARNESS |
| Combat | Reapers press in while firing (walk-fire), run faster, react sooner, pause less between bursts. | HARNESS |
| Combat | Cover works: your real head sets your hit box (duck behind a rock and bolts pass over). Reapers who lose sight of you lob grenades (warned by the throw sound). | HARNESS |
| Sound | Binaural cues: a sound to one side reaches the far ear up to 0.65 ms later and duller (head shadow); sounds behind you are duller in both ears. Makes it easier to hear where a Reaper is. | HARNESS (unit test) |
| Effects | Muzzle flashes and explosions light up their surroundings for a moment; explosions get a swelling fireball and thrown embers. | HARNESS |
| Atmosphere | Floating motes: dust in the light by day, pulsing fireflies at night. | HARNESS |
| Looks | Textures up to 256 px are doubled and sharpened when loaded (crisper up close; +~1 s level load on the PC). **Fixed after APK 68003c5:** a tap-offset bug in the doubling made every other row and column come from the wrong place, so magnified ground looked blocky - in every rework APK until then. Now smooth (unit test added). | HARNESS |
| Combat | A hit Reaper's (or Ripper's) body flashes hot for a moment. Low health: a pulsing dark red rim (the heartbeat in both hands was already there - a second one the rework had added is gone). | HARNESS |
| Pacing | Ambushes: after 70-110 s on the move without a fight, a squad of 2-3 Reapers comes at you from ahead (Options: AMBUSHES). Not in base missions, races, multiplayer or as Kabuto. | HARNESS |
| VR feel | Aim laser from the gun's muzzle to what it would hit (Options: AIM LASER, default on). Reach out with a hand to take a pickup. | HARNESS |
| VR feel | Touch: a hand pushed into the ground or scenery gives a tap, then a light hum while it stays in. | HARNESS |
| Guidance | Objective marker: a gold diamond toward the nearest unfinished objective with its distance (beyond 20 m). | HARNESS |
| Performance | The adaptive quality fallback (level 2+) drops the per-pixel relief and highlights. | HARNESS |
| Tools | `scripts/make_video.sh`: progress videos from the harness (spectator view, captions). | HARNESS |
| Combat | A Reaper who sees your aim laser resting on him dives aside (now and then; not posted sentries). | HARNESS |
| Combat | Explosions throw nearby Reapers back. Reaper grenades leave from over the thrower's head (they used to burst on him at once). | HARNESS |
| Combat | Shots shove Reapers back (more for head hits and heavier guns); a killing shot sends the body sliding. A bolt flying close past a Reaper makes him flinch, often diving aside. Badly hurt, a Reaper often falls back for a couple of seconds before fighting on. One Reaper in four is a marksman: he keeps his distance and fires single aimed shots, a red aiming line runs from his gun to you for the 0.7 s before each shot - see it and move. | HARNESS |
| Pacing | Ambushes only in missions that have Reapers of their own. | HARNESS |
| VR feel | Holsters follow your body's direction (your head's yaw within a neck's turn), not just the room. | HARNESS |
| Options | LIGHTING: REWORK / CLASSIC (the same as `GVR_LOOK=0`). | HARNESS |
| Looks | Grass (a smaller patch at the first adaptive-quality step): tufts on gentle grassy ground within 16 m of you, coloured and lit like the ground under them, swaying in gusts. Not on red earth, sand, rock or as Kabuto; off with the classic lighting and the adaptive fallback. | HARNESS |
| Kabuto | Hands: in first person, reach down and squeeze a grip on what is at your feet. A Reaper or a boulder stays in that hand - swing and open it to throw, or bring it up to your mouth to eat it; a Smartie, a Vimp or a small creature is scooped up and eaten. (X still grabs as before.) | HARNESS |
| Delphi | Sword parry: a Reaper bolt meeting the blade is knocked away with a spark and a ping; met with a swing, it goes back at the Reaper it came from. | HARNESS |
| Delphi | A faint blue trail follows her blade on fast swings. | HARNESS |
| Delphi | Jump for real and she jumps (as Kabuto leaps). | HARNESS |
| Delphi | Throw your fire bolt: let go of the trigger in a throwing motion and the bolt flies along your throw, faster (40 m/s instead of 28), with a kick in the hand. A still tap fires along your aim as before. | HARNESS |
| Effects | Shots striking scenery throw a soft glow and quick sparks (the old flash sprite was blocky up close). | HARNESS |
| Cutscenes | Two shots that stayed black for 10 s (Delphi's jetski arrival at the volcano, the Story 3 prison lift) now show. Shot cameras looking steeply down or up at their actors swing round them to a comfortable angle (your view never tilts by itself). All 30 opening cutscenes and the 17 other cutscenes reviewed on contact sheets (a flyover's far look-at point is left alone). | HARNESS |
| Kabuto | Story check: every gate objective of his campaign breaks with its own mechanic (punches; adrenaline - both grips; the magnifying glass), and the other Kabuto in kstory5_4 falls to punches. Portals: see the climbing line below. | HARNESS |
| Kabuto | Climbs like a giant: slopes up to 68 degrees (a man's limit is 50), judged over his stride, so banks and ledges small beside him are steps. The test walker reached the portals of k1b, k2d and kstory4_3. kstory3_4's portal is on a plateau 109 m up, ringed by 67-80 degree faces: he climbs the cliff with his hands (below) and walks to it - mission complete in the harness. | HARNESS |
| Kabuto | Jump for real: a quick hop of your own (your head rising fast from standing) makes him leap, about half as high again as the A-button jump. Drop into a quick crouch and he slams the ground (a quake round his feet). | HARNESS |
| Climbing | Kabuto and Delphi climb steep ground with their hands: squeeze a grip with the hand on a face too steep to walk, pull down to haul yourself up, reach up with the other hand and grab again. Kabuto's long arms pull 1.5 times as far. Let go of both and you drop. | HARNESS |
| Guidance | One-time hints the first time Kabuto has something grabbable at his feet and the first time a bolt flies close by Delphi with her sword out. | HARNESS |
| Menus | Title and options panels: dark glass with a thin gold edge and a gold highlight on the chosen row. | HARNESS |
| Looks | Distant haze takes its colour from the sky behind it, so far ground and sea dissolve into the sky instead of ending at a flat fog-coloured plane (`GVR_SKY_HAZE=0` for the flat colour). The fog colour itself had been read with red and blue swapped in every world (Rstory1's teal haze was olive, Story3's orange one blue) - fixed, checked against all 14 skies. | HARNESS |
| Atmosphere | Palms bend in the wind, more toward the crown, each on its own beat (`GVR_NO_WIND=1`: still; rework lighting only). | HARNESS |
| Looks | The status panel's backing at the wrist is a soft-edged rounded plate instead of a hard grey box. | HARNESS |
| Missions | A leftover test display (a Smartie, a giant Kabuto walking on the spot, a turret and a palm by the map's centre) appeared in the five missions that have no characters of their own: kstory3_1, kstory5_4, rstory5_queen_4, rstory5_to_queen_3 and the ending. Gone. | HARNESS |
| VR feel | Your own body never comes within reach of your eyes in first person (Delphi's idle lean had put her shoulder in view), and everything on the head is hidden with it (her hair, eyes and lips; Kabuto's jaw, teeth and tongue). | HARNESS |
| Missions | A start marker on top of a mission character: r1_saveyan began with Delphi standing inside Yan (his face filled the view when he waved) - she now starts a step back with Yan in front of her. In 2b_2 the wife placed on the start marker rides on the Meccaryn's back ("the road home, with someone's wife on your back"), unseen in first person, instead of standing in the way. | HARNESS |
| VR feel | Wave at the Smarties: raise a hand above your head and wave it side to side - the villagers around wave back and one calls out (Meccaryn and Delphi). | HARNESS |
| Kabuto | Beat your chest: fists onto the chest, right, left, right (each within a second) and he roars - the same roar as Y, knocking Reapers back. A thump and a buzz at each blow. | HARNESS |
| Combat | More life in the Reapers, from the original's unused clips: marksmen kneel to take their aimed shots, bazooka carriers kneel with it on the shoulder (rpman_bzka_kneel_fire), and about a third of the others crouch to fire (a smaller target - the hit box and head-shot height drop with them); get right up against a Reaper and he punches you (a step back, some damage) instead of firing point blank; a head shot drops him with the eye-hit death, a leg shot with the to-the-knees death; grenade throwers actually throw; a heavy blow (a head shot, the shotgun up close, a rocket's edge) knocks a Reaper off his feet and he scrambles up again (and is a low target meanwhile); when they reposition, Reapers often run to cover - beside the far side of a rock, tree or hut from you - and fire crouched from there (they give it up if you move round them). Not as Kabuto. | HARNESS (unit test) |
| Pacing | Half the ambush squads now come down from the sky on hover packs (the original's unused rpman_Hover clip), flame and smoke under them, 20-30 m ahead - shoot them out of the air or meet them when they land ("REAPERS - ABOVE YOU!"). | HARNESS |
| Atmosphere | When the last Reaper or Ripper near a Smartie village dies, the villagers celebrate and one calls out (not as Kabuto - they're his food). | HARNESS |
| Sound | Room echo: in canyons, gorges, caves and tunnels the sounds come back as reverb (walls all round and a roof overhead are measured four times a second); out in the open it is nearly dry. Distant sounds carry more of it. | HARNESS (unit test) |
| Delphi | Spells by gesture (first person, casting-hand trigger held): draw a circle above your head for the tornado (where she has it); sweep the hand fast across your body, low, for the fire wall. Holding the trigger to charge still works. | HARNESS |
| Missions | Barracks send out soldiers while they stand: Reaper barracks a Reaper or two out of the door every 20-35 s while you are within ~110 m, Ripper barracks medium Rippers, at most four of each one's own about; destroy the building to stop it. The placed Rippers all start underground except the small ones (nest and barracks broods come out above ground). | HARNESS |
| VR feel | First-person arms reach the controllers every time: Delphi's arms stretch to them, her hands are bigger (they read tiny), the sword sits in her hand; Kabuto's arms likewise. | HARNESS |
| Kabuto | Picks up everything but buildings: reach down and squeeze on a tree to uproot it - swing it as a club, throw it (it crashes down on what's there) or let it fall; Smarties, Vimps and small creatures stay in the hand - bring them to your mouth to eat, throw them (they die where they land) or open the hand gently to set them down. | HARNESS |
| Delphi | The bow is held in the left hand; the right hand takes the string, pulls back and lets go - an arrow flies only then (the trigger no longer draws it in first person, and her left hand casts no bolts while it holds the bow). | HARNESS |
| Water | Piranhas only go for the Meccaryn: Delphi swims freely, and no piranha takes on Kabuto - but he can't swim, so out of his depth he goes under and drowns (about 15 s) unless he gets back to the shallows; a warning tells him. | HARNESS |
| Smarties | Village Smarties can be hurt and killed by any hero - shots, blasts, a giant's fists or feet: they flinch (sm_gets_shot), run, and die (sm_death), and the body lies a couple of minutes. The Meccaryn and Delphi pick them up too (grip with the hand on one: it squirms; throw it and a hard landing kills it, or set it down and it cowers); Kabuto's hand grab holds them the same way. Point a gun or bow at one for a moment and it puts its hands up. A word from Timmy the first time you kill one. | HARNESS (unit test) |
| Vimps | A killed Vimp's body stays where it fell and its soul stands over it - see-through, glowing, swaying. A minute later the soul fades and the Vimp is born again by its herd, small; it grows over a minute and a half and can only be hunted (shot, blown up, grabbed or eaten) once grown. One eaten whole by Kabuto leaves only its soul. | HARNESS (unit test) |
| Delphi | Sword: a thrust (the blade moving along its own length) does four times the damage; a thrust that kills leaves the enemy run through - he hangs limp on the blade (a ragdoll pinned there) and moves with it, bleeding, until you yank the sword back out (or swing it hard, or after 8 s), and he falls off with the motion. A slash that kills cuts the body apart (the gib burst) instead. | HARNESS |
| Reapers | Ragdolls: a Reaper blown up, flung or killed falling goes limp at once and flies with the push - arms and legs flailing, tumbling, landing and sliding to rest on the ground; one shot dead plays the first moment of his death (the hit) and then crumples with the shot's push. A body lying still stays as it fell; a later blast throws it again. | HARNESS (unit test) |
| Graphics | Grass redone (user: "Grass looks awful"): each tuft takes the colour of the ground it grows from (the terrain's own colour at that spot, its baked light), darker roots, lighter and varied tips (fresh, yellowed, a few dry blades), thin curving blades, three cards per tuft so it reads from above; it grows in clumps and patches with bare ground between instead of evenly sprinkled sprites; edges smoothed by the headset's MSAA (alpha-to-coverage). Rock ("the rocks textures as well"): cliffs and slopes blend three projections (no seams or smears where a face turns), gently tilted strata of warmer and cooler stone, moss and earth on ledges that face up, a finer octave of the rock up close, and deeper relief lighting from the rock's own texture. | HARNESS |
| Cutscenes | Cinema screen (Options -> CUTSCENES: CINEMA SCREEN, the default; AROUND YOU = the immersive view as before): every shot is shown from its own camera - position, pitch, roll and movement as the original framed it - on a 3.6 m screen standing in a dark room in front of you; the screen stays put through the cuts. Fixes found by playing all 66 cutscene chains to the end: characters with no typed record were all drawn as Meccaryns (Delphi, Yan, Timmy, Smarties, Borjoyzee, the wife) or took another character's place - now each gets its own model on its own mover; a camera standing inside a cliff sees out as in the original instead of being lifted onto it; a shot that has faded to black with nothing left in it ends 2 s later (the ending held 130 s of black after THE END). | HARNESS |
| Combat | Shots fired from inside a big target's body (the Charger or an Offspring right on top of you) now hit it; before, they passed through and the fight stalled at close range. | HARNESS |
| Missions | The invisible markers where a Sonak and its guards spawn were solid and stopped your shots at the guards (story3_3sonaks_4 could stall); they are now neither drawn nor solid. | HARNESS |
| Missions | Meccaryn and Delphi story check: the pop-up bomb destroys the prison (story3_prison_7), the Torture Square gate (4a) and the barracks (5c); the tornado the barracks in both Reaper battles, also after building the base first; the Offspring (4b), the Charger (5a), the Raiks (r1_clusterspell), the Sonak guards (story3_sonak_1, story3_3sonaks_4) and all twelve guard objectives of rstory3_pre_race_1 fall to ordinary fighting. Scripts in `scripts/completion/`. | HARNESS |

Harness regression: all 56 missions exit 0 with no errors (latest run after the grass).

## Trying it on the Quest
Install the newest `GiantsVR-rework-<commit>.apk` like the others. New in Options: COMFORT VIGNETTE, AMBUSHES,
AIM LASER, LIGHTING. With the headset on USB, the usual tests plus the rework's cost comparisons:

```powershell
cd $env:USERPROFILE\Documents\GiantsQuestVR\devtest
powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -Apk ..\GiantsVR-rework-<commit>.apk
powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -NoInstall -Set experiments -Tests x_village_base,x_village_look0,x_village_msaa0,x_village_up0,x_k3_base,x_k3_look0,x_k3_msaa0,x_k3_up0,x_village_nograss
powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -NoInstall -Tests kabuto_grab_eat,kabuto_grab_throw,delphi_parry,delphi_throw_bolt,kabuto_climb,kabuto_climb_k34,kabuto_chest_beat,wave_smarties,delphi_tornado_circle,delphi_firewall_sweep,kabuto_tree,kabuto_smartie_throw,delphi_bows,delphi_impale,reaper_ragdolls
```

The second compares the rework's look (`_base`) with the classic lighting (`_look0`), no anti-aliasing
(`_msaa0`), no texture sharpening (`_up0`) and no grass (`_nograss`) in demanding scenes, so the cost of
each shows. The last line runs the new hand mechanics on the headset (Kabuto's grab, climb and chest beat; Delphi's parry and thrown bolt; waving at Smarties).

## Next
- More combat rework: peeking from cover (rpman_behind_tree_peek, rpman_rock_peek).
- Effects: impact sparks.
- Cutscenes: the later (mid-mission) cutscenes; compare camera framing with the original on video.
- Missions: play each objective chain in the harness (Kabuto end to end; Meccaryn/Delphi special objectives and bosses checked).
