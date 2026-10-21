"""Reference GBS decoder (prototype, mirrors src/assets/gbs.cpp) + archive lookup helper."""
import glob, os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import gzp

_index = None
def find(B, name):
    """Return the bytes of `name` from any archive in Bin directory B."""
    global _index
    if _index is None:
        _index = {}
        for f in sorted(glob.glob(os.path.join(B, '*.gzp'))):
            try: d, ents = gzp.read_gzp(f)
            except Exception: continue
            for k, e in ents.items(): _index.setdefault(k, (f, e))
    f, e = _index[name.lower()]
    d, _ = gzp.read_gzp(f)
    return gzp.extract(d, e)

def parse(b):
    o = 0
    def u32():
        nonlocal o; v = struct.unpack_from('<I', b, o)[0]; o += 4; return v
    def i32():
        nonlocal o; v = struct.unpack_from('<i', b, o)[0]; o += 4; return v
    assert u32() == 0xAA0100BE
    m = {'flags': u32()}
    n = u32(); m['pos'] = [struct.unpack_from('<3f', b, o + 12 * i) for i in range(n)]; o += 12 * n
    if m['flags'] & 1:
        m['nNormDefs'] = u32(); nw = u32(); m['normWords'] = struct.unpack_from('<%dH' % nw, b, o); o += 2 * nw
    nd = u32(); m['disp'] = struct.unpack_from('<%dH' % nd, b, o); o += 2 * nd
    if m['flags'] & 1: m['dispNorm'] = struct.unpack_from('<%dH' % nd, b, o); o += 2 * nd
    if m['flags'] & 2: m['uv'] = [struct.unpack_from('<2f', b, o + 8 * i) for i in range(nd)]; o += 8 * nd
    if m['flags'] & 4: m['rgb'] = [tuple(b[o + 3 * i:o + 3 * i + 3]) for i in range(nd)]; o += 3 * nd
    nm = u32(); m['max'] = [struct.unpack_from('<5i', b, o + 20 * i) for i in range(nm)]; o += 20 * nm
    ns = u32(); m['subs'] = []
    for _ in range(ns):
        s = {'name': b[o:o + 32].split(b'\0')[0].decode('latin1')}; o += 32
        s['maxIndex'] = u32(); s['tris'] = i32(); nw = i32()
        s['words'] = struct.unpack_from('<%dH' % nw, b, o); o += 2 * nw
        s['vstart'] = i32(); s['vcount'] = i32()
        if m['flags'] & 2:
            s['tex'] = b[o:o + 32].split(b'\0')[0].decode('latin1'); s['tex2'] = b[o + 32:o + 64].split(b'\0')[0].decode('latin1'); o += 64
        s['falloff'] = struct.unpack_from('<f', b, o)[0]; o += 4
        if m['flags'] & 4: s['blend'] = struct.unpack_from('<f', b, o)[0]; o += 4
        s['flags'] = u32(); s['emissive'], s['ambient'], s['diffuse'], s['specular'] = struct.unpack_from('<4I', b, o); o += 16
        s['power'] = struct.unpack_from('<f', b, o)[0]; o += 4
        m['subs'].append(s)
    m['trailing'] = len(b) - o
    return m
