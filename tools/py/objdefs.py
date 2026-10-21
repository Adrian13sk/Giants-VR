"""Prototype: object-type registry from xx_*.bin definition streams + placement scan."""
import glob, os, re, struct, sys

def registry(bin_dir):
    meshes = {}   # meshId -> name
    groups = {}   # groupId -> first LOD mesh id
    types = {}    # typeId -> (groupId, file)
    for f in sorted(glob.glob(os.path.join(bin_dir, '*.bin'))):
        d = open(f, 'rb').read()
        # mesh/group entries: u32 A, name\0 (pad to 4), u32 G, 0x51, u32 A
        for m in re.finditer(rb'([\x20-\x7e]{2,40})\x00', d):
            s, e = m.start(), m.end()
            if s < 4: continue
            A = struct.unpack_from('<I', d, s - 4)[0]
            if not (0 < A < 100000): continue
            for pad in range(0, 4):
                g = e + pad
                if g + 9 > len(d): break
                if d[g + 4] == 0x51 and struct.unpack_from('<I', d, g + 5)[0] == A:
                    G = struct.unpack_from('<I', d, g)[0]
                    meshes[A] = m.group(1).decode()
                    groups.setdefault(G, A)
                    break
        for G in list(groups):
            pat = b'\x01' + struct.pack('<I', G)
            for m in re.finditer(re.escape(pat), d):
                s = m.start()
                if s < 4: continue
                T = struct.unpack_from('<I', d, s - 4)[0]
                if 0 < T < 100000 and T not in types:
                    types[T] = (G, os.path.basename(f))
    return meshes, groups, types

if __name__ == '__main__':
    B = sys.argv[1]
    meshes, groups, types = registry(B)
    print(len(meshes), 'meshes', len(groups), 'groups', len(types), 'types')
    for t in (1001, 1092, 1298, 1290, 1379, 522, 1004):
        if t in types:
            G, f = types[t]; print(t, f, G, meshes.get(groups.get(G)))
