"""Reference decoder for mission setup streams (wm_<name>.bin).

Layout (verified: every wm_*.bin walks to its terminator at EOF, see main()):
    u32 version = 4
    { u8 op; payload } ...   op 2 terminates
Payload sizes come from the original loader's cursor advances (decompilation used as a
factual reference only); field meanings are provisional unless noted.

"cur" = the current object (set by ops 3/4, pushed/popped by 0x16/0x17).
"""
import os, struct, sys

def _u8(d, p): return d[p]
def _u32(d, p): return struct.unpack_from('<I', d, p)[0]
def _f32(d, p): return struct.unpack_from('<f', d, p)[0]
def _name(d, p): return d[p:p + 32].split(b'\0')[0].decode('latin1')

class Obj:
    def __init__(self, op, type_, pos, yaw, parent=None, mount=None):
        self.op, self.type, self.pos, self.angles = op, type_, pos, [yaw, 0.0, 0.0]
        self.parent, self.mount = parent, mount
        self.team = None; self.scale = None; self.flags = 0; self.flicks = []
        self.v6 = None; self.v7 = None; self.ai = {}; self.children = []

def parse(d):
    if len(d) < 5 or _u32(d, 0) != 4:
        raise ValueError('bad version')
    p = 4
    objs, mission, ops = [], {}, []
    cur = None
    stack = []
    while True:
        if p >= len(d): raise ValueError('ran past EOF')
        op = d[p]; q = p + 1
        ops.append(op)
        if op == 1: pass
        elif op == 2:
            return objs, mission, ops, q
        elif op in (3, 4):
            t = _u32(d, q); x, y, z, yaw = struct.unpack_from('<4f', d, q + 4); q += 20
            cur = Obj(op, t, (x, y, z), yaw)
            if op == 4:
                cur.angles[1], cur.angles[2] = struct.unpack_from('<2f', d, q); q += 8
            objs.append(cur); stack = []
        elif op == 5: cur.team = _u8(d, q); q += 1
        elif op == 6: cur.v6 = struct.unpack_from('<3I', d, q); q += 12
        elif op == 7: cur.v7 = _u32(d, q); q += 4
        elif op == 8: cur.flags |= _u8(d, q) & 0xF; q += 1
        elif op == 9: cur.scale = _f32(d, q); q += 4
        elif op == 0x0A:
            n = _u8(d, q); q += 1
            cur.ai['list0a'] = list(struct.unpack_from('<%dI' % n, d, q)); q += 4 * n
        elif op == 0x0B:
            mission['setup'] = struct.unpack_from('<4I', d, q); q += 16
        elif op == 0x0C: mission['passenger'] = _u32(d, q); q += 4
        elif op == 0x0D: mission['v0d'] = _u32(d, q); q += 4
        elif op == 0x0E:
            n = _u32(d, q); q += 4
            mission['items'] = list(struct.unpack_from('<%dI' % n, d, q)); q += 4 * n
        elif op in (0x0F, 0x10, 0x11, 0x12, 0x23):
            mission['v%02x' % op] = _u32(d, q); q += 4
        elif op in (0x13, 0x15, 0x19):
            mission['s%02x' % op] = _name(d, q); q += 32
        elif op == 0x14: cur.flicks.append(_name(d, q)); q += 32
        elif op == 0x16:
            t = _u32(d, q); a, b = d[q + 4], d[q + 5]; q += 6
            stack.append(cur)
            child = Obj(op, t, None, 0.0, parent=stack[-1], mount=(a, b))
            stack[-1].children.append(child)
            cur = child
        elif op == 0x17:
            cur = stack.pop()
        elif op == 0x18:
            cur.angles = list(struct.unpack_from('<3f', d, q)); q += 12
        elif op in (0x1A, 0x1B):
            cur.ai['v%02x' % op] = struct.unpack_from('<2I', d, q); q += 8
        elif op in (0x1C, 0x1D, 0x22, 0x24, 0x25):
            if op in (0x1C, 0x1D): cur.ai['f%02x' % op] = 1
            else: mission['f%02x' % op] = 1
        elif op in (0x1E, 0x1F):
            n = _u32(d, q); q += 4
            cur.ai['list%02x' % op] = list(struct.unpack_from('<%di' % n, d, q)); q += 4 * n
        elif op == 0x20: cur.ai['v20'] = _u8(d, q); q += 1
        elif op == 0x21: cur.ai['v21'] = _u32(d, q); q += 4
        elif op == 0x26: mission['v26'] = struct.unpack_from('<2I', d, q); q += 8
        else:
            raise ValueError('unknown op 0x%02x at %d' % (op, p))
        p = q

def main(B):
    from collections import Counter
    ok = bad = 0
    opc = Counter()
    for f in sorted(os.listdir(B)):
        if not (f.startswith('wm_') and f.endswith('.bin')): continue
        d = open(os.path.join(B, f), 'rb').read()
        try:
            objs, mission, ops, end = parse(d)
            opc.update(ops)
            nchild = sum(len(o.children) for o in objs)
            status = 'OK ' if end == len(d) else 'END@%d/%d' % (end, len(d))
            ok += end == len(d); bad += end != len(d)
            print('%-34s %s objs=%3d mounted=%2d %s' % (f, status, len(objs), nchild,
                  {k: v for k, v in mission.items() if k in ('setup', 's15', 's13', 's19')}))
        except Exception as e:
            bad += 1
            print('%-34s FAIL %s' % (f, e))
    print('ok', ok, 'bad', bad)
    print('ops', sorted(opc.items()))

if __name__ == '__main__':
    main(sys.argv[1])
