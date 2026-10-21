"""Prototype: find object placements for a world (world op 42 + mission op 0x03 records)."""
import glob, os, re, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import objdefs, worldbin

def strict_types(B, groups):
    types = {}
    for f in sorted(glob.glob(os.path.join(B, '*.bin'))):
        d = open(f, 'rb').read()
        for G in groups:
            for m in re.finditer(re.escape(b'\x01' + struct.pack('<I', G)), d):
                s = m.start() - 4
                if s < 3: continue
                T = struct.unpack_from('<I', d, s)[0]
                if 0 < T < 100000 and (b'\xff' in d[s - 3:s] or d[s - 3:s] == b'\x00\x00\x00'):
                    types.setdefault(T, G)
    return types

def missions_for_world(B, world):
    d = open(os.path.join(B, 'w_%s.bin' % world), 'rb').read()
    (_, _), recs, _ = worldbin.walk(d)
    out = []
    for p, op, pl in recs:
        if op == 92:
            nm = pl[5:5 + 32].split(b'\0')[0].decode('latin1')
            out.append((pl[0], nm))
    return out

def scan_spawns(path, types, bounds):
    d = open(path, 'rb').read()
    (x0, x1, y0, y1) = bounds
    res = []
    for i in range(len(d) - 21):
        if d[i] != 3: continue
        T, x, y, z, a = struct.unpack_from('<I4f', d, i + 1)
        if T in types and x0 <= x <= x1 and y0 <= y <= y1 and -500 < z < 3000:
            res.append((i, T, x, y, z, a))
    return res

if __name__ == '__main__':
    B, world = sys.argv[1], sys.argv[2]
    meshes, groups, _ = objdefs.registry(B)
    types = strict_types(B, groups)
    print(len(types), 'types')
    ms = missions_for_world(B, world)
    print('missions', ms)
    bounds = (-6000, 6000, -6000, 6000)
    for kind, nm in ms:
        f = os.path.join(B, 'wm_%s.bin' % nm)
        if not os.path.exists(f): print(' missing', f); continue
        sp = scan_spawns(f, types, bounds)
        print(' wm_%s: %d spawns' % (nm, len(sp)))
        for i, T, x, y, z, a in sp[:12]:
            print('   @%d type %d %-24s (%.0f, %.0f, %.1f) a=%.2f' % (i, T, meshes.get(groups[types[T]]), x, y, z, a))
