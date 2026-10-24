# On-device tests

`devtest.ps1` runs scripted tests on the Quest connected to this PC by USB. Nobody needs to wear the
headset: the app plays each script itself, then the runner pulls the results.

```powershell
cd $env:USERPROFILE\Documents\GiantsQuestVR\devtest
powershell -ExecutionPolicy Bypass -File .\devtest.ps1            # all tests in .\tests
powershell -ExecutionPolicy Bypass -File .\devtest.ps1 -Sweep     # plus all 56 missions, 20 s each (~40 min)
```

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
