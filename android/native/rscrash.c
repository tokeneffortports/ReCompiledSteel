// librscrash.so - loaded before everything else so that a native crash, even
// one while the game's libraries load, leaves a report the player can send:
// the signal, the fault address and a backtrace as library + offset (symbolize
// with llvm-addr2line against the unstripped libraries). Afterwards the
// previous handler runs (Android's own crash reporting).
//
// Java: RealSteelActivity.nativeInstallCrashHandler(String reportPath).

#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

static char g_path[512];
static struct sigaction g_previous[32];
static const int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};

struct BacktraceState {
  uintptr_t frames[64];
  int count;
};

static _Unwind_Reason_Code CollectFrame(struct _Unwind_Context* context, void* arg) {
  struct BacktraceState* state = (struct BacktraceState*)arg;
  const uintptr_t pc = _Unwind_GetIP(context);
  if (pc != 0 && state->count < 64) state->frames[state->count++] = pc;
  return state->count < 64 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

static void WriteText(int fd, const char* text) {
  size_t left = strlen(text);
  while (left > 0) {
    const ssize_t n = write(fd, text, left);
    if (n <= 0) return;
    text += n;
    left -= (size_t)n;
  }
}

static void Handler(int signal_number, siginfo_t* info, void* ucontext) {
  const int fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    char line[640];
    const uintptr_t pc =
        ucontext != NULL ? (uintptr_t)((ucontext_t*)ucontext)->uc_mcontext.pc : 0;
    snprintf(line, sizeof(line),
             "\n== native crash: signal %d code %d fault address %p pc %p (thread %d) at %ld\n",
             signal_number, info != NULL ? info->si_code : 0,
             info != NULL ? info->si_addr : NULL, (void*)pc, (int)gettid(), (long)time(NULL));
    WriteText(fd, line);
    struct BacktraceState state;
    state.count = 0;
    if (pc != 0) state.frames[state.count++] = pc;
    _Unwind_Backtrace(CollectFrame, &state);
    for (int i = 0; i < state.count; ++i) {
      Dl_info dl;
      memset(&dl, 0, sizeof(dl));
      if (dladdr((void*)state.frames[i], &dl) && dl.dli_fname != NULL) {
        const char* base = strrchr(dl.dli_fname, '/');
        snprintf(line, sizeof(line), "  #%02d %s+0x%lx %s\n", i, base ? base + 1 : dl.dli_fname,
                 (unsigned long)(state.frames[i] - (uintptr_t)dl.dli_fbase),
                 dl.dli_sname != NULL ? dl.dli_sname : "");
      } else {
        snprintf(line, sizeof(line), "  #%02d %p\n", i, (void*)state.frames[i]);
      }
      WriteText(fd, line);
    }
    close(fd);
  }
  // Hand over to whoever was there before (debuggerd), then let it kill us.
  struct sigaction* previous = &g_previous[signal_number & 31];
  sigaction(signal_number, previous, NULL);
  raise(signal_number);
}

JNIEXPORT void JNICALL Java_com_recompiledsteel_app_RealSteelActivity_nativeInstallCrashHandler(
    JNIEnv* env, jclass clazz, jstring path) {
  (void)clazz;
  const char* utf = (*env)->GetStringUTFChars(env, path, NULL);
  if (utf == NULL) return;
  strncpy(g_path, utf, sizeof(g_path) - 1);
  (*env)->ReleaseStringUTFChars(env, path, utf);
  for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); ++i) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = Handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(kSignals[i], &action, &g_previous[kSignals[i] & 31]);
  }
}
