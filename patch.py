#!/usr/bin/env python3
"""Build a patched Catherine.exe from the original.

Finds the original exe (Catherine.exe or Catherine.exe.orig, identified by its
SHA-256), keeps it as Catherine.exe.orig, and writes a patched copy to
Catherine.exe. Re-running always starts again from the original.

Patches (all on by default except quotes):
  mg      Rename the "MF.dll" import to "MG.dll" and copy videohook/mg.dll
          next to the exe (fixes the movie flicker on NVIDIA).
  x2      0x63c840 resolves asset paths. In hi-res mode it tries name_x2.dds/.sp2
          and falls back to name.ext, but returns "this is x2" either way, so
          the callers halve the texture coords of a 1x fallback. Make it return
          whether the _x2 file actually exists, so deleted _x2 files just load 1x.
  logos   Skip the boot logos. The title state machine (0x5e5330, state in
          [this+0x28]) loads the logos in states 1-3, then shows them in
          0xb-0x14. At the end of state 3 this jumps to the code that runs
          after the last logo (0x5e6a2a), so it goes straight to the title.
  quotes  (opt-in) Remove the 5 s minimum on quote loading screens (#99 keeps
          its 7 s) by repointing the 5.0 constant at 0x63520f to -0.0.

usage: patch.py [--no-mg] [--no-x2] [--no-logos] [--quotes] [GAME_DIR]
       patch.py --revert [GAME_DIR]   (restore the original exe, remove mg.dll)
"""
import hashlib, os, shutil, sys

ORIG_SHA256 = 'ab4801d2882238fdcb1197dc0191f33543123a65fb3e1e28113603075c5ed7dc'

# name -> [(file offset, original bytes, new bytes)]
PATCHES = {
    'mg': [(0x849239, b'\0MF.dll\0', b'\0MG.dll\0')],
    # 0x63c926: add esp,4 / lea ecx,[ebp-0x10c] / test al,al / cmove ecx,esi / push ecx
    #        -> pop ecx / xchg ebx,eax / test bl,bl / cmove ecx,esi / push ecx / nop*7
    # 0x63c94d: mov al,1 -> mov al,bl
    'x2': [(0x23bd26, bytes.fromhex('83c4048d8df4feffff84c00f44ce51'),
                      bytes.fromhex('599384db0f44ce51' + '90' * 7)),
           (0x23bd4d, bytes.fromhex('b001'), bytes.fromhex('8ac3'))],
    # 0x5e6630: call 0x7a47d0 -> jmp 0x5e6a2a
    'logos': [(0x1e5a30, bytes.fromhex('e89be11b00'), bytes.fromhex('e9f5030000'))],
    'quotes': [(0x234618, bytes.fromhex('f30f100dc842bf00'), bytes.fromhex('f30f100d6085bf00'))],
}

args = sys.argv[1:]
on = {'mg': True, 'x2': True, 'logos': True, 'quotes': False}
for f in ('--no-mg', '--no-x2', '--no-logos'):
    if f in args: args.remove(f); on[f[5:]] = False
revert = '--revert' in args
if revert: args.remove('--revert')
if '--quotes' in args: args.remove('--quotes'); on['quotes'] = True
game = args[0] if args else '/var/data/Games/common/Catherine Classic'
exe, orig = os.path.join(game, 'Catherine.exe'), os.path.join(game, 'Catherine.exe.orig')
mg_dst = os.path.join(game, 'mg.dll')
mg_src = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'videohook', 'mg.dll')

sha = lambda p: hashlib.sha256(open(p, 'rb').read()).hexdigest() if os.path.isfile(p) else None
if sha(orig) == ORIG_SHA256:
    pass
elif sha(exe) == ORIG_SHA256:
    shutil.copy2(exe, orig)
else:
    sys.exit('no original Catherine.exe found (neither Catherine.exe nor .orig matches); '
             'verify the game files in Steam and run again')

if revert:
    shutil.copy2(orig, exe)
    if os.path.exists(mg_dst): os.remove(mg_dst)
    sys.exit(f'reverted {exe}')

d = bytearray(open(orig, 'rb').read())
for name, ps in PATCHES.items():
    if not on[name]: continue
    for off, old, new in ps:
        assert d[off:off+len(old)] == old, f'{name}: unexpected bytes at {off:#x}'
        d[off:off+len(new)] = new
open(exe, 'wb').write(d)
if on['mg']: shutil.copy2(mg_src, mg_dst)
elif os.path.exists(mg_dst): os.remove(mg_dst)
print('wrote', exe, 'with', ', '.join(n for n in PATCHES if on[n]) or 'no patches')
