/*
 * unityhost - runs the Android Unity player (libmain/libunity/libil2cpp) on a glibc aarch64 system.
 * M1: load the libs, run constructors, call JNI_OnLoad, emulate NativeLoader.load().
 */
#define _GNU_SOURCE
#include "android_native.h"
#include "fakejni.h"
#include "loader.h"
#include "shim.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

/* Android code reads its stack-protector canary from [tpidr_el0 + 40]. On glibc/aarch64 that offset lies in the
 * padding between the TCB and the first static TLS block. Forcing a 64-byte aligned TLS block guarantees the
 * padding exists and is never written by anyone, so the canary stays constant. */
__thread char tls_canary_pad[64] __attribute__((aligned(64), used));

typedef int (*jni_onload_fn)(void *vm, void *reserved);
typedef int (*native_load_fn)(void *env, void *clazz, void *jstr);

/* ---- sampling watchdog: every UNITY_SAMPLE seconds, interrupt the engine thread and print where it is ---- */
static pthread_t g_engine_thread;
static volatile int g_engine_running;
#define SAMPLE_SIG (SIGRTMIN + 3)

static void describe(unsigned long addr, char *out, size_t n) {
    for (so_lib *l = so_first(); l; l = l->next)
        if (addr >= (unsigned long)l->base && addr < (unsigned long)l->base + l->size) {
            snprintf(out, n, "%s+0x%lx", l->name, addr - (unsigned long)l->base);
            return;
        }
    snprintf(out, n, "0x%lx", addr);
}
static void sample_handler(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    ucontext_t *uc = ucv;
    char a[160], b[160], line[1024], who[160] = "engine";
    {
        pthread_t ts[256]; void *fns[256];
        int nt = shim_threads(ts, fns, 256);
        for (int i = 0; i < nt; i++)
            if (ts[i] && pthread_equal(ts[i], pthread_self())) { describe((unsigned long)fns[i], who, sizeof who); break; }
    }
    unsigned long pc = uc->uc_mcontext.pc, sp = uc->uc_mcontext.sp, fp = uc->uc_mcontext.regs[29];
    describe(pc, a, sizeof a);
    describe(uc->uc_mcontext.regs[30], b, sizeof b);
    if (strstr(b, "libunity.so+0x7ea2b4")) return;   /* idle job-worker wait: hide so the interesting threads stand out */
    char tname[32] = "?";
    pthread_getname_np(pthread_self(), tname, sizeof tname);
    int n = snprintf(line, sizeof line, "[sample] name=%s thr(%s) pc=%s lr=%s bt:", tname, who, a, b);
    for (int i = 0; i < 14 && fp > sp && fp < sp + (8u << 20) && !(fp & 7); i++) {
        unsigned long *f = (unsigned long *)fp;
        describe(f[1], a, sizeof a);
        n += snprintf(line + n, sizeof line - n, " %s", a);
        if (f[0] <= fp) break;
        fp = f[0];
    }
    line[n++] = '\n';
    (void)!write(2, line, n);
}

static void crash_handler(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    char pc_str[160], lr_str[160];
    unsigned long pc = uc ? uc->uc_mcontext.pc : 0;
    unsigned long lr = uc ? uc->uc_mcontext.regs[30] : 0;
    describe(pc, pc_str, sizeof pc_str);
    describe(lr, lr_str, sizeof lr_str);
    char buf[512];
    int n = snprintf(buf, sizeof buf, "\n[CRASH] Signal %d at pc=%s lr=%s addr=%p\n",
                     sig, pc_str, lr_str, si ? si->si_addr : NULL);
    (void)!write(2, buf, n);
    _exit(128 + sig);
}

static void term_handler(int sig) {
    (void)sig;
    _exit(0);
}

static void *watchdog(void *arg) {
    (void)arg;
    int period = getenv("UNITY_SAMPLE") ? atoi(getenv("UNITY_SAMPLE")) : 0;
    if (period <= 0) return NULL;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sample_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SAMPLE_SIG, &sa, NULL);
    for (int t = 0; g_engine_running; t += period) {
        sleep(period);
        so_log("watchdog t=%ds", t + period);
        pthread_kill(g_engine_thread, SAMPLE_SIG);
    }
    return NULL;
}

static void *player_native(const char *name) {
    void *f = fakejni_find_native("com/unity3d/player/UnityPlayer", name);
    if (!f) so_log("UnityPlayer.%s native missing", name);
    return f;
}

/* Equivalent of UnityPlayer$e (the "UnityMain" thread): drives the engine lifecycle and render loop. */
fobj *g_unity_player = NULL;

static void *unity_main(void *arg) {
    (void)arg;
    void *env = fakejni_env();
    fobj *player = jnew("com/unity3d/player/UnityPlayer");
    g_unity_player = player;
    fobj *surface = jnew("android/view/Surface");
    int max_frames = getenv("UNITY_FRAMES") ? atoi(getenv("UNITY_FRAMES")) : 0;

    int (*initJni)(void *, void *, void *) = player_native("initJni");
    void (*webreq)(void *, void *, void *) = player_native("nativeInitWebRequest");
    void (*recreate)(void *, void *, int, void *) = player_native("nativeRecreateGfxState");
    void (*resume)(void *, void *) = player_native("nativeResume");
    void (*focus)(void *, void *, int) = player_native("nativeFocusChanged");
    void (*mute)(void *, void *, int) = player_native("nativeMuteMasterAudio");
    int (*render)(void *, void *) = player_native("nativeRender");

    so_log("UnityMain: initJni");
    if (initJni) initJni(env, player, g_activity);
    so_log("UnityMain: nativeInitWebRequest");
    if (webreq) webreq(env, player, jclass("com/unity3d/player/UnityWebRequest"));
    so_log("UnityMain: nativeRecreateGfxState");
    if (recreate) recreate(env, player, 0, surface);
    so_log("UnityMain: nativeResume");
    if (resume) resume(env, player);
    so_log("UnityMain: nativeFocusChanged(1)");
    if (focus) focus(env, player, 1);
    if (mute) {
        so_log("UnityMain: nativeMuteMasterAudio(0)");
        mute(env, player, 0);
    }

    so_log("UnityMain: render loop");
    for (int f = 0; render; f++) {
        int r = render(env, player);
        if (f < 5 || f % 60 == 0) so_log("nativeRender #%d -> %d", f, r);
        if (!r) { so_log("nativeRender returned false, stopping"); break; }
        if (max_frames && f + 1 >= max_frames) { so_log("frame limit reached"); break; }
    }
    g_engine_running = 0;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <dir-with-android-libs> [-v]\n", argv[0]);
        return 2;
    }
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "-v")) so_verbose++;
    /* stderr goes to log.txt on the SD card. Block-buffer it so so_log() costs one
     * write per 64 KB instead of one per line (the render loop logs every 60 frames,
     * plus egl/assetpack/jni lines during scene loads). so_log_flush() drains it at
     * milestones, and every exit path flushes explicitly. */
    static char logbuf[1 << 16];
    setvbuf(stderr, logbuf, _IOFBF, sizeof logbuf);
    so_set_libdir(argv[1]);
    so_set_args(argc, argv);
    struct sigaction csa;
    memset(&csa, 0, sizeof csa);
    csa.sa_sigaction = crash_handler;
    csa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
    sigaction(SIGSEGV, &csa, NULL);
    sigaction(SIGBUS, &csa, NULL);
    sigaction(SIGABRT, &csa, NULL);
    sigaction(SIGILL, &csa, NULL);
    sigaction(SIGFPE, &csa, NULL);

    struct sigaction tsa;
    memset(&tsa, 0, sizeof tsa);
    tsa.sa_handler = term_handler;
    sigaction(SIGTERM, &tsa, NULL);
    sigaction(SIGINT, &tsa, NULL);
    shim_init();
    android_native_init();
    fakejni_init();
    androidfw_init();

    char path[640];
    snprintf(path, sizeof path, "%s/libmain.so", argv[1]);
    so_lib *lmain = so_load(path, "libmain.so");
    if (!lmain) return 1;
    so_run_init(lmain);

    jni_onload_fn onload = (jni_onload_fn)so_lib_sym(lmain, "JNI_OnLoad");
    if (!onload) { so_log("libmain has no JNI_OnLoad"); return 1; }
    so_log("calling libmain JNI_OnLoad");
    int ver = onload(fakejni_vm(), NULL);
    so_log("libmain JNI_OnLoad -> 0x%x", ver);

    /* Java: NativeLoader.load(nativeLibraryDir + "/libunity.so") */
    native_load_fn nload = (native_load_fn)fakejni_find_native("com/unity3d/player/NativeLoader", "load");
    if (!nload) { so_log("NativeLoader.load was not registered"); return 1; }
    snprintf(path, sizeof path, "%s", argv[1]);   /* libmain appends "/libunity.so" itself */
    so_log("calling NativeLoader.load(%s)", path);
    int ok = nload(fakejni_env(), jclass("com/unity3d/player/NativeLoader"), jstr(path));
    so_log("NativeLoader.load -> %d", ok);

    /* libunity dlopen()s libil2cpp lazily; load it now to stress-test relocations + C++ constructors */
    snprintf(path, sizeof path, "%s/libil2cpp.so", argv[1]);
    so_lib *il2 = so_load(path, "libil2cpp.so");
    if (!il2) return 4;
    so_run_init(il2);
    so_log("il2cpp exports il2cpp_init=%p il2cpp_runtime_invoke=%p", so_lib_sym(il2, "il2cpp_init"),
           so_lib_sym(il2, "il2cpp_runtime_invoke"));

    so_log("M1 done: loaded libs:");
    for (so_lib *l = so_first(); l; l = l->next)
        so_log("  %-24s base=%p", l->name, (void *)l->base);
    if (!ok) return 3;

    /* M3: real graphics through SDL2 (must be initialised on this, the main, thread) */
    const char *gl = getenv("UNITY_GL");
    int use_sdl = gl && !strcmp(gl, "sdl");
    if (use_sdl && !sdl_gl_init()) {
        fprintf(stderr, "UNITY_GL=sdl: could not create an SDL/GLES window (see log above)\n");
        return 5;
    }

    /* M2: run the engine on a dedicated big-stack thread, as Android's UnityMain does */
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8 << 20);
    pthread_t th;
    g_engine_running = 1;
    pthread_create(&th, &at, unity_main, NULL);
    g_engine_thread = th;
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);
    if (use_sdl) {
        while (g_engine_running) {
            if (sdl_poll()) { so_log("window closed, exiting"); so_log_flush(); fflush(NULL); _exit(0); }
            usleep(4000);
        }
    }
    pthread_join(th, NULL);
    g_engine_running = 0;
    so_log("engine thread finished, exiting");
    so_log_flush();
    return 0;
}
