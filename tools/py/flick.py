"""Reference dumper for cutscene scripts (flk_*.bin), from the original interpreter's cursor
advances and field uses (decompiled FLK_FlickProcess / flick_file_load, used as references).

    header (24 B): u32 version=3, u32 totalSize, u32 nDirectors, u32 nActors, u32 nBlocks,
                   u32 nextFlickNameOffset (0 = last shot)
    records from 0x18: {u32 op, u32 sizeInDwords (incl. these 2), payload...}; string fields are
                   byte offsets from the file start (0 = none). Op 5 yields (ends a frame's run).
Known ops: 1 director {idx, nameOff}; 2 actor {director, nameOff, actorIdx, ...}; 3 node bind;
4 camera {director, animNameOff, nodeNameOff}; 7 value ramp {target, seconds}; 8 wait time
{seconds}; 9/10 wait anim frame/tick; 0xB/0xC actor show/hide; 0x1A/0x1B sound play/stop;
0x1C callback(str); 0x20 actor offset; 0x25 screen fade {r,g,b,alpha,seconds}; 0x2C stream(str);
0x2F subtitle(str key into GText); 0x34 at the camera clip's end: stream(str).
"""
import struct, sys

def cstr(d, off):
    if off <= 0 or off >= len(d): return None
    e = d.find(b'\0', off)
    return d[off:e].decode('latin1')

def parse(d):
    ver, total, nd, na, nb, nxt = struct.unpack_from('<6I', d, 0)
    ops = []
    p = 0x18
    while p + 8 <= len(d):
        op, sz = struct.unpack_from('<2I', d, p)
        if sz == 0 or op > 0x40: break
        words = list(struct.unpack_from('<%dI' % (sz - 2), d, p + 8)) if sz > 2 else []
        ops.append((p, op, words))
        p += 4 * sz
        if op == 0: break
    return {'version': ver, 'total': total, 'directors': nd, 'actors': na, 'blocks': nb,
            'next': cstr(d, nxt) if nxt else None, 'ops': ops, 'end': p}

def f32(w): return struct.unpack('<f', struct.pack('<I', w))[0]

STR_FIELDS = {1: [1], 2: [1], 3: [1, 3], 4: [1, 2], 0x19: [1], 0x1C: [0], 0x28: [0], 0x2C: [0], 0x2F: [0], 0x34: [0]}
FLOAT_OPS = {7, 8, 0x1F, 0x20, 0x25, 0x2A}

def describe(d, op, w):
    out = []
    for i, v in enumerate(w):
        if i in STR_FIELDS.get(op, []):
            out.append(repr(cstr(d, v)))
        elif op in FLOAT_OPS and v > 0xFFFF:
            out.append('%.3g' % f32(v))
        else:
            out.append(str(v if v < 0x80000000 else v - (1 << 32)))
    return ' '.join(out)

if __name__ == '__main__':
    for path in sys.argv[1:]:
        d = open(path, 'rb').read()
        f = parse(d)
        print(path, {k: v for k, v in f.items() if k != 'ops'}, len(d))
        for p, op, w in f['ops']:
            print('  %5d op %2d (0x%02x) %s' % (p, op, op, describe(d, op, w)))
