#include "platform/android/crash_handler.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/log.h"

namespace gvr {
namespace {

char gDir[512];
struct sigaction gOld[32];
char gAltStack[64 * 1024];
volatile sig_atomic_t gInHandler = 0;
// Our own library's address range and name, found once at install: the handler then needs no dladdr (it takes the
// linker's lock - a crash inside dlopen would deadlock instead of writing the report; review 2026-10-28).
uintptr_t gLibLo = 0, gLibHi = 0;
char gLibName[128] = "libgiantsvr.so";

int findOwnLib(struct dl_phdr_info* info, size_t, void* data) {
    const uintptr_t probe = reinterpret_cast<uintptr_t>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) continue;
        const uintptr_t lo = info->dlpi_addr + ph.p_vaddr, hi = lo + ph.p_memsz;
        if (probe >= lo && probe < hi) {
            gLibLo = info->dlpi_addr;
            uintptr_t top = 0;
            for (int k = 0; k < info->dlpi_phnum; ++k)
                if (info->dlpi_phdr[k].p_type == PT_LOAD) top = std::max<uintptr_t>(top, info->dlpi_addr + info->dlpi_phdr[k].p_vaddr + info->dlpi_phdr[k].p_memsz);
            gLibHi = top;
            if (info->dlpi_name && info->dlpi_name[0]) {
                const char* slash = std::strrchr(info->dlpi_name, '/');
                std::snprintf(gLibName, sizeof gLibName, "%s", slash ? slash + 1 : info->dlpi_name);
            }
            return 1;
        }
    }
    return 0;
}

constexpr int kSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};

struct Frames {
    uintptr_t pc[48];
    int n = 0;
};

_Unwind_Reason_Code unwindOne(struct _Unwind_Context* ctx, void* arg) {
    Frames* f = static_cast<Frames*>(arg);
    const uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && f->n < 48) f->pc[f->n++] = pc;
    return f->n < 48 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

// Async-signal-safe output: snprintf into a stack buffer, write(2).
void put(int fd, const char* s) {
    if (fd >= 0) (void)!write(fd, s, std::strlen(s));
}

const char* sigName(int s) {
    switch (s) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGILL: return "SIGILL";
        default: return "signal";
    }
}

void handler(int sig, siginfo_t* info, void* uctx) {
    if (!gInHandler) {
        gInHandler = 1;
        char path[640];
        const long now = long(time(nullptr));
        std::snprintf(path, sizeof path, "%s/crash_%ld.txt", gDir, now);
        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        char line[512];
        std::snprintf(line, sizeof line, "GiantsVR crashed: %s (code %d) at address %p, time %ld\n\nbacktrace:\n", sigName(sig),
                      info ? info->si_code : 0, info ? info->si_addr : nullptr, now);
        put(fd, line);
        Frames f;
        _Unwind_Backtrace(unwindOne, &f);
        // (Frames in our library as "lib+offset" - addr2line -e the unstripped build/apk .so gives file:line; other
        // frames as absolute addresses: debuggerd's tombstone names those.)
        for (int i = 0; i < f.n; ++i) {
            if (gLibHi > gLibLo && f.pc[i] >= gLibLo && f.pc[i] < gLibHi)
                std::snprintf(line, sizeof line, "  #%02d pc %08zx  %s\n", i, size_t(f.pc[i] - gLibLo), gLibName);
            else
                std::snprintf(line, sizeof line, "  #%02d pc %p\n", i, reinterpret_cast<void*>(f.pc[i]));
            put(fd, line);
        }
        put(fd, "\nlast log lines:\n");
        static char tail[160 * 242];
        const size_t n = recentLogLines(tail, sizeof tail, 160);
        if (fd >= 0 && n) (void)!write(fd, tail, n);
        if (fd >= 0) close(fd);
        __android_log_print(ANDROID_LOG_ERROR, GVR_LOG_TAG, "Crash report written: %s", path);
    }
    // Then as before: the previous handler (the system's debuggerd tombstone), or the default action.
    const struct sigaction& old = gOld[sig < 32 ? sig : 0];
    if (old.sa_flags & SA_SIGINFO) {
        if (old.sa_sigaction) old.sa_sigaction(sig, info, uctx);
    } else if (old.sa_handler != SIG_IGN && old.sa_handler != SIG_DFL && old.sa_handler) {
        old.sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

}  // namespace

void installCrashHandler(const char* dir) {
    std::snprintf(gDir, sizeof gDir, "%s", dir);
    dl_iterate_phdr(findOwnLib, reinterpret_cast<void*>(&installCrashHandler));
    mkdir(gDir, 0775);
    // (2026-10-29 review) android_main can run again in the same process: installed twice, our own handler would be
    // kept as the 'old' one and a crash would call itself instead of the system's.
    static bool installed = false;
    if (installed) return;
    installed = true;
    stack_t ss{};
    ss.ss_sp = gAltStack;
    ss.ss_size = sizeof gAltStack;
    sigaltstack(&ss, nullptr);
    for (int s : kSignals) {
        struct sigaction sa{};
        sa.sa_sigaction = handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(s, &sa, &gOld[s]);
    }
    LOGI("Crash handler: reports go to %s (%s at %p)", gDir, gLibName, reinterpret_cast<void*>(gLibLo));
}

}  // namespace gvr
