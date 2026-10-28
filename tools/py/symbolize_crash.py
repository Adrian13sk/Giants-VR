#!/usr/bin/env python3
"""Symbolize a GiantsVR crash report (files/crash/crash_<time>.txt, pulled by devtest.ps1).

Usage: symbolize_crash.py <crash_report.txt> [libgiantsvr.so] [llvm-addr2line]

Frames written as "pc <offset>  libgiantsvr.so" are looked up in the unstripped library of the SAME build
(build/android/libgiantsvr.so by default) with the NDK's llvm-addr2line; function, file and line are printed.
"""
import os
import re
import subprocess
import sys


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    lib = argv[2] if len(argv) > 2 else os.path.join(repo, 'build', 'android', 'libgiantsvr.so')
    tool = argv[3] if len(argv) > 3 else os.path.join(repo, '..', 'toolchains', 'ndk', 'android-ndk-r27c', 'toolchains', 'llvm',
                                                      'prebuilt', 'linux-x86_64', 'bin', 'llvm-addr2line')
    text = open(argv[1], errors='replace').read()
    for line in text.splitlines():
        m = re.match(r'\s*#(\d+) pc ([0-9a-f]+)\s+(\S+\.so)\s*$', line)
        if not m or 'giantsvr' not in m.group(3):
            print(line)
            continue
        # (the return address: the call is one instruction before it)
        off = max(0, int(m.group(2), 16) - 4)
        try:
            out = subprocess.run([tool, '-C', '-f', '-i', '-e', lib, hex(off)], capture_output=True, text=True, timeout=30).stdout.strip()
        except (OSError, subprocess.SubprocessError) as e:
            out = '(addr2line failed: %s)' % e
        print('%s\n        %s' % (line, out.replace('\n', '\n        ')))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
