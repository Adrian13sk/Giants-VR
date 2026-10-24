# Play-test checklist (Quest 2)

Use **Main menu -> MISSION** (left/right, trigger) to jump to a mission; numbers below are its place in that list.
Turn on **Options -> FRAME RATE: SHOWN** and note the number wherever it drops below 70 (green = 70+).
Everything here passed the desktop test harness; none of it has run on the headset yet unless marked.

For each item: works / doesn't / how it feels. A phone photo of anything that looks wrong helps a lot.

## 1. Base building (newest work)
**22. 5c "The Battle!" (Meccaryn)**
- [ ] Opening cutscene plays; you start near Borjoyzee (he waves when you're close).
- [ ] Walk to a **Smartie hatch** (the level has three): 3 Smarties climb out and run to the base.
- [ ] Shoot Vimps, pick up the meat, walk past the **MEAT DROP-OFF**: "VIMP MEAT STORED".
- [ ] Stand on the **BUILD PAD**: two rows of signs within arm's reach. Touch one with a hand: it starts building.
      Are the signs easy to read and to hit? Do they ever trigger by accident?
- [ ] Smarties walk to the site and hammer / saw / dig; the building rises out of the ground.
- [ ] Finished buildings are solid. **Giftshop** opens a shop counter in front of it.
- [ ] **Raids**: Reapers march in from the barracks' side and set buildings on fire ("UNDER ATTACK" sign).
      Is their pace OK (they arrive about a minute after a raid starts)?
- [ ] One raider grabs **Borjoyzee** and carries him off: kill it and he walks home. Let it escape: MISSION FAILED,
      the mission restarts.
- [ ] Upgrade a finished building (its sign says UPGRADE TO ...).
- [ ] Base complete (Pub + Workshop + Giftshop + Party House): you get a **POP-UP BOMB** (weapon strip, 5th cell).
- [ ] Pop-up bomb by the army barracks: right stick forward to pick it, trigger to set it down, run - it destroys
      the barracks (only the bomb can).

**34. r4a "Sappho's Fleet" (Delphi)** - same as above with souls, the Reaper buildings and Yan; the **tornado**
spell (hold the cast longer) only works once the base is complete. Also **27** and **36** (the Reaper battles).

## 2. Pop-up bomb
**17. story3_prison_7** - Reg gives you a bomb; the Shop has one more. Only the bomb breaks the prison gates.

## 3. Cutscenes
- [ ] Any mission's opening and closing cutscene: fades in/out smoothly, characters stand on the ground, weapons are
      in hands (e.g. **26. r1_saveyan** / the Reaper guards firing into the air), effects (flashes, dust, glows).

## 4. Earlier work still waiting for a headset check
- Story 1 (**1-5**): Timmy, Rippers, jetpack, the wax Smarties, the Shop.
- Kabuto (**39-54**): growing by eating, throwing boulders and Reapers (hold X), magnifying glass (**45**, **47**).
- Delphi: sword swings, fire wall, bubble; Reaperski races (**30-33**).
- Frame rate in Story 1's village, at a tower collapse, in flight, and in a base mission with buildings up.

## Reporting
Anything odd: the mission number and what you did just before. If the Quest is plugged in, the automated runner
(`devtest.ps1`) also collects the log and frame times.
