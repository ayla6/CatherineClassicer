#!/usr/bin/env python3
"""Rebuild Catherine Classic's cutscenes from the PS3 masters.

Output files are MP4s (named .wmv so the game finds them) with:
  * video re-encoded from the PS3 .pam files (--codec h264/x265/av1) (true 23.976 fps, PS3 colour levels)
  * English: the PS3 audio (AC-3 5.1 copied as-is; ATRAC3plus-only files -> Opus)
  * Japanese: the *same* video bytes + the JP PC audio (WMA -> Opus, since MP4 can't hold WMA)
Video is stored first at a fixed 4K-aligned offset, so EN/JP pairs share
their video blocks on btrfs (done by `dedupe` with FIDEDUPERANGE).

Subcommands
  convert   build EN + JP files into OUT (mirrors data/movie, movie2, ...)
            resumable: progress is kept in OUT/.movies-state.json, so just rerun it
  pause / resume / stop   tell a running convert to wait / continue / quit
            *after the current video finishes* (it checks a flag file in OUT)
  jp-audio  later: swap the JP audio for JP PS3 audio, reusing the EN video
  dedupe    share identical blocks between EN and JP files in a data dir

Examples
  ./movies.py convert --only 002 011                 # test a couple into ./out
  ./movies.py convert --codec x265 --out ...           # pick the video encoder
  ./movies.py pause --out ...   /   resume   /   stop
  ./movies.py convert --out "$GAME/data"              # everything, in place
  ./movies.py jp-audio /path/to/JP_PS3/USRDIR/movie --data "$GAME/data"
"""
import argparse, fcntl, json, os, shutil, signal, struct, subprocess, sys, tempfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / 'tools'))
from mp4_flatten import main as flatten  # noqa: E402

GAME = Path('/var/data/Games/common/Catherine Classic')
PS3 = Path('/var/data/Games/Emulated/rpcs3/dev_hdd0/game/NPUB30698/USRDIR/movie')
DIRS = [('movie', 'movie_jp'), ('movie2', 'movie2_jp')]
# movie/NNN files that are byte-identical in movie_jp on PC: no separate JP
# version exists, so JP is always a reflink of EN and jp-audio never touches them.
SAME_EN_JP = {f'movie/{n}' for n in """
    026 029 030 031 032 033 034 035 036 150
    200 201 204 205 206 208 209 210 211 212
    213 214 215 216 217 218 219 220 221 222
    223 224 225 226 227 228 229 234 238 239
    240 241 310 311 312 313 314 316 320 321
""".split()}
# Different files on PC, but the EN and JP audio is the same (Chromaprint
# fingerprint bit-error 0.000-0.006; voiced scenes score 0.32+). JP gets the
# EN file (PS3 audio) as a reflink, same as SAME_EN_JP. 021 (0.05) and 026
# (0.12) are close but not listed: check by ear before adding them.
SAME_AUDIO = {f'movie/{n}' for n in """
    202 203 207 230 231 232 233 235 236 237
    242 243 244 245 246 300 301 302 303 304
    305 306 307 308 309 317 318 319
""".split()} | {'movie2/000'}
TMP = None  # set from --out
MAX_LEN_DIFF = 1.0  # s; PS3 vs PC length mismatch beyond this -> keep PC (e.g. credits)

# PS3 colour levels are kept as-is (they're what the PS3 showed).
COLOR = ['-color_range', 'tv', '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709']
# Video encoder profiles. Changing a profile's args makes existing outputs stale
# (the state file remembers what each file was built with).
PROFILES = {
    'h264': ['-pix_fmt', 'yuv420p', '-c:v', 'libx264', '-preset', 'slow', '-crf', '18', '-tune', 'animation'],
    'x265': ['-pix_fmt', 'yuv420p10le', '-c:v', 'libx265', '-preset', 'medium', '-crf', '18', '-tune', 'animation',
             '-tag:v', 'hvc1', '-x265-params', 'log-level=error:no-sao=1'],
    'av1':  ['-pix_fmt', 'yuv420p', '-c:v', 'libsvtav1', '-crf', '22', '-preset', '6',
             '-svtav1-params', 'enable-tf=0'],
}
VIDEO = None  # set from --codec
# Opus is VBR by default; compression_level 10 = best quality. Every source here
# is 48 kHz (Opus' native rate), so opus_args() pins 48 kHz and nothing gets
# resampled; for any other rate it would use SoX's resampler (soxr) explicitly.
OPUS = ['-c:a', 'libopus', '-b:a', '128k', '-vbr', 'on', '-compression_level', '10']
OPUS51 = ['-c:a', 'libopus', '-mapping_family', '1', '-b:a', '256k']


os.environ.setdefault('SVT_LOG', '1')  # quiet SVT-AV1 banner


NOISE = ('vmaf', 'model file', 'model from path')  # this system's libx265 spams these; harmless


def ff(*args):
    p = subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-y', *map(str, args)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors='replace')
    for line in p.stderr.splitlines():
        if not any(n in line for n in NOISE):
            print('\n  ERROR (ffmpeg):', line, file=sys.stderr)
    if p.returncode:
        raise subprocess.CalledProcessError(p.returncode, 'ffmpeg')


def duration(path, fmt=None):
    cmd = ['ffprobe', '-v', 'error', *(['-f', fmt] if fmt else []),
           '-show_entries', 'format=duration', '-of', 'csv=p=0', str(path)]
    return float(subprocess.run(cmd, capture_output=True, text=True).stdout.strip() or 0)


# ---------- PS3 PAMF ----------

def pamf_substreams(path):
    """private_stream_1 payloads by substream id (0x30 AC-3, 0x00 ATRAC3plus)."""
    d = Path(path).read_bytes()
    out = {}
    i = struct.unpack('>I', d[8:12])[0] * 2048 if d[:4] == b'PAMF' else 0
    while i < len(d) - 6:
        if d[i:i+3] != b'\0\0\1':
            i += 1; continue
        sid = d[i+3]
        if sid == 0xBA:
            i += 14 + (d[i+13] & 7); continue
        L = struct.unpack('>H', d[i+4:i+6])[0]
        if sid == 0xBD:
            hl = d[i+8]; p = d[i+9+hl:i+6+L]
            out.setdefault(p[0], bytearray()).extend(p[4:])
        i += 6 + L
    return out


def at3p_to_oma(raw, dst):
    """Wrap PAMF ATRAC3plus frames (8-byte 0x0FD0 headers) in an OMA file ffmpeg reads."""
    params = struct.unpack('>H', raw[2:4])[0]
    fs = (params & 0x3FF) * 8 + 8
    frames = b''.join(raw[i+8:i+8+fs] for i in range(0, len(raw), fs + 8)
                      if raw[i:i+2] == b'\x0f\xd0')
    hdr = bytearray(96)
    hdr[0:3] = b'EA3'; hdr[3] = 1; hdr[4:6] = b'\x00\x60'; hdr[6:8] = b'\xff\xff'
    hdr[32] = 1; hdr[33:36] = struct.pack('>I', params)[1:]
    Path(dst).write_bytes(bytes(hdr) + frames)


def audio_rate(input_args):
    fmt = input_args[input_args.index('-f') + 1] if '-f' in input_args else None
    src = input_args[input_args.index('-i') + 1]
    cmd = ['ffprobe', '-v', 'error', *(['-f', fmt] if fmt else []), '-select_streams', 'a:0',
           '-show_entries', 'stream=sample_rate', '-of', 'csv=p=0', str(src)]
    return int(subprocess.run(cmd, capture_output=True, text=True).stdout.strip() or 0)


def opus_args(input_args):
    """Opus settings for this input: no resampling at 48 kHz, soxr otherwise."""
    if audio_rate(input_args) == 48000:
        return OPUS + ['-ar', '48000']
    return OPUS + ['-af', 'aresample=resampler=soxr:precision=28', '-ar', '48000']


def ps3_audio(pam, tmp):
    """-> (input args, codec args) for the best audio track in a .pam."""
    subs = pamf_substreams(pam)
    if 0x30 in subs:
        p = Path(tmp) / 'a.ac3'; p.write_bytes(subs[0x30])
        return ['-i', p], ['-c:a', 'copy']
    if 0x00 in subs:
        p = Path(tmp) / 'a.oma'; at3p_to_oma(subs[0x00], p)
        ain = ['-f', 'oma', '-i', p]
        return ain, opus_args(ain)
    # 024 and 200-246 have no audio on PS3; the PC files carry a silent track,
    # so add one too (a few KB) in case the game expects an audio stream
    dur = duration(pam, 'mpeg')
    return ['-f', 'lavfi', '-t', f'{dur:.3f}', '-i', 'anullsrc=r=48000:cl=stereo'], OPUS + ['-ar', '48000']


# ---------- building ----------

def mux(video, audio_in, audio_codec, out, tmp):
    """video: (input args, codec args) giving stream 0:v; audio is input 1."""
    vin, vcod = video
    t = Path(tmp) / 'mux.mp4'
    ff(*vin, *audio_in, '-map', '0:v', '-map', '1:a', *vcod, *audio_codec, '-f', 'mp4', t)
    out.parent.mkdir(parents=True, exist_ok=True)
    flatten(str(t), str(out) + '.part')
    os.replace(str(out) + '.part', out)


def video_from(existing):
    """Input args that reuse the (byte-identical) video stream of a built file."""
    return ['-i', existing], ['-c:v', 'copy']


def pc_file(data, d, n):
    p = data / d / f'{n}_00.wmv'
    return p if p.exists() else None


# ---------- dedupe (FIDEDUPERANGE) ----------

FIDEDUPERANGE = 0xC0189436
CHUNK = 16 << 20  # btrfs limit per call
BS = 4096


def dedupe_pair(a, b):
    """Share every identical 4K-aligned block run between files a and b."""
    shared = 0
    with open(a, 'rb') as fa, open(b, 'r+b') as fb:
        n = min(os.fstat(fa.fileno()).st_size, os.fstat(fb.fileno()).st_size) // BS
        run = None
        for i in range(n + 1):
            same = i < n and fa.read(BS) == fb.read(BS)
            if same and run is None:
                run = i
            if not same and run is not None:
                off, end = run * BS, i * BS
                while off < end:
                    ln = min(CHUNK, end - off)
                    arg = struct.pack('=QQHHI', off, ln, 1, 0, 0) + struct.pack('=qQQiI', fb.fileno(), off, 0, 0, 0)
                    buf = bytearray(arg)
                    fcntl.ioctl(fa.fileno(), FIDEDUPERANGE, buf, True)
                    _, _, done, status, _ = struct.unpack('=qQQiI', buf[24:56])
                    if status < 0:
                        raise OSError(-status, f'dedupe failed: {b}')
                    shared += done
                    off += ln
                run = None
    return shared


def dedupe_dir(data):
    total = 0
    for en_d, jp_d in DIRS:
        for jp in sorted((data / jp_d).glob('*_00.wmv')):
            en = data / en_d / jp.name
            if en.exists():
                total += dedupe_pair(en, jp)
    print(f'deduped {total / 2**20:.0f} MiB')


# ---------- state, pause, cleanup ----------

class State:
    """OUT/.movies-state.json: what's done and with which settings. Written after every file."""
    def __init__(self, out):
        self.path = out / '.movies-state.json'
        try:
            self.d = json.loads(self.path.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            self.d = {'done': {}}

    def is_done(self, key, out_file, settings):
        e = self.d['done'].get(key)
        return bool(e) and e.get('settings') == settings and Path(out_file).exists()

    def mark(self, key, settings, **info):
        self.d['done'][key] = {'settings': settings, 'at': time.strftime('%F %T'), **info}
        tmp = self.path.with_suffix('.tmp')
        tmp.write_text(json.dumps(self.d, indent=1, sort_keys=True))
        os.replace(tmp, self.path)


def flag(out, name):
    return Path(out) / f'.movies-{name}'


def between_videos(out):
    """Called after each video: honour stop/pause requests from the CLI."""
    if flag(out, 'stop').exists():
        flag(out, 'stop').unlink()
        print('stop requested: quitting (rerun the same command to continue)', flush=True)
        return False
    if flag(out, 'pause').exists():
        print(f'[{time.strftime("%T")}] paused; run `movies.py resume` to continue', flush=True)
        while flag(out, 'pause').exists():
            if flag(out, 'stop').exists():
                return between_videos(out)
            time.sleep(2)
        print(f'[{time.strftime("%T")}] resumed', flush=True)
    return True


def cleanup(out):
    """Remove leftovers from a run that was killed mid-file."""
    for en_d, jp_d in DIRS:
        for d in (en_d, jp_d):
            for p in (out / d).glob('*.part'):
                p.unlink()
    for p in out.glob('.movies-tmp*'):
        if p.is_dir():
            shutil.rmtree(p, ignore_errors=True)


def running(out):
    try:
        os.kill(int(flag(out, 'running').read_text()), 0)
        return True
    except (FileNotFoundError, ValueError, ProcessLookupError):
        return False


def cmd_flag(a, name):
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    if name == 'resume':
        flag(out, 'pause').unlink(missing_ok=True)
        print('resuming')
    else:
        flag(out, name).touch()
        print(f'will {name} after the current video finishes')
    if not running(out):
        print(f'(no convert seems to be running for {out}; the flag stays until one does)')


def relink_same(out):
    """Make JP a reflink of EN wherever the audio is the same (also fixes files
    built before a key was added to SAME_AUDIO)."""
    for key in sorted(SAME_EN_JP | SAME_AUDIO):
        d, n = key.split('/')
        en, jp = out / d / f'{n}_00.wmv', out / f'{d}_jp' / f'{n}_00.wmv'
        if en.exists() and jp.exists() and subprocess.run(['cmp', '-s', en, jp]).returncode:
            subprocess.run(['cp', '--reflink=auto', en, jp], check=True)
            print(f'  {d}_jp/{n}: same audio as EN, now a reflink', flush=True)


def fmt_t(sec):
    return time.strftime('%H:%M:%S', time.gmtime(max(0, sec)))


# ---------- commands ----------

def cmd_convert(a):
    global TMP, VIDEO
    src_data = Path(a.game) / 'data'
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    TMP = out
    if out.resolve() == src_data.resolve():
        sys.exit('--out must differ from --game/data: the PC files are the source for JP audio.\n'
                 'Point --game at an untouched copy (e.g. a reflink backup) to write in place.')
    VIDEO = COLOR + PROFILES[a.codec]
    settings = {'codec': a.codec, 'video': VIDEO}
    flag(out, 'running').write_text(str(os.getpid()))
    os.nice(a.nice)
    cleanup(out)
    state = State(out)

    pams = sorted(Path(a.ps3).glob('*.pam'))
    if a.only:
        pams = [p for p in pams if p.stem in a.only]
    # plan first, so we can show progress / ETA
    jobs, skipped = [], []
    for pam in pams:
        n = pam.stem
        for en_d, jp_d in DIRS:
            pc_en = pc_file(src_data, en_d, n)
            if not pc_en:
                continue
            dp, dc = duration(pam, 'mpeg'), duration(pc_en)
            if abs(dp - dc) > MAX_LEN_DIFF:
                skipped.append(f'{en_d}/{n}')
                continue
            jobs.append((pam, n, en_d, jp_d, pc_en, dp))
    todo = [j for j in jobs if not state.is_done(f'{j[2]}/{j[1]}', out / j[2] / j[4].name, settings)]
    total = sum(j[5] for j in todo)
    print(f'{len(jobs)} videos, {len(jobs) - len(todo)} already done, {len(todo)} to go '
          f'({fmt_t(total)} of footage), codec {a.codec}', flush=True)
    if skipped:
        print(f'keeping PC version (length differs from PS3): {" ".join(skipped)}', flush=True)

    t0, footage_done, failed = time.time(), 0.0, []
    try:
        for i, (pam, n, en_d, jp_d, pc_en, dp) in enumerate(todo, 1):
            key = f'{en_d}/{n}'
            en_out = out / en_d / pc_en.name
            jp_out = out / jp_d / pc_en.name
            pc_jp = pc_file(src_data, jp_d, n)
            ts = time.time()
            print(f'[{i}/{len(todo)}] {key} ({dp:.0f}s of video) ...', end=' ', flush=True)
            try:
                with tempfile.TemporaryDirectory(dir=TMP, prefix='.movies-tmp') as tmp:
                    ain, acod = ps3_audio(pam, tmp)
                    mux((['-f', 'mpeg', '-i', pam], VIDEO), ain, acod, en_out, tmp)
                    jp_kind = None
                    if pc_jp:
                        jp_out.parent.mkdir(parents=True, exist_ok=True)
                        if key in SAME_EN_JP or key in SAME_AUDIO or subprocess.run(['cmp', '-s', pc_en, pc_jp]).returncode == 0:
                            jp_kind = 'reflink'
                            subprocess.run(['cp', '--reflink=auto', en_out, jp_out], check=True)
                        else:
                            jp_kind = 'en-video+jp-pc-audio'
                            mux(video_from(en_out), ['-i', pc_jp], opus_args(['-i', pc_jp]), jp_out, tmp)
                            dedupe_pair(en_out, jp_out)
            except Exception as e:  # keep going; report at the end
                failed.append(key)
                print(f'FAILED: {e}', flush=True)
                for f in (en_out, jp_out):
                    Path(str(f) + '.part').unlink(missing_ok=True)
                continue
            secs = time.time() - ts
            footage_done += dp
            state.mark(key, settings, secs=round(secs, 1), footage=round(dp, 1),
                       bytes=en_out.stat().st_size, pc_bytes=pc_en.stat().st_size, jp=jp_kind)
            rate = (time.time() - t0) / footage_done
            print(f'done in {secs:.0f}s, {en_out.stat().st_size / 2**20:.1f} MiB (PC was {pc_en.stat().st_size / 2**20:.1f})'
                  f'  | ETA {fmt_t(rate * (total - footage_done))}', flush=True)
            if not between_videos(out):
                return
        relink_same(out)
    finally:
        flag(out, 'running').unlink(missing_ok=True)
        if failed:
            print(f'\n{len(failed)} FAILED (rerun to retry): {" ".join(failed)}', flush=True)
    dedupe_dir(out)


def cmd_jp_audio(a):
    data = Path(a.data)
    global TMP
    TMP = data
    for pam in sorted(Path(a.jp_ps3).glob('*.pam')):
        n = pam.stem
        for en_d, jp_d in DIRS:
            en, jp = data / en_d / f'{n}_00.wmv', data / jp_d / f'{n}_00.wmv'
            if not (en.exists() and jp.exists()) or f'{en_d}/{n}' in SAME_EN_JP | SAME_AUDIO:
                continue
            if Path(a.ps3, pam.name).exists() and subprocess.run(['cmp', '-s', pam, Path(a.ps3, pam.name)]).returncode == 0:
                continue  # identical to the EN master: the reflinked EN file is already right
            if abs(duration(pam, 'mpeg') - duration(en)) > MAX_LEN_DIFF + 8:
                print(f'skip {jp_d}/{n}: length mismatch'); continue
            print(f'{jp_d}/{n}: EN video + JP PS3 audio', flush=True)
            with tempfile.TemporaryDirectory(dir=TMP, prefix='.movies-tmp') as tmp:
                ain, acod = ps3_audio(pam, tmp)
                mux(video_from(en), ain, acod, jp, tmp)
    dedupe_dir(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('convert')
    c.add_argument('--game', default=str(GAME), help='source install (reads data/movie*)')
    c.add_argument('--ps3', default=str(PS3), help='PS3 USRDIR/movie with .pam files')
    c.add_argument('--out', default=str(HERE / 'out'), help='output data dir (default ./out)')
    c.add_argument('--only', nargs='*', help='only these numbers, e.g. 002 011')
    c.add_argument('--codec', choices=PROFILES, default='x265', help='video encoder profile (default x265: 10-bit, medium, CRF 18, no SAO)')
    c.add_argument('--nice', type=int, default=10, help='CPU niceness, so the PC stays usable (default 10)')
    for name in ('pause', 'resume', 'stop'):
        x = sub.add_parser(name)
        x.add_argument('--out', default=str(HERE / 'out'), help='same --out as the running convert')
    j = sub.add_parser('jp-audio')
    j.add_argument('jp_ps3', help='JP PS3 USRDIR/movie with .pam files')
    j.add_argument('--data', default=str(GAME / 'data'))
    j.add_argument('--ps3', default=str(PS3), help='EN PS3 movie dir (to skip identical files)')
    d = sub.add_parser('dedupe')
    d.add_argument('--data', default=str(GAME / 'data'))
    a = ap.parse_args()
    {'convert': cmd_convert, 'jp-audio': cmd_jp_audio, 'dedupe': lambda a: dedupe_dir(Path(a.data)),
     'pause': lambda a: cmd_flag(a, 'pause'), 'resume': lambda a: cmd_flag(a, 'resume'),
     'stop': lambda a: cmd_flag(a, 'stop')}[a.cmd](a)


if __name__ == '__main__':
    main()
