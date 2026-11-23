#!/bin/sh
# Install the movie hook: copies mg.dll next to the exe and renames the exe's
# "MF.dll" import to "MG.dll" (1 byte). Original exe kept as Catherine.exe.orig.
# usage: install.sh [GAME_DIR]      uninstall: install.sh --revert [GAME_DIR]
set -e
[ "$1" = --revert ] && { rev=1; shift; }
G=${1:-"/var/data/Games/common/Catherine Classic"}
if [ -n "$rev" ]; then cp "$G/Catherine.exe.orig" "$G/Catherine.exe"; rm -f "$G/mg.dll"; echo reverted; exit; fi
[ -f "$G/Catherine.exe.orig" ] || cp "$G/Catherine.exe" "$G/Catherine.exe.orig"
python3 - "$G/Catherine.exe.orig" "$G/Catherine.exe" <<'P'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
o = 0x84923a
assert d[o-1:o+7] == b'\0MF.dll\0', 'unexpected exe'
d[o+1:o+2] = b'G'
open(sys.argv[2], 'wb').write(d)
P
cp "$(dirname "$0")/mg.dll" "$G/mg.dll"
"$(dirname "$0")/../patch-quotes.py" "$G/Catherine.exe"
echo "installed; log: $G/videohook.log"
