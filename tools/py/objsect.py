"""Reference parser for the object-definition section (section 3) of 0x1A0002E5 .bin files.

    u32 size; blob[size]                       (offsets below are relative to blob start)
    blob: u32 nMesh, u32 meshOff[nMesh]   -> {u32 meshId, strz name}
          u32 nX,    u32 x[nX]                 (unknown, 0 in every file seen so far)
          u32 nGroup,u32 groupOff[nGroup] -> {u32 groupId, group ops..., 0xFF}
          u32 nType, u32 typeOff[nType]   -> {u32 typeId, type ops...}
Group ops (sizes from the original loader, used as a factual reference):
    0x51 u32 mesh, f32 angle, f32 dist   LOD level        0x59/0x5A/0x5B u32 mesh  special LODs
    0x52 u8 slot, strz anm               animation        0x53 u8 slot, char[16]   anim name?
    0x54 u8, u8, char[16], u32 (22 B)    attachment       0x55                     flag 0x10
    0x56 u32,u32  0x57/0x58/0x5C u32,u32,u32              animation events
    0x7F/0x80 u32 tick, 5 strz fx names, u32      fx events (e.g. footsteps)
Type op 1 = u32 groupId (always the first op in the shipped data).
"""
import glob, os, struct, sys

def section3(d):
    if len(d) < 32 or struct.unpack_from('<I', d, 0)[0] != 0x1A0002E5:
        return None
    off = struct.unpack_from('<7I', d, 4)[3]
    if off + 4 > len(d): return None
    size = struct.unpack_from('<I', d, off)[0]
    if size == 0: return None
    return d[off + 4: off + 4 + size]

def _strz(b, p):
    e = b.index(b'\0', p); return b[p:e].decode('latin1'), e + 1

def parse_group(b, p):
    gid = struct.unpack_from('<I', b, p)[0]; p += 4
    lods, anims, ops = [], {}, []
    while True:
        op = b[p]; p += 1; ops.append(op)
        if op == 0xFF: break
        if op == 0x51:
            m, ang, dist = struct.unpack_from('<Iff', b, p); p += 12; lods.append((m, ang, dist))
        elif op in (0x59, 0x5A, 0x5B):
            lods.append((struct.unpack_from('<I', b, p)[0], None, {0x59: -3.0, 0x5A: -1.0, 0x5B: -2.0}[op])); p += 4
        elif op == 0x52:
            slot = b[p]; name, p = _strz(b, p + 1); anims[slot] = name
        elif op == 0x53: p += 17
        elif op == 0x54: p += 22
        elif op == 0x55: pass
        elif op == 0x56: p += 8
        elif op in (0x57, 0x58, 0x5C): p += 12
        elif op in (0x7F, 0x80):
            p += 4
            for _ in range(5): _, p = _strz(b, p)
            p += 4
        else:
            raise ValueError('group %d: unknown op 0x%02x' % (gid, op))
    return gid, lods, anims

def parse(b):
    p = 0
    def table():
        nonlocal p
        n = struct.unpack_from('<I', b, p)[0]; p += 4
        t = list(struct.unpack_from('<%dI' % n, b, p)); p += 4 * n
        return t
    meshes = {}
    for o in table():
        mid = struct.unpack_from('<I', b, o)[0]; meshes[mid] = _strz(b, o + 4)[0]
    xs = table()
    groups = {}
    for o in table():
        gid, lods, anims = parse_group(b, o); groups[gid] = (lods, anims)
    types = {}
    for o in table():
        tid = struct.unpack_from('<I', b, o)[0]
        types[tid] = walk_type(b, o + 4)
    return meshes, xs, groups, types

# Type-op sizes (bytes including the op) for ops whose cursor advance is fixed in the original
# loader; 'z' = op + f32 + strz, 's' = op + strz. Walking stops at any other op.
TYPE_OP_SIZE = {1: 5, 3: 13, 4: 9, 5: 5, 6: 5, 7: 5, 8: 9, 9: 5, 10: 5, 0x0B: 5, 0x0C: 13, 0x38: 13,
    0x0D: 5, 0x13: 2, 0x14: 1, 0x15: 5, 0x16: 1, 0x17: 5, 0x18: 1, 0x19: 5, 0x1A: 5, 0x1B: 1,
    0x1C: 9, 0x1D: 1, 0x1E: 2, 0x1F: 17, 0x20: 1, 0x21: 9, 0x22: 1, 0x23: 1, 0x24: 1, 0x3F: 1,
    0x43: 1, 0x25: 5, 0x26: 9, 0x27: 1, 0x28: 17, 0x29: 5, 0x2B: 1, 0x2E: 1, 0x2F: 1, 0x30: 5,
    0x31: 5, 0x32: 5, 0x37: 5, 0x3E: 5, 0x33: 9, 0x34: 9, 0x35: 9, 0x36: 2, 0x3B: 5, 0x3C: 17,
    0x42: 13, 0x45: 5, 0x69: 5, 2: 'z', 0x2C: 's', 0x2D: 's'}

def walk_type(b, p):
    """-> dict(group, ospec, scale, stop) from the leading fixed-size type ops."""
    info = {'group': None, 'ospec': None, 'scale': None, 'stop': None}
    for _ in range(64):
        op = b[p]
        if op == 1: info['group'] = struct.unpack_from('<I', b, p + 1)[0]
        elif op == 0x1E: info['ospec'] = b[p + 1]
        elif op == 0x17: info['scale'] = struct.unpack_from('<f', b, p + 1)[0]
        sz = TYPE_OP_SIZE.get(op)
        if sz is None:
            info['stop'] = op; break
        if sz == 'z': p = b.index(b'\0', p + 5) + 1
        elif sz == 's': p = b.index(b'\0', p + 1) + 1
        else: p += sz
    return info

def registry(B):
    M, G, T, src = {}, {}, {}, {}
    bad = []
    for f in sorted(glob.glob(os.path.join(B, '*.bin'))):
        d = open(f, 'rb').read()
        b = section3(d)
        if b is None: continue
        try:
            m, xs, g, t = parse(b)
        except Exception as e:
            bad.append((os.path.basename(f), str(e))); continue
        M.update(m); G.update(g)
        for k, v in t.items(): T[k] = v; src[k] = os.path.basename(f)
    return M, G, T, src, bad

if __name__ == '__main__':
    M, G, T, src, bad = registry(sys.argv[1])
    print(len(M), 'meshes', len(G), 'groups', len(T), 'types', 'bad', bad)
    from collections import Counter
    nog = [t for t, g in T.items() if g['group'] is None]
    print('types without op1:', len(nog), nog[:10])
    print('walk stops:', Counter(g['stop'] for g in T.values()).most_common(12))
    print('ospec found:', sum(g['ospec'] is not None for g in T.values()))
    unres = [t for t, g in T.items() if g['group'] is not None and (g['group'] not in G or not G[g['group']][0])]
    print('types whose group is missing/empty:', len(unres), unres[:20])
    for t in sys.argv[2:]:
        t = int(t); g = T.get(t, {}); lods, anims = G.get(g.get('group'), ([], {}))
        print(t, src.get(t), g, [M.get(l[0]) for l in lods][:3], sorted(anims.items())[:6])
