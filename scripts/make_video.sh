#!/bin/bash
# Progress video: short clips rendered by the headless harness (a spectator view from the player's
# head, 1280x720, 4x MSAA, 24 fps of the 72 Hz simulation) joined with captions.
# usage: GVR_BIN=<Bin> scripts/make_video.sh <out.mp4> [clip list file]
# Clip list lines: <caption>|<world>|<mission>|<from frame>|<frames>|<env and harness args...>
B="${GVR_BIN:?set GVR_BIN to the Bin folder}"
OUT=${1:?output mp4}
LIST=${2:-}
HR=${HR:-build/host/headless_render}
cd "$(dirname "$0")/.."
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT  # (the clips take gigabytes)
FONT=$(fc-match -f '%{file}' 'DejaVu Sans:bold' 2>/dev/null || echo /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf)
if [ -z "$LIST" ]; then
  LIST=$WORK/clips.txt
  cat > "$LIST" <<'EOF'
Kabuto story opening: cutscene camera fixed|Kstory1|k1a_smarties|0|720|
Kabuto starts at his village (marker id 3), faster stride|Kstory1|k1a_smarties|0|720|GVR_CUTSCENE=none --move 0 1
Meccaryn: new lighting, 4x MSAA, gun recoil|Story1|1c_1_firstmecc|0|600|GVR_CUTSCENE=none GVR_GOD=1 --spawn-reaper 14 --auto-aim --trigger-window 60 600 --trigger-pulse 8
Base defence: turrets and raids|Story5|5c_securebase|14400|720|GVR_CUTSCENE=none GVR_GOD=1 GVR_BASE_ORDER=all GVR_RAID_SOON=1
Delphi's island|Rstory1|r1_saveyan|0|600|GVR_CUTSCENE=none --move 0 1
EOF
fi
n=0
: > "$WORK/concat.txt"
while IFS='|' read -r cap world mission from frames args; do
  [ -z "$cap" ] && continue
  case "$cap" in \#*) continue;; esac
  d=$WORK/c$n; mkdir -p "$d"
  envs=""; harg=""
  for a in $args; do
    case "$a" in GVR_*=*) envs="$envs $a";; *) harg="$harg $a";; esac
  done
  total=$((from + frames))
  env $envs GVR_MISSION="$mission" timeout 3000 "$HR" --data "$B" --world "$world" --play --frames $total \
      --video "$d" 3 --video-from "$from" --out "$d/final.png" $harg > "$d/log" 2>&1 < /dev/null
  echo "clip $n: $cap ($(ls "$d"/f*.png 2>/dev/null | wc -l) frames, exit $?)"
  # The caption from a file (drawtext's own escaping lost apostrophes and cut at colons and commas).
  printf '%s' "$cap" > "$WORK/cap$n.txt"
  ffmpeg -nostdin -loglevel error -y -framerate 24 -i "$d/f%05d.png" \
      -vf "drawtext=fontfile=$FONT:textfile=$WORK/cap$n.txt:x=24:y=h-56:fontsize=28:fontcolor=white:box=1:boxcolor=black@0.55:boxborderw=10" \
      -c:v libx264 -pix_fmt yuv420p -crf 20 "$WORK/c$n.mp4"
  echo "file '$WORK/c$n.mp4'" >> "$WORK/concat.txt"
  n=$((n + 1))
done < "$LIST"
ffmpeg -nostdin -loglevel error -y -f concat -safe 0 -i "$WORK/concat.txt" -c copy "$OUT"
echo "wrote $OUT ($n clips)"
