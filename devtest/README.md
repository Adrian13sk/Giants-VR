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
