#!/usr/bin/env python3
"""Rewrite an MP4/MOV so its media data isn't interleaved and dedupes well.

Layout:  ftyp | mdat [ track 1 samples | pad to 4K | track 2 samples ... ] | moov
The first track (video) starts at a fixed 4096-byte offset, so two files with
the same video track but different audio share all of their video blocks on btrfs.

usage: mp4_flatten.py IN.mp4 OUT.mp4
"""
import struct, sys

ALIGN = 4096
CONTAINERS = {b'moov', b'trak', b'mdia', b'minf', b'stbl', b'edts', b'dinf', b'udta'}


def parse(buf, off=0, end=None):
    end = len(buf) if end is None else end
    out = []
    while off + 8 <= end:
        size, typ = struct.unpack('>I4s', buf[off:off+8]); hdr = 8
        if size == 1:
            size = struct.unpack('>Q', buf[off+8:off+16])[0]; hdr = 16
        elif size == 0:
            size = end - off
        body = buf[off+hdr:off+size]
        kids = parse(buf, off+hdr, off+size) if typ in CONTAINERS else None
        out.append([typ, body, kids])
        off += size
    return out


def ser(boxes):
    out = bytearray()
    for typ, body, kids in boxes:
        payload = ser(kids) if kids is not None else body
        out += struct.pack('>I4s', 8 + len(payload), typ) + payload
    return bytes(out)


def find(boxes, typ):
    return next(b for b in boxes if b[0] == typ)


def track_samples(src, stbl):
    """Return list of (offset, size) for every sample, in decode order."""
    k = {b[0]: b for b in stbl[2]}
    sz = k[b'stsz'][1]
    fixed, n = struct.unpack('>II', sz[4:12])
    sizes = [fixed] * n if fixed else list(struct.unpack(f'>{n}I', sz[12:12+4*n]))
    if b'stco' in k:
        c = k[b'stco'][1]; cn = struct.unpack('>I', c[4:8])[0]
        chunks = struct.unpack(f'>{cn}I', c[8:8+4*cn])
    else:
        c = k[b'co64'][1]; cn = struct.unpack('>I', c[4:8])[0]
        chunks = struct.unpack(f'>{cn}Q', c[8:8+8*cn])
    s = k[b'stsc'][1]; sn = struct.unpack('>I', s[4:8])[0]
    stsc = [struct.unpack('>III', s[8+12*i:20+12*i]) for i in range(sn)]
    res, si = [], 0
    for ci, coff in enumerate(chunks, 1):
        per = next(e[1] for e in reversed(stsc) if e[0] <= ci)
        o = coff
        for _ in range(per):
            res.append((o, sizes[si])); o += sizes[si]; si += 1
    assert si == n, 'sample count mismatch'
    return res


def main(inp, outp):
    src = open(inp, 'rb').read()
    top = parse(src)
    ftyp = find(top, b'ftyp')
    moov = find(top, b'moov')
    traks = [b for b in moov[2] if b[0] == b'trak']
    # video first, then everything else, in original order
    def is_video(t):
        hdlr = find(find(t[2], b'mdia')[2], b'hdlr')[1]
        return hdlr[8:12] == b'vide'
    traks.sort(key=lambda t: not is_video(t))

    head = ser([ftyp])
    mdat_hdr = 16  # 64-bit size mdat header: fixed size whatever the length
    pos = head and len(head) + mdat_hdr
    pre_pad = (-pos) % ALIGN
    # put a 'free' box before mdat to push the video to an aligned offset
    if pre_pad and pre_pad < 8:
        pre_pad += ALIGN
    data = bytearray()
    base = len(head) + pre_pad + mdat_hdr
    for i, t in enumerate(traks):
        if i:
            data += b'\0' * ((-(base + len(data))) % ALIGN)
        stbl = find(find(find(t[2], b'mdia')[2], b'minf')[2], b'stbl')
        samples = track_samples(src, stbl)
        start = base + len(data)
        for o, s in samples:
            data += src[o:o+s]
        n = len(samples)
        # one chunk holding every sample
        for b in stbl[2]:
            if b[0] == b'stsc':
                b[1] = struct.pack('>IIIII', 0, 1, 1, n, 1)
            elif b[0] in (b'stco', b'co64'):
                b[0], b[1] = b'co64', struct.pack('>IIQ', 0, 1, start)
    moov[2] = [b for b in moov[2] if b[0] != b'trak'] + traks
    with open(outp, 'wb') as f:
        f.write(head)
        if pre_pad:
            f.write(struct.pack('>I4s', pre_pad, b'free') + b'\0' * (pre_pad - 8))
        f.write(struct.pack('>I4sQ', 1, b'mdat', mdat_hdr + len(data)))
        f.write(data)
        f.write(ser([moov]))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
