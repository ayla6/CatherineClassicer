#!/usr/bin/env python3
"""Remove the 5 s minimum on the quote ("kakugen") loading screens.

State 7 of the loading-screen state machine (0x63520f) waits until its timer
reaches 5.0 s (or 7.0 s for quote #99) before fading out, even if loading is
done. This repoints the 5.0 load at a -0.0 constant in .rdata (0xbf8560), so
it fades out as soon as it can. Quote #99 keeps its 7 s.
usage: patch-quotes.py [EXE]   (default: the game's Catherine.exe, edited in place)
"""
import sys
exe = sys.argv[1] if len(sys.argv) > 1 else '/var/data/Games/common/Catherine Classic/Catherine.exe'
OFF, OLD, NEW = 0x23461C, bytes.fromhex('c842bf00'), bytes.fromhex('6085bf00')
d = bytearray(open(exe, 'rb').read())
cur = bytes(d[OFF:OFF+4])
if cur == NEW: print('already patched'); sys.exit()
assert cur == OLD and d[OFF-4:OFF] == bytes.fromhex('f30f100d'), 'unexpected exe'
d[OFF:OFF+4] = NEW
open(exe, 'wb').write(d); print('patched', exe)
