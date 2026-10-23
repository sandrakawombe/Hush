#!/usr/bin/env bash
# Download the public clips the test bench is built from (~140 MB).
#   ESC-50 (coughs, sneezes and look-alike sounds)  CC BY-NC 3.0
#   Free Spoken Digit Dataset (speech)               CC BY-SA 4.0
set -euo pipefail
cd "$(dirname "$0")/../data"
mkdir -p raw/esc50 raw/fsdd
ESC=https://raw.githubusercontent.com/karolpiczak/ESC-50/master
FSDD=https://raw.githubusercontent.com/Jakobovski/free-spoken-digit-dataset/master/recordings

curl -sSf -o raw/esc50/esc50.csv "$ESC/meta/esc50.csv"
grep -E ',(coughing|sneezing|laughing|clapping|door_wood_knock|keyboard_typing|breathing|mouse_click),' raw/esc50/esc50.csv \
  | cut -d, -f1 > raw/esc50/want.txt
# extra everyday sounds for model training (folds 1-4 only: fold 5 is the locked test set)
grep -E ',(dog|door_wood_creaks|footsteps|glass_breaking|can_opening|drinking_sipping|brushing_teeth|clock_alarm|crying_baby|snoring|hand_saw|pouring_water|toilet_flush|vacuum_cleaner|cat|rooster|siren|car_horn|church_bells|water_drops),' raw/esc50/esc50.csv \
  | awk -F, '$2<5{print $1}' >> raw/esc50/want.txt
cat raw/esc50/want.txt | xargs -P 16 -I{} sh -c '[ -f raw/esc50/{} ] || curl -sSf -o raw/esc50/{} '"$ESC"'/audio/{}'

for s in jackson nicolas theo yweweler george lucas; do
  for d in 0 1 2 3 4 5 6 7 8 9; do for i in $(seq 0 14); do echo "${d}_${s}_${i}.wav"; done; done
done | xargs -P 16 -I{} sh -c '[ -f raw/fsdd/{} ] || curl -sSf -o raw/fsdd/{} '"$FSDD"'/{}'

echo "ESC-50 clips: $(ls raw/esc50/*.wav | wc -l)   speech clips: $(ls raw/fsdd/*.wav | wc -l)"
