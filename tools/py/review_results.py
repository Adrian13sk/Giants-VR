"""Review an on-device test run (devtest\\results\\<stamp>, as pulled by devtest.ps1) and summarise it on one
page (Markdown):

  - per test: status, fps, frame-time percentiles, CPU update / render time, GPU time, draw calls, memory,
    and the system's own VrApi numbers (fps, app GPU time, GPU load);
  - the worst frames of each test, with what they were waiting on (GPU, CPU, or neither: a hitch);
  - the costs some tests are built to measure: the sniper zoom pass (weapons_mecc) and the original fx.bin
    effects (fx_original);
  - a comparison with earlier runs of the same tests (the newest older run in the same results folder, or
    the runs given with --compare), so a slowdown shows up at once;
  - what needs a look: tests that didn't finish, crashes, errors, frame rate under 70, the adaptive
    quality dropping, and screenshots that look broken (nearly black, nearly uniform).

All statistics are recomputed from frames.csv where it exists (result.txt from older builds can carry
nonsense GPU times from the Adreno timer bug; times outside 0..200 ms are dropped).

    python3 tools/py/review_results.py <results\\stamp> [--compare <older results\\stamp> ...] [--no-history]
                                       [--out review.md]
"""
import csv
import glob
import os
import re
import statistics
import struct
import sys
import zlib

FRAME_MS = 1000.0 / 72.0
SLOW_MS = 1.5 * FRAME_MS


def png_stats(path):
    """Mean brightness and standard deviation of a PNG (8-bit RGB/RGBA, no interlace), subsampled."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        return None
    pos, w, h, ctype, idat = 8, 0, 0, 0, b''
    while pos < len(data):
        n, = struct.unpack('>I', data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            if depth != 8 or ctype not in (2, 6):
                return None
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    prev = bytearray(stride)
    vals = []
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        prev = line
        if y % 8 == 0:
            for x in range(0, w, 8):
                o = x * bpp
                vals.append((line[o] + line[o + 1] + line[o + 2]) / 3.0)
    return statistics.mean(vals), statistics.pstdev(vals)


def read(path):
    try:
        with open(path, encoding='utf-8-sig', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def pct(v, p):
    if not v: return None
    s = sorted(v)
    return s[min(len(s) - 1, int(p * (len(s) - 1) + 0.5))]


def mean(v):
    return sum(v) / len(v) if v else None


def fmt(x, d=1):
    return '-' if x is None else ('%.' + str(d) + 'f') % x


def delta(new, old, d=1, worse_if_higher=True, tol=0.0):
    """'+1.2' with a marker when it moved the bad way by more than tol."""
    if new is None or old is None: return '-'
    dv = new - old
    bad = (dv > tol) if worse_if_higher else (dv < -tol)
    return ('%+.' + str(d) + 'f') % dv + (' **(worse)**' if bad else '')


class Frames:
    """frames.csv rows: t, dt_ms, update_ms, render_cpu_ms, gpu_ms, draws[, screenshot_stall]."""

    def __init__(self, path):
        self.rows = []
        try:
            with open(path, encoding='utf-8-sig') as f:
                for r in csv.DictReader(f):
                    try:
                        g = float(r.get('gpu_ms', -1))
                        self.rows.append({
                            't': float(r['t']), 'dt': float(r['dt_ms']), 'upd': float(r['update_ms']),
                            'cpu': float(r['render_cpu_ms']), 'gpu': g if 0 <= g < 200 else None,
                            'draws': int(float(r['draws'])), 'stall': r.get('screenshot_stall', '0') not in ('0', '', None)})
                    except (KeyError, ValueError):
                        continue
        except OSError:
            pass
        # (Builds before the screenshot_stall column count screenshot stalls as ordinary frames.)

    def window(self, t0, t1, keep=None):
        return [r for r in self.rows if t0 <= r['t'] <= t1 and not r['stall'] and (keep is None or keep(r))]

    @staticmethod
    def stats(rows):
        dt = [r['dt'] for r in rows]
        g = [r['gpu'] for r in rows if r['gpu'] is not None]
        m = mean(dt)
        return {
            'n': len(rows), 'fps': 1000.0 / m if m else None, 'p50': pct(dt, 0.5), 'p95': pct(dt, 0.95), 'p99': pct(dt, 0.99),
            'max': max(dt) if dt else None, 'slow': sum(1 for x in dt if x > SLOW_MS),
            'upd': mean([r['upd'] for r in rows]), 'cpu': mean([r['cpu'] for r in rows]),
            'gpu': mean(g), 'gpu95': pct(g, 0.95), 'gpuN': len(g), 'draws': mean([r['draws'] for r in rows])}


def cause(r):
    if r['gpu'] is not None and r['gpu'] > FRAME_MS: return 'GPU %.1f ms' % r['gpu']
    if r['upd'] + r['cpu'] > FRAME_MS: return 'CPU %.1f ms (update %.1f, render %.1f)' % (r['upd'] + r['cpu'], r['upd'], r['cpu'])
    return 'hitch (CPU %.1f, GPU %s ms)' % (r['upd'] + r['cpu'], fmt(r['gpu']))


def vrapi(path):
    """Means of the system's once-a-second line (VrApi) over the middle of the run."""
    lines = [l for l in read(path).splitlines() if 'FPS=' in l]
    if len(lines) > 6: lines = lines[4:-1]
    fps, app, load = [], [], []
    for l in lines:
        m = re.search(r'FPS=(\d+)/', l)
        if m: fps.append(float(m.group(1)))
        m = re.search(r'App=([\d.]+)ms', l)
        if m: app.append(float(m.group(1)))
        m = re.search(r'GPU%=([\d.]+)', l)
        if m: load.append(float(m.group(1)))
    return {'fps': mean(fps), 'app': mean(app), 'load': mean(load), 'n': len(lines)} if lines else None


class Test:
    def __init__(self, root, name, summary):
        self.name = name
        d = os.path.join(root, name)
        self.dir = d
        self.res = read(os.path.join(d, 'autotest_out', 'result.txt'))
        self.log = read(os.path.join(d, 'logcat.txt'))
        m = re.search(r'^%s: (\w+)' % re.escape(name), summary, re.M)
        self.status = m.group(1) if m else ('done' if 'status ok' in self.res else 'unknown')
        m = re.search(r'^window ([\d.]+)-([\d.]+)', self.res, re.M)
        self.win = (float(m.group(1)), float(m.group(2))) if m else (3.0, 1e9)
        self.frames = Frames(os.path.join(d, 'autotest_out', 'frames.csv'))
        self.s = Frames.stats(self.frames.window(*self.win)) if self.frames.rows else self.from_result()
        mem = re.search(r'memory_kb rss (\d+) peak (\d+)', self.res)
        self.rss, self.peak = (int(mem.group(1)) / 1024.0, int(mem.group(2)) / 1024.0) if mem else (None, None)
        q = re.search(r'effects_quality_level (\d+)', self.res)
        self.quality = q.group(1) if q else None
        self.vr = vrapi(os.path.join(d, 'vrapi.txt'))
        self.errs = re.findall(r' E/GiantsVR.*', self.log)
        self.fatal = re.findall(r'(F/libc.*|FATAL EXCEPTION.*|SIGSEGV.*|Abort message.*)', self.log)

    def from_result(self):
        def f(pat):
            m = re.search(pat, self.res, re.M)
            return float(m.group(1)) if m else None
        g = f(r'gpu_ms mean ([\d.]+)')
        return {'n': 0, 'fps': f(r'^fps ([\d.]+)'), 'p50': f(r'p50 ([\d.]+)'), 'p95': f(r'frame_ms .*?p95 ([\d.]+)'),
                'p99': f(r'p99 ([\d.]+)'), 'max': f(r'frame_ms .*?max ([\d.]+)'), 'slow': f(r'slow_frames (\d+)'),
                'upd': f(r'update_ms mean ([\d.]+)'), 'cpu': f(r'render_cpu_ms mean ([\d.]+)'),
                'gpu': g if g is not None and g < 200 else None, 'gpu95': None, 'gpuN': 0, 'draws': f(r'draws mean ([\d.]+)')}


def load_run(root):
    summary = read(os.path.join(root, 'summary.txt'))
    names = sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d)) and d != 'sweep_scripts')
    apk = re.search(r'apk sha256 ([0-9a-f]{12})', summary)
    return {'root': root, 'stamp': os.path.basename(root.rstrip('/\\')), 'apk': apk.group(1) if apk else '?',
            'tests': {n: Test(root, n, summary) for n in names}}


# Costs that particular tests are built to measure: (label, window A, window B), compared within one run.
def special_costs(t):
    out = []
    F = t.frames
    if not F.rows: return out
    if t.name == 'weapons_mecc':
        # Sniper in hand with its zoom pass forced (3-10 s) against the mortar in hand, same spot, no zoom.
        a = Frames.stats(F.window(3.0, 10.5))
        b = Frames.stats(F.window(14.5, 18.5))
        out.append(('Sniper zoom pass (sniper 3-10.5 s vs mortar 14.5-18.5 s, same spot)', a, b))
    if t.name == 'fx_original':
        # GVR_FX_TEST starts an explosion about 1 s into play and every 3 s after: the 1.5 s after each
        # start against the 1.5 s before the next.
        lo, hi = max(2.0, t.win[0]), t.win[1]
        a = Frames.stats(F.window(lo, hi, keep=lambda r: ((r['t'] - 1.0) % 3.0) < 1.5))
        b = Frames.stats(F.window(lo, hi, keep=lambda r: ((r['t'] - 1.0) % 3.0) >= 1.5))
        out.append(('Original effects (1.5 s after each explosion vs the quiet 1.5 s between)', a, b))
    return out


def history(root, explicit, use_history):
    if explicit: return [load_run(r) for r in explicit]
    if not use_history: return []
    parent = os.path.dirname(os.path.abspath(root.rstrip('/\\')))
    me = os.path.basename(root.rstrip('/\\'))
    older = sorted(d for d in os.listdir(parent) if d < me and os.path.isdir(os.path.join(parent, d)) and re.match(r'\d{8}-\d{6}$', d))
    return [load_run(os.path.join(parent, d)) for d in older]


def main(argv):
    args, compare, use_history, out_path = [], [], True, None
    i = 0
    while i < len(argv):
        if argv[i] == '--compare': compare.append(argv[i + 1]); i += 2
        elif argv[i] == '--no-history': use_history = False; i += 1
        elif argv[i] == '--out': out_path = argv[i + 1]; i += 2
        else: args.append(argv[i]); i += 1
    if len(args) != 1:
        print(__doc__)
        return 2
    run = load_run(args[0])
    past = history(args[0], compare, use_history)
    tests = run['tests']
    o = ['# Device run %s (APK %s...)' % (run['stamp'], run['apk']), '']

    # Overview.
    o.append('| Test | Status | fps | frame ms p50 / p95 / p99 / max | slow | update ms | render CPU ms | GPU ms mean / p95 | draws | RSS / peak MB | VrApi fps, app GPU ms, load |')
    o.append('|---|---|---|---|---|---|---|---|---|---|---|')
    issues = []
    for n, t in tests.items():
        s = t.s
        vr = '%s, %s, %s' % (fmt(t.vr['fps']), fmt(t.vr['app'], 2), fmt(t.vr['load'], 2)) if t.vr else '-'
        o.append('| %s | %s | %s | %s / %s / %s / %s | %s | %s | %s | %s / %s | %s | %s / %s | %s |' % (
            n, t.status, fmt(s['fps']), fmt(s['p50']), fmt(s['p95']), fmt(s['p99']), fmt(s['max'], 0), fmt(s['slow'], 0),
            fmt(s['upd'], 2), fmt(s['cpu'], 2), fmt(s['gpu'], 2), fmt(s['gpu95'], 2), fmt(s['draws'], 0), fmt(t.rss, 0),
            fmt(t.peak, 0), vr))
        if t.status != 'done': issues.append('**%s** did not finish (%s).' % (n, t.status))
        if t.fatal: issues.append('**%s** crashed: `%s`' % (n, t.fatal[0][:160]))
        if t.errs: issues.append('**%s**: %d error lines, first: `%s`' % (n, len(t.errs), t.errs[0][:160]))
        if s['fps'] is not None and s['fps'] < 70: issues.append('**%s**: %.1f fps.' % (n, s['fps']))
        if t.quality and t.quality != '0': issues.append('**%s**: effects quality dropped to level %s.' % (n, t.quality))
        if 'gpu_ms mean' in t.res and re.search(r'gpu_ms mean \d{6,}', t.res):
            issues.append('**%s**: result.txt GPU times are timer garbage (old build); recomputed from frames.csv.' % n)
    o.append('')
    o.append('Slow = frames over %.1f ms (1.5 refresh intervals at 72 Hz). Screenshot stalls are left out.' % SLOW_MS)

    # Measured costs.
    costs = [(n, c) for n, t in tests.items() for c in special_costs(t)]
    if costs:
        o += ['', '## Measured costs', '']
        o.append('| Test | What | with: fps, frame p95, GPU ms | without: fps, frame p95, GPU ms | GPU cost ms |')
        o.append('|---|---|---|---|---|')
        for n, (label, a, b) in costs:
            gc = (a['gpu'] - b['gpu']) if a['gpu'] is not None and b['gpu'] is not None else None
            o.append('| %s | %s | %s, %s, %s | %s, %s, %s | %s |' % (
                n, label, fmt(a['fps']), fmt(a['p95']), fmt(a['gpu'], 2), fmt(b['fps']), fmt(b['p95']), fmt(b['gpu'], 2),
                ('%+.2f' % gc) if gc is not None else '-'))

    # GPU time per offscreen pass (result.txt "gpu_passes", APKs from 2026-10-28 on).
    rows = []
    for n, t in tests.items():
        m = re.search(r'^gpu_passes (frame .*)$', t.res, re.M)
        if not m: continue
        v = dict(re.findall(r'([a-z]+) ([\d.]+)', m.group(1)))
        rows.append('| %s | %s | %s | %s | %s | %s | %s | %s |' % (
            n, v.get('frame', '-'), v.get('shadow', '-'), v.get('haze', '-'), v.get('reflection', '-'), v.get('scope', '-'),
            v.get('screen', '-'), v.get('eyes', '-')))
    if rows:
        o += ['', '## GPU time per pass (mean ms; "eyes" = the rest of the frame: the eye pass itself)', '']
        o.append('| Test | frame | shadow | haze | reflection | scope | screen | eyes |')
        o.append('|---|---|---|---|---|---|---|---|')
        o += rows

    # Worst frames.
    o += ['', '## Worst frames (in each test\'s window)', '']
    for n, t in tests.items():
        rows = sorted(t.frames.window(*t.win), key=lambda r: -r['dt'])[:3]
        if not rows: continue
        o.append('- **%s**: ' % n + '; '.join('%.0f ms at %.1f s, %s, %d draws' % (r['dt'], r['t'], cause(r), r['draws']) for r in rows))
        slow = [r for r in t.frames.window(*t.win) if r['dt'] > SLOW_MS]
        if slow:
            g = sum(1 for r in slow if r['gpu'] is not None and r['gpu'] > FRAME_MS)
            c = sum(1 for r in slow if r['upd'] + r['cpu'] > FRAME_MS and not (r['gpu'] is not None and r['gpu'] > FRAME_MS))
            o.append('  slow frames: %d GPU over budget, %d CPU over budget, %d neither (compositor / hitch)' % (g, c, len(slow) - g - c))

    # History.
    if past:
        o += ['', '## Against earlier runs', '']
        o.append('| Test | Earlier run | fps | frame p95 ms | slow | GPU ms | RSS MB |')
        o.append('|---|---|---|---|---|---|---|')
        any_row = False
        for n, t in tests.items():
            prev = [p for p in past if n in p['tests'] and p['tests'][n].status == 'done']
            if not prev: continue
            p = prev[-1]
            q = p['tests'][n]
            any_row = True
            o.append('| %s | %s (APK %s) | %s -> %s (%s) | %s -> %s (%s) | %s -> %s | %s -> %s (%s) | %s -> %s |' % (
                n, p['stamp'], p['apk'][:8], fmt(q.s['fps']), fmt(t.s['fps']), delta(t.s['fps'], q.s['fps'], 1, False, 1.0),
                fmt(q.s['p95']), fmt(t.s['p95']), delta(t.s['p95'], q.s['p95'], 1, True, 1.5), fmt(q.s['slow'], 0), fmt(t.s['slow'], 0),
                fmt(q.s['gpu'], 2), fmt(t.s['gpu'], 2), delta(t.s['gpu'], q.s['gpu'], 2, True, 0.5), fmt(q.rss, 0), fmt(t.rss, 0)))
            if q.s['fps'] and t.s['fps'] and t.s['fps'] < q.s['fps'] - 1.0:
                issues.append('**%s**: slower than run %s (%.1f -> %.1f fps).' % (n, p['stamp'], q.s['fps'], t.s['fps']))
        if not any_row: o.append('| (no test in common with the earlier runs) | | | | | | |')

    # Problems.
    shots = {}
    for n, t in tests.items():
        for png in sorted(glob.glob(os.path.join(t.dir, 'autotest_out', '*.png'))):
            st = png_stats(png)
            if st is None: continue
            m, sd = st
            name = os.path.basename(png)
            shots[name] = (round(m, 1), round(sd, 1))
            if m < 12: issues.append('**%s**: screenshot %s is nearly black (mean %.0f).' % (n, name, m))
            elif sd < 6: issues.append('**%s**: screenshot %s is nearly uniform (sd %.1f).' % (n, name, sd))
    o += ['', '## Needs a look' if issues else '## Nothing flagged']
    o += ['- ' + x for x in issues]
    if shots:
        o += ['', '## Screenshots (mean brightness / spread)']
        o += ['- %s: %s / %s' % (k, v[0], v[1]) for k, v in sorted(shots.items())]
    text = '\n'.join(o) + '\n'
    if out_path:
        with open(out_path, 'w', encoding='utf-8') as f: f.write(text)
    sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
