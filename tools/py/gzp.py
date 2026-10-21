"""Small Python reference reader for GZP archives (used for format investigation/tests)."""
import struct, sys

def lzss(src, expected):
    win = bytearray(b' ' * 4096); r = 0xFEE; flags = 0; i = 0; out = bytearray()
    n = len(src)
    while len(out) < expected:
        flags >>= 1
        if not flags & 0x100:
            if i >= n: break
            flags = src[i] | 0xFF00; i += 1
        if flags & 1:
            if i >= n: break
            c = src[i]; i += 1; out.append(c); win[r] = c; r = (r + 1) & 0xFFF
        else:
            if i + 1 >= n: break
            b1, b2 = src[i], src[i + 1]; i += 2
            pos = b1 | ((b2 & 0xF0) << 4); ln = (b2 & 0xF) + 3
            for k in range(ln):
                if len(out) >= expected: break
                c = win[(pos + k) & 0xFFF]; out.append(c); win[r] = c; r = (r + 1) & 0xFFF
    return bytes(out)

def read_gzp(path):
    d = open(path, 'rb').read()
    magic, off = struct.unpack_from('<II', d, 0)
    assert magic == 0x6608F101, hex(magic)
    _, cnt = struct.unpack_from('<II', d, off)
    p = off + 8; ents = {}
    for _ in range(cnt):
        size, usz, date, start, comp, nl = struct.unpack_from('<IIIIBB', d, p)
        name = d[p + 18:p + 18 + nl].split(b'\0')[0].decode('latin1'); p += 18 + nl
        ents[name.lower()] = (name, size, usz, start, comp)
    return d, ents

def extract(d, ent):
    name, size, usz, start, comp = ent
    payload = d[start + 16:start + size]
    if comp == 2: return payload[:usz]
    return lzss(payload, usz)

if __name__ == '__main__':
    d, ents = read_gzp(sys.argv[1])
    for k, e in ents.items(): print(e[0], e[2], e[4])
