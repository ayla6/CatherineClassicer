# CatherineClassicer

Scripts to shrink a Catherine Classic (PC) install on btrfs. They don't delete
anything: duplicate files become reflinks, so they share the same blocks on disk.

Requires: btrfs, GNU coreutils `cp`, python3. Game dir defaults to
`/var/data/Games/common/Catherine Classic`; pass another path as the last argument.
Every script supports `--dry-run`.

```sh
./replace-x2.sh            # _x2 textures/sprites -> reflink of 1x (~2.5 GB)
./dedupe-identical.py      # byte-identical files in data/ (~1.6 GB)
btrfs filesystem du -s "/var/data/Games/common/Catherine Classic/data"
```

Run `replace-x2.sh` first: afterwards each `_x2` file is identical to its base,
and the dedupe pass sees nothing extra to do.

The dedupe script can't tell whether files are already reflinked, so on an
install that's already done it still lists every duplicate. Re-linking them is
harmless. Use `btrfs filesystem du` to see what's actually shared.

## What and why

- **movie / movie_jp, movie2 / movie2_jp:** 52 videos are byte-identical across
  languages (~1.2 GB). The rest are separate encodes (JP has extra frames and
  different lip-sync), so they can't be deduplicated. `151_00.wmv` exists only in `movie_jp`.
- **sound / sound_jp:** 98 files are identical (~350 MB). The voiced `evv_*`/`evt_*` files differ.
- **`*_x2.dds` (303) and `*_x2.sp2` (284):** 2x UI assets for 1440p/4K. They're
  upscales and look worse, so they're replaced with the 1x versions. This is the
  risky part: fonts (`data/font/*_x2.dds`) and sprite sheets could render wrong.
  Restore single files from a backup if something breaks.
- **Misc duplicates:** ~95 small ones (sound banks, models, textures), ~47 MB.
- `data/test` and `EXTRAS` are left alone on purpose.

Space is only freed once nothing else (such as a snapshot) still references the old blocks.

Steam "verify files" will undo the `_x2` swap. The reflinked identical files stay
correct, though, because their contents are unchanged.

## Cutscenes from the PS3 masters (`movies.py`)

The PC videos are 30 fps re-encodes of the PS3 masters, with padded frames,
a lower bitrate and flattened levels. `movies.py` rebuilds them from the PS3
`.pam` files as HEVC (x265 10-bit, medium, CRF 18, no SAO) + original audio in an MP4 named `.wmv`. The game plays
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
videohook/install.sh      # mg.dll + exe import MF.dll -> MG.dll + quote patch; backup Catherine.exe.orig
videohook/install.sh --revert
```
`videohook.log` next to the exe logs each movie. Touch `videohook.passthrough`
in the game dir to use the real EVR again (for comparing).

## Quote screens (`patch-quotes.py`)

Loading screens with a quote stay up at least 5 s even when loading is done
(state machine at 0x63520f). The patch points that 5.0 at a -0.0 constant so
they fade out right away; quote #99 keeps its 7 s.
