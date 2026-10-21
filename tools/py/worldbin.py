"""Reference walker for w_*.bin world opcode streams (magic 0x1A0002E5)."""
import struct
# total record size incl. opcode byte; None = variable; 'T' = terminator; 'E' = error
SIZES = {19:2,23:5,29:17,30:17,31:17,32:17,33:17,34:17,35:'T',36:17,37:17,38:17,39:18,40:4,41:12,42:21,
 43:5,44:'T',45:38,46:13,47:29,48:19,49:41,50:6,52:45,53:45,54:45,57:29,58:37,59:2,60:17,61:'T',62:21,
 63:9,64:26,65:9,66:93,67:'T',68:1,69:1,70:29,71:21,72:13,73:5,74:1,76:7,77:1,78:34,79:65,80:None,
 81:13,82:5,83:5,84:45,88:12,89:None,90:13,91:13,92:38,93:33,94:33,95:13,96:5,97:9,98:9,99:1,100:1,
 101:13,102:9,103:None,104:None,106:4,107:5,108:'T',109:'T',110:'T',111:65,112:'T',113:34,114:45,
 115:45,116:45,117:5,118:None,119:49,120:49,121:93,129:5,130:33,132:None,133:45,134:45,135:45,
 136:45,137:46,138:46}

def strz(b, p):
    e = b.index(b'\0', p); return b[p:e].decode('latin1'), e + 1

def walk(d):
    hdr = struct.unpack_from('<8I', d, 0)
    assert hdr[0] == 0x1A0002E5
    off = hdr[1]
    size, = struct.unpack_from('<I', d, off)
    s = d[off + 4: off + 4 + size]
    p = 0
    a, p = strz(s, p); b, p = strz(s, p)
    recs = []
    while p < len(s):
        op = s[p]
        if op == 0xFF: recs.append((p, op, b'')); break
        sz = SIZES.get(op, 'E')
        if sz == 'E': recs.append((p, op, 'UNKNOWN')); break
        if sz == 'T': recs.append((p, op, b'')); break
        if sz is None:
            if op == 80: sz = 2 + 4 * s[p + 1]
            elif op in (103, 104): sz = 5 + 4 * struct.unpack_from('<I', s, p + 1)[0]
            elif op in (89, 132): sz = s.index(b'\0', p + 1) + 1 - p
            elif op == 118: sz = s.index(b'\0', p + 1) + 1 - p + 20
        recs.append((p, op, s[p + 1:p + sz])); p += sz
    return (a, b), recs, len(s)

if __name__ == '__main__':
    import sys, collections
    d = open(sys.argv[1], 'rb').read()
    (a, b), recs, n = walk(d)
    print(a, b, 'records', len(recs), 'end', recs[-1][0], 'of', n, 'last op', recs[-1][1])
    print(collections.Counter(r[1] for r in recs))
