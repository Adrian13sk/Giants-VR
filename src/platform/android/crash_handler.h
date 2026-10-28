// A native crash report (2026-10-28): on SIGSEGV / SIGABRT / SIGBUS / SIGFPE / SIGILL the app writes
// <dir>/crash_<time>.txt - the signal, the fault address, a backtrace (module + offset, symbol where
// the dynamic table has one) and the last log lines - then lets the signal kill it as before.
// devtest.ps1 pulls files/crash/ with each test's results.
#pragma once

namespace gvr {
void installCrashHandler(const char* dir);
}
