#!/usr/bin/env python
"""List every CUDA architecture embedded in an nvngx_*.dll.

Usage: python fatbin_walk.py <dll> [dll ...]

If the output does not list your card's architecture (sm_86 for RTX 30, say),
the DLL cannot run on it, whatever its description claims.
"""
import struct, os, sys, collections

MAGIC = b'\x50\xed\x55\xba'
KIND = {1: 'PTX', 2: 'ELF/cubin'}
SM_NAME = {75: 'Turing / RTX 20', 86: 'Ampere / RTX 30', 89: 'Ada / RTX 40',
           120: 'Blackwell / RTX 50'}

def walk(path):
    b = open(path, 'rb').read()
    print('=====', os.path.basename(path), '(%d bytes)' % len(b))
    tally = collections.Counter()
    pos = nfat = 0
    while True:
        pos = b.find(MAGIC, pos)
        if pos < 0:
            break
        nfat += 1
        hsz = struct.unpack_from('<H', b, pos + 6)[0]
        fat_size = struct.unpack_from('<Q', b, pos + 8)[0]
        e, end = pos + hsz, pos + hsz + fat_size
        while e < end - 8:
            kind = struct.unpack_from('<H', b, e)[0]
            ehsz = struct.unpack_from('<I', b, e + 4)[0]
            payload = struct.unpack_from('<Q', b, e + 8)[0]
            sm = struct.unpack_from('<I', b, e + 28)[0]
            if ehsz == 0 or ehsz > 4096:
                break
            tally[(KIND.get(kind, 'kind%d' % kind), sm)] += 1
            e += ehsz + payload
        pos += 4
    print('  fatbins:', nfat)
    for (k, sm), c in sorted(tally.items(), key=lambda x: (x[0][1], x[0][0])):
        print('    %-10s sm_%-4s %-22s : %d entries' % (k, sm, '(%s)' % SM_NAME.get(sm, '?'), c))
    if not tally:
        print('  no CUDA fatbins found -- is this an nvngx_*.dll?')
    return {sm for _, sm in tally}

if __name__ == '__main__':
    args = sys.argv[1:]
    if not args:
        print(__doc__.strip())
        sys.exit(2)
    for a in args:
        walk(a)
        print()
