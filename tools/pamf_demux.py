#!/usr/bin/env python3
"""Extract private_stream_1 substreams from a PS3 PAMF (.pam) file.
usage: pamf_demux.py IN.pam OUT_PREFIX   -> OUT_PREFIX.ac3 (sub 0x30), OUT_PREFIX.at3p.raw (sub 0x00)"""
import sys, struct
d = open(sys.argv[1], 'rb').read()
outs = {}
i = struct.unpack('>I', d[8:12])[0] * 2048 if d[:4] == b'PAMF' else 0
if i == 0 or i > len(d): i = 0x800
while i < len(d) - 6:
    if d[i:i+3] != b'\0\0\1': i += 1; continue
    sid = d[i+3]
    if sid == 0xBA: i += 14 + (d[i+13] & 7); continue
    L = struct.unpack('>H', d[i+4:i+6])[0]
    if sid == 0xBD:
        hl = d[i+8]; p = d[i+9+hl:i+6+L]
        outs.setdefault(p[0], bytearray()).extend(p[4:])
    i += 6 + L
ext = {0x30: '.ac3', 0x00: '.at3p.raw'}
for k, v in outs.items():
    open(sys.argv[2] + ext.get(k, f'.sub{k:02x}'), 'wb').write(v)
    print(hex(k), len(v))
