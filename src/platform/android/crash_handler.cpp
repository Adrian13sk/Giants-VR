#include "platform/android/crash_handler.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

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
        for (int i = 0; i < f.n; ++i) {
            Dl_info di{};
            if (dladdr(reinterpret_cast<void*>(f.pc[i]), &di) && di.dli_fname) {
                const char* mod = std::strrchr(di.dli_fname, '/');
                std::snprintf(line, sizeof line, "  #%02d pc %08zx  %s (%s+%zu)\n", i, size_t(f.pc[i] - uintptr_t(di.dli_fbase)),
                              mod ? mod + 1 : di.dli_fname, di.dli_sname ? di.dli_sname : "?",
                              di.dli_saddr ? size_t(f.pc[i] - uintptr_t(di.dli_saddr)) : size_t(0));
            } else {
                std::snprintf(line, sizeof line, "  #%02d pc %p\n", i, reinterpret_cast<void*>(f.pc[i]));
            }
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
    mkdir(gDir, 0775);
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
    LOGI("Crash handler: reports go to %s", gDir);
}

}  // namespace gvr
