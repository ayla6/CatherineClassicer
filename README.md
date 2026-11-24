# CatherineClassicer

Fixes and space savings for Catherine Classic (PC) on Linux/Proton: dedupe the
install on btrfs, rebuild the cutscenes from the PS3 masters, fix the movie
flicker on NVIDIA, and a few exe patches.

Requires: btrfs, GNU coreutils `cp`, python3. Game dir defaults to
`/var/data/Games/common/Catherine Classic`; pass another path as the last argument.

## Dedupe (`dedupe-identical.py`)

Duplicate files become reflinks, so they share the same blocks on disk; nothing
is deleted. Supports `--dry-run`.

```sh
./dedupe-identical.py      # byte-identical files in data/ (~1.6 GB)
btrfs filesystem du -s "/var/data/Games/common/Catherine Classic/data"
```

The dedupe script can't tell whether files are already reflinked, so on an
install that's already done it still lists every duplicate. Re-linking them is
harmless. Use `btrfs filesystem du` to see what's actually shared.

### What gets deduped

- **movie / movie_jp, movie2 / movie2_jp:** 52 videos are byte-identical across
  languages (~1.2 GB). The rest are separate encodes (JP has extra frames and
  different lip-sync), so they can't be deduplicated. `151_00.wmv` exists only in `movie_jp`.
- **sound / sound_jp:** 98 files are identical (~350 MB). The voiced `evv_*`/`evt_*` files differ.
- **`*_x2.dds` (303) and `*_x2.sp2` (284):** 2x UI assets for 1440p/4K. They're
  upscales and look worse. You can just delete them (~2.5 GB): with the `x2`
  exe patch (`patch.py`, below) the game then loads the 1x versions correctly.
- **Misc duplicates:** ~95 small ones (sound banks, models, textures), ~47 MB.
- `data/test` and `EXTRAS` are left alone on purpose.

Space is only freed once nothing else (such as a snapshot) still references the old blocks.

Steam "verify files" brings back the `_x2` files and the original exe (re-run
`patch.py`). The reflinked identical files stay correct, because their contents
are unchanged.

## Cutscenes from the PS3 masters (`movies.py`)

The PC videos are 30 fps re-encodes of the PS3 masters, with padded frames,
a lower bitrate and flattened levels. `movies.py` rebuilds them from the PS3
`.pam` files as HEVC (x265 10-bit, medium, CRF 18, no SAO) + original audio
in an MP4 named `.wmv`. The game plays
these under Proton, because GStreamer detects the format from the contents.
English and JP files share one video stream, deduped on btrfs.

```sh
./movies.py convert --game ".../Catherine Classic_bak" --out work/out --only 002 011   # test
./movies.py convert --game ".../Catherine Classic_bak" --out ".../Catherine Classic/data"
./movies.py jp-audio /path/to/JP_PS3/USRDIR/movie     # later, if you get the JP PS3 version
```

- `--game` must be an untouched copy, because the PC JP audio is read from it.
- Credits (`030`–`036`, which differ in length) and videos with no PS3
  version are left as the PC files.
- PS3 videos that only have ATRAC3plus audio (e.g. `000`) get Opus. The JP
  PC audio becomes Opus 128k, since MP4 can't hold WMA.
- `tools/mp4_flatten.py` puts the video first at a 4K-aligned offset, so the
  EN/JP video blocks match. `movies.py dedupe` shares them with FIDEDUPERANGE.


## Movie flicker on NVIDIA (`videohook/`)

The game plays movies with Media Foundation's EVR into a child window. Under
Wine + DXVK on NVIDIA that second swapchain fights the game's own, and movies
flicker (fine on the iGPU). `videohook/mg.dll` replaces only the video
renderer: decoded frames are drawn into the game's own back buffer at
Present, and `GetCurrentImage` feeds the game's own movie texture (skip
prompt, pause background, subtitles, cursor). The real MF.dll still handles
decoding, audio and timing.

Only works on a Proton whose Media Foundation decodes through GStreamer
(tested: proton-cachyos 11). GE-Proton's newer winedmo pipeline has no
HEVC/AC-3 decoders and froze even on H.264 + Opus test files, so the HEVC
movies don't play there at all, hook or not.

```sh
videohook/build.sh        # builds mg.dll in the "fedora" distrobox (mingw32-gcc)
./patch.py                # installs it (see below)
./patch.py --revert
```
`videohook.log` next to the exe logs each movie. Touch `videohook.passthrough`
in the game dir to use the real EVR again (for comparing).

## Exe patches (`patch.py`)

`patch.py [--no-mg] [--no-x2] [--quotes] [GAME_DIR]` finds the original exe
(whichever of `Catherine.exe` / `Catherine.exe.orig` has the known SHA-256),
keeps it as `Catherine.exe.orig`, writes a patched `Catherine.exe` and copies
`videohook/mg.dll` next to it. `--revert` puts the original back and removes mg.dll.
It always starts from the original, so it's safe to re-run with other flags.

- **mg** (default): import `MF.dll` -> `MG.dll` + copy mg.dll, for the movie hook above.
- **x2** (default): with hi-res assets on, the path resolver at 0x63c840
  falls back from `name_x2.dds/.sp2` to `name.ext` but still reports "x2", so
  the 1x fallback's texture coords get halved. The patch makes it report
  whether the `_x2` file actually exists, so you can delete `_x2` files and
  the game loads the 1x ones correctly.
- **quotes** (opt-in): quote loading screens stay up at least 5 s even when
  loading is done (state machine at 0x63520f). This points that 5.0 at a
  -0.0 constant so they fade out right away; quote #99 keeps its 7 s.
