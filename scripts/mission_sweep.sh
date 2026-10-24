#!/bin/bash
# Regression sweep: every mission headless for 500 frames with firing, jetpack, the map (Y), an
# injection (X) and a flare; prints exit code, error / warning counts and the update time per mission.
# usage: GVR_BIN=<Bin> scripts/mission_sweep.sh <out dir>
B="${GVR_BIN:?set GVR_BIN to the Bin folder}"; S=${1:?output folder}; mkdir -p "$S/sweep"
cd "$(dirname "$0")/.."
for f in "$B"/wm_*.bin; do
  m=$(basename "$f" .bin); m=${m#wm_}
  case $m in 1*) w=Story1;; 2*) w=Story2;; story3*) w=Story3;; 4*) w=Story4;; 5*) w=Story5;; r1*) w=Rstory1;; r4*) w=Rstory4;; rstory2*) w=Rstory2;; rstory3*) w=Rstory3;; rstory5*) w=Rstory5;; k1*) w=Kstory1;; k2*) w=Kstory2;; kstory3*) w=Kstory3;; kstory4*) w=Kstory4;; kstory5*) w=Kstory5;; end) w=the_end;; mission1) w=Story1;; *) w=Story1;; esac
  GVR_MISSION=$m GVR_CUTSCENE=none timeout 600 build/host/headless_render --data "$B" --world $w --play --frames 500 --trigger-window 60 300 --jet-window 100 250 --y-window 320 322 --x-window 400 402 --ltrigger-window 200 202 --out $S/sweep/$m.png > $S/sweep/$m.log 2>&1
  ec=$?
  echo "$m $w exit=$ec E=$(grep -c '\[E\]' $S/sweep/$m.log) W=$(grep -c '\[W\]' $S/sweep/$m.log) $(grep -oE 'update: [0-9.]+ ms average, [0-9.]+ ms worst' $S/sweep/$m.log)"
done
