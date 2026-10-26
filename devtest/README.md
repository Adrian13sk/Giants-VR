# On-device tests

`devtest.ps1` runs scripted tests on the Quest connected to this PC by USB. Nobody needs to wear the
headset: the app plays each script itself, then the runner pulls the results.

```powershell
cd $env:USERPROFILE\Documents\GiantsQuestVR\devtest
powershell -ExecutionPolicy Bypass -File .\devtest.ps1            # all tests in .\tests
powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -Sweep     # plus all 56 missions, 20 s each (~40 min)
```

**The whole test** (2026-10-27): every set, then the sweep, in one run of about 5 h 20 min. Keep the
headset on its charger: a PC's USB port charges it too slowly for five hours. The runner prints the
battery level before each test and waits to charge if it drops under 15 %.

```powershell
powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -Apk ..\GiantsVR-rework-<commit>.apk -All
powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -All -Skip campaign      # without the 55 min campaign
powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -Set missions,weapons    # some sets only
```

| Folder | Tests | Length | What |
|---|---|---|---|
| tests | 60 | ~39 min | feature tests |
| missions | 29 | ~58 min | objectives by their own mechanic: fights, pop-up bombs, tornadoes, races, Kabuto's gates and bosses |
| weapons | 18 | ~15 min | every weapon and ability of the three heroes |
| npcs | 7 | ~8 min | Reapers, Rippers, Vimps, the base crew, Offspring |
| ui | 10 | ~9 min | main menu, options, map, briefing, shop, hints |
| cutscenes | 66 | ~91 min | every cutscene chain to its end |
| campaign | 1 | ~55 min | the whole campaign in one run, each mission completed after 25 s of play |
| (sweep) | 56 | ~42 min | every mission for 20 s |

To run the GPU experiments (same scenes with foveation levels, shadows or reflections off):
`powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -Set experiments` (~9 min).

Results go to `results\<date-time>\`: `summary.txt`, and per test `autotest_out\result.txt` (fps, frame
time percentiles, CPU update / render time, GPU time, draw calls, memory), `frames.csv` (every frame),
left-eye screenshots, and the app's log (`logcat.txt`).

Before a run: plug the Quest in, wake it, and leave it on a desk in a lit room with the controllers
nearby. Close Rookie Sideloader (it runs its own adb). The runner keeps the headset awake and
restores the proximity sensor at the end.

Test scripts are plain text (`tests\*.txt`); the format is described at the top of
`src/game/test_script.h`. A script can be dry-run on the PC with the headless harness:
`build/host/headless_render --data <Bin> --play --script devtest/tests/perf_1a_village.txt`.

## Reading the results

`python3 tools/py/review_results.py devtest/results/<stamp>` prints a one-page Markdown summary: fps,
frame-time percentiles, CPU and GPU time, memory and the system's VrApi numbers per test; the worst frames
and whether they waited on the GPU, the CPU or neither; the sniper zoom and original-effects costs
(weapons_mecc, fx_original); and the change against the newest earlier run of the same tests in the same
results folder (or `--compare <older stamp>`). Anything that needs a look is listed at the end.
