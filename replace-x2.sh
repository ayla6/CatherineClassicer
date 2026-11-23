#!/usr/bin/env bash
# Replace the (upscaled, worse-looking) *_x2.dds and *_x2.sp2 assets with
# reflinks of their 1x originals. Saves ~2.5 GB. Meant for 1080p.
# usage: replace-x2.sh [--dry-run] [GAME_DIR]
set -euo pipefail
dry=0; [[ ${1:-} == --dry-run ]] && { dry=1; shift; }
game=${1:-"/var/data/Games/common/Catherine Classic"}
cd "$game/data"

n=0; skipped=0
while IFS= read -r -d '' x; do
  base="${x%_x2.*}.${x##*.}"
  if [[ ! -f $base ]]; then echo "no base, skip: $x"; skipped=$((skipped+1)); continue; fi
  if (( dry )); then echo "would link: $x <- $base"
  else cp --reflink=always --preserve=all "$base" "$x"; fi
  n=$((n+1))
done < <(find . -path ./test -prune -o -type f \( -name '*_x2.dds' -o -name '*_x2.sp2' \) -print0)
echo "replaced $n, skipped $skipped"
