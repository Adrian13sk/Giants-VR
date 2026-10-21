"""Reference GTI terrain decoder (format investigation / cross-check for the C++ loader)."""
import struct

def read_gti(g):
    magic, = struct.unpack_from('<I', g, 0)
    assert magic & 0xFFFFFF00 == 0x94965000, hex(magic)
    p = 4
    if (magic & 0xFF) >= 2:
        cc, = struct.unpack_from('<I', g, p); p += 4 + 0x34 * cc
    minx, minz, minh, maxh = struct.unpack_from('<4f', g, p)
    tx, tz = struct.unpack_from('<2I', g, p + 16)
    stretch, = struct.unpack_from('<f', g, p + 24)
    p += 44
    flags, matlo, mathi = g[p], g[p + 1], g[p + 2]
    p += 4 + 4 + 4
    name = g[p:p + 32].split(b'\0')[0].decode('latin1'); p += 32
    W, H = tx + 2, tz + 2
    height = [[None] * W for _ in range(H)]
    tri = [[0] * W for _ in range(H)]
    light = [[(0, 0, 0)] * W for _ in range(H)]
    x, z = 1, 1
    while z < tz + 1:
        run = struct.unpack_from('<b', g, p)[0]; p += 1
        if run < 0:
            x += -run
            while x >= tx + 1:
                x += 2 - W; z += 1
        else:
            for _ in range(run + 1):
                h, = struct.unpack_from('<f', g, p); t = g[p + 4]; p += 5
                lm = (0, 0, 0)
                if flags & 2: lm = tuple(g[p:p + 3]); p += 3
                if flags & 4: p += 1
                if z < H and x < W:
                    height[z][x] = h; tri[z][x] = t; light[z][x] = lm
                x += 1
                if x >= tx + 1: x = 1; z += 1
    return dict(minx=minx, minz=minz, minh=minh, maxh=maxh, tx=tx, tz=tz, stretch=stretch,
                flags=flags, name=name, height=height, tri=tri, light=light, consumed=p, size=len(g))
