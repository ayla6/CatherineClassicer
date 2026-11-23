#!/usr/bin/env bash
# Print every game data file Catherine.exe opens, as it happens.
# Polls /proc/<pid>/fd, so no root and no ptrace are needed. Start it before
# or after launching the game.
# usage: watch-open-files.sh [REGEX]   (default: \.wmv$; use . for everything)
pat=${1:-'\.wmv$'}
declare -A seen
echo "waiting for Catherine.exe..."
while :; do
  pids=$(pgrep -if 'catherine\.exe' | tr '\n' ' ')
  if [[ -z $pids ]]; then sleep 1; continue; fi
  for pid in $pids; do
    for fd in /proc/$pid/fd/*; do
      t=$(readlink "$fd" 2>/dev/null) || continue
      [[ $t == *"/data/movie"* && $t =~ $pat ]] || continue
      key="$pid:$fd:$t"
      if [[ -z ${seen[$key]} ]]; then
        seen[$key]=1
        printf '%s  %s\n' "$(date +%T)" "${t#*/Catherine Classic/}"
      fi
    done
  done
  # forget fds that were closed, so a reopen is printed again
  for key in "${!seen[@]}"; do
    IFS=: read -r pid fd t <<<"$key"
    [[ $(readlink "$fd" 2>/dev/null) == "$t" ]] || unset "seen[$key]"
  done
  sleep 0.2
done
