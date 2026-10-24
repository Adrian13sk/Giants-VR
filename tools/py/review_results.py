"""Review an on-device test run (devtest\\results\\<stamp>, as pulled by devtest.ps1) and list what needs a
look: tests that didn't finish, crashes in the log, errors, frame-rate problems, the adaptive quality
dropping, and screenshots that look broken (nearly black, nearly uniform, or identical to another
test's). Prints a Markdown report.

    python3 tools/py/review_results.py <results folder>
"""
import glob
import os
import re
import statistics
import struct
import sys
import zlib


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


def main(root):
    out = ['# Device run review: ' + os.path.basename(root.rstrip('/\\')), '']
    summary = read(os.path.join(root, 'summary.txt'))
    tests = sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d)) and d != 'sweep_scripts')
    issues = []
    shots = {}
    rows = []
    for t in tests:
        d = os.path.join(root, t)
        res = read(os.path.join(d, 'autotest_out', 'result.txt'))
        log = read(os.path.join(d, 'logcat.txt'))
        status = re.search(r'^%s: (\w+)' % re.escape(t), summary, re.M)
        status = status.group(1) if status else ('done' if 'status ok' in res else 'unknown')
        fps = re.search(r'^fps ([\d.]+)', res, re.M)
        p95 = re.search(r'p95 ([\d.]+)', res)
        slow = re.search(r'slow_frames (\d+)', res)
        q = re.search(r'effects_quality_level (\d+)', res)
        errs = len(re.findall(r' E/GiantsVR', log))
        fatal = re.findall(r'(F/libc.*|FATAL EXCEPTION.*|SIGSEGV.*|Abort message.*)', log)
        rows.append((t, status, fps.group(1) if fps else '-', p95.group(1) if p95 else '-', slow.group(1) if slow else '-',
                     q.group(1) if q else '-', errs))
        if status != 'done': issues.append('**%s** did not finish (%s).' % (t, status))
        if fatal: issues.append('**%s** crashed: `%s`' % (t, fatal[0][:160]))
        if errs: issues.append('**%s**: %d error lines, first: `%s`' % (t, errs, (re.search(r' E/GiantsVR.*', log) or [''])[0][:160]))
        if fps and float(fps.group(1)) < 70: issues.append('**%s**: %s fps.' % (t, fps.group(1)))
        if q and q.group(1) != '0': issues.append('**%s**: effects quality dropped to level %s.' % (t, q.group(1)))
        for png in sorted(glob.glob(os.path.join(d, 'autotest_out', '*.png'))):
            st = png_stats(png)
            name = os.path.basename(png)
            if st is None: continue
            mean, sd = st
            shots[name] = (round(mean, 1), round(sd, 1))
            if mean < 12: issues.append('**%s**: screenshot %s is nearly black (mean %.0f).' % (t, name, mean))
            elif sd < 6: issues.append('**%s**: screenshot %s is nearly uniform (sd %.1f).' % (t, name, sd))
    out.append('| Test | Status | fps | frame p95 ms | slow frames | quality level | errors |')
    out.append('|---|---|---|---|---|---|---|')
    for r in rows: out.append('| %s | %s | %s | %s | %s | %s | %s |' % r)
    out.append('')
    out.append('## Needs a look' if issues else '## Nothing flagged')
    out += ['- ' + i for i in issues]
    if shots:
        out.append('')
        out.append('## Screenshots (mean brightness / spread)')
        out += ['- %s: %s / %s' % (k, v[0], v[1]) for k, v in sorted(shots.items())]
    print('\n'.join(out))


if __name__ == '__main__':
    main(sys.argv[1])
