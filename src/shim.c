/*
 * bionic -> glibc compatibility shims.
 *
 * Only symbols whose ABI differs between Android's bionic and glibc (struct sizes, constants,
 * data symbols, missing functions) are overridden here. Everything else is resolved straight to
 * the host glibc by so_resolve_global().
 *
 * The list of differences was derived from the import tables of libmain/libunity/libil2cpp
 * (see feasibility_analysis.md).
 */
#define _GNU_SOURCE
#include "android_native.h"
#include "loader.h"
#include "shim.h"
#include "opensles.h"

#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

/* ========================================================================== */
/* shim table                                                                  */
/* ========================================================================== */

typedef struct { const char *name; void *addr; } shim_entry;
static shim_entry *g_shims = NULL;
static size_t g_nshims = 0, g_cap_shims = 0;

void shim_register(const char *name, void *addr) {
    if (g_nshims == g_cap_shims) {
        g_cap_shims = g_cap_shims ? g_cap_shims * 2 : 512;
        g_shims = realloc(g_shims, g_cap_shims * sizeof *g_shims);
    }
    g_shims[g_nshims++] = (shim_entry){name, addr};
}

void *shim_lookup(const char *name) {
    for (size_t i = 0; i < g_nshims; i++)
        if (strcmp(g_shims[i].name, name) == 0) return g_shims[i].addr;
    return NULL;
}

/* imports we know about and implement later (graphics/android layers): do not warn loudly */
int shim_is_deferred(const char *n) {
    static const char *const pfx[] = {"egl", "gl", "ANativeWindow_", "ALooper_", "ASensor", "AAsset", "AInput",
                                      "AConfiguration", "AAudio", "AMedia", "AChoreographer", NULL};
    for (int i = 0; pfx[i]; i++)
        if (strncmp(n, pfx[i], strlen(pfx[i])) == 0) return 1;
    return 0;
}

/* ========================================================================== */
/* data symbols                                                                */
/* ========================================================================== */

#define BIONIC_FILE_SZ 152
static char shim_sF[3 * BIONIC_FILE_SZ] __attribute__((aligned(16)));

static FILE *xf(void *f) {
    uintptr_t p = (uintptr_t)f, b = (uintptr_t)shim_sF;
    if (p >= b && p < b + sizeof shim_sF) {
        switch ((p - b) / BIONIC_FILE_SZ) {
        case 0: return stdin;
        case 1: return stdout;
        default: return stderr;
        }
    }
    return (FILE *)f;
}

/* bionic ctype table: index -1..255 */
static unsigned char shim_ctype[257];
static void init_ctype(void) {
    enum { U = 1, L = 2, N = 4, S = 8, P = 16, C = 32, X = 64, B = 128 };
    memset(shim_ctype, 0, sizeof shim_ctype);
    unsigned char *t = shim_ctype + 1;
    for (int c = 0; c < 128; c++) {
        unsigned v = 0;
        if (c < 32 || c == 127) v |= C;
        if (c >= 9 && c <= 13) v |= S;
        if (c == ' ') v |= S | B;
        if (c >= '0' && c <= '9') v |= N;
        if (c >= 'A' && c <= 'Z') v |= U;
        if (c >= 'a' && c <= 'z') v |= L;
        if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) v |= X;
        if (c > 32 && c < 127 && !(v & (U | L | N))) v |= P;
        t[c] = (unsigned char)v;
    }
}

/* ========================================================================== */
/* logging / android properties                                                */
/* ========================================================================== */

static int g_log_level = 0;     /* 0 = print everything */

static int sh_android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    if (prio < g_log_level) return 0;
    static const char pc[] = "?V?DIWEFS";
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    fprintf(stderr, "[%c/%s] %s\n", prio >= 0 && prio < 9 ? pc[prio] : '?', tag ? tag : "", buf);
    return 1;
}
static int sh_android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = sh_android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}
static int sh_android_log_write(int prio, const char *tag, const char *text) {
    return sh_android_log_print(prio, tag, "%s", text);
}

static int sh_system_property_get(const char *name, char *value) {
    const char *v = shim_property(name);
    if (!v) v = "";
    strcpy(value, v);
    return (int)strlen(v);
}
static void sh_noop(void) {}

const char *shim_property(const char *name) {
    static const struct { const char *k, *v; } props[] = {
        {"ro.build.version.sdk", "29"},
        {"ro.build.version.release", "10"},
        {"ro.product.model", "R36S"},
        {"ro.product.manufacturer", "Anbernic"},
        {"ro.product.cpu.abi", "arm64-v8a"},
        {"ro.hardware", "rk3326"},
        {NULL, NULL}};
    for (int i = 0; props[i].k; i++)
        if (strcmp(props[i].k, name) == 0) return props[i].v;
    return NULL;
}

/* ========================================================================== */
/* pthread: bionic object sizes differ from glibc (mutex 40 vs 48, attr 56 vs 64, ...)  */
/* ========================================================================== */

/*
 * bionic mutex/cond objects are only 4-byte aligned, while glibc's use 64-bit atomics (a misaligned
 * ldxr/stxr raises SIGBUS). So the guest object only stores a lazily-created pointer to a real
 * glibc object, accessed with 32-bit atomics only:
 *
 *   word0 (u32) : state. <= 0xffff = bionic static initialiser bits, BUSY while creating, READY when ptr valid
 *   word1/word2 : low/high half of the heap pointer
 */
#define LZ_BUSY 0xFFFFFFFEu
#define LZ_READY 0xFFFFFFFFu
typedef struct { _Atomic uint32_t state; uint32_t lo, hi; } lazy_t;

static void *lazy_get(void *obj, void *(*mk)(uint32_t init_state)) {
    lazy_t *l = obj;
    for (;;) {
        uint32_t s = atomic_load_explicit(&l->state, memory_order_acquire);
        if (s == LZ_READY) return (void *)((uintptr_t)l->lo | ((uintptr_t)l->hi << 32));
        if (s == LZ_BUSY) { sched_yield(); continue; }
        if (atomic_compare_exchange_weak_explicit(&l->state, &s, LZ_BUSY, memory_order_acq_rel, memory_order_relaxed)) {
            void *p = mk(s);
            l->lo = (uint32_t)(uintptr_t)p;
            l->hi = (uint32_t)((uintptr_t)p >> 32);
            atomic_store_explicit(&l->state, LZ_READY, memory_order_release);
            return p;
        }
    }
}
static void *lazy_take(void *obj) {      /* destroy: returns the real object (or NULL) and resets to the zero state */
    lazy_t *l = obj;
    uint32_t s = atomic_load(&l->state);
    void *p = NULL;
    if (s == LZ_READY) p = (void *)((uintptr_t)l->lo | ((uintptr_t)l->hi << 32));
    atomic_store(&l->state, 0);
    return p;
}

/* mutex: bionic static initialisers are 0 (normal), 0x4000 (recursive), 0x8000 (errorcheck) */
static void *mk_mutex(uint32_t s) {
    int type = PTHREAD_MUTEX_NORMAL;
    if (s & 0x4000) type = PTHREAD_MUTEX_RECURSIVE;
    else if (s & 0x8000) type = PTHREAD_MUTEX_ERRORCHECK;
    pthread_mutex_t *nm = malloc(sizeof *nm);
    pthread_mutexattr_t ga;
    pthread_mutexattr_init(&ga);
    pthread_mutexattr_settype(&ga, type);
    pthread_mutex_init(nm, &ga);
    pthread_mutexattr_destroy(&ga);
    return nm;
}
static pthread_mutex_t *mtx_get(void *m) { return lazy_get(m, mk_mutex); }

static int sh_mutex_init(void *m, const int *attr) {
    int type = attr ? (*attr & 3) : 0;
    atomic_store(&((lazy_t *)m)->state, type == 1 ? 0x4000u : type == 2 ? 0x8000u : 0u);
    (void)mtx_get(m);
    return 0;
}
static int sh_mutex_destroy(void *m) {
    pthread_mutex_t *p = lazy_take(m);
    if (p) { pthread_mutex_destroy(p); free(p); }
    return 0;
}
static int sh_mutex_lock(void *m) { return pthread_mutex_lock(mtx_get(m)); }
static int sh_mutex_trylock(void *m) { return pthread_mutex_trylock(mtx_get(m)); }
static int sh_mutex_unlock(void *m) { return pthread_mutex_unlock(mtx_get(m)); }
static int sh_mutexattr_init(int *a) { *a = 0; return 0; }
static int sh_mutexattr_destroy(int *a) { (void)a; return 0; }
static int sh_mutexattr_settype(int *a, int t) { if (t < 0 || t > 2) return EINVAL; *a = (*a & ~3) | t; return 0; }

/* condvar: attr bit 1 = CLOCK_MONOTONIC; the guest object's state word carries the same bit before creation */
static int sh_condattr_init(int *a) { *a = 0; return 0; }
static int sh_condattr_destroy(int *a) { (void)a; return 0; }
static int sh_condattr_setclock(int *a, int clk) {
    if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC) return EINVAL;
    *a = (*a & ~2) | (clk == CLOCK_MONOTONIC ? 2 : 0);
    return 0;
}
static void *mk_cond(uint32_t s) {
    pthread_cond_t *c = malloc(sizeof *c);
    pthread_condattr_t ga;
    pthread_condattr_init(&ga);
    if (s & 2) pthread_condattr_setclock(&ga, CLOCK_MONOTONIC);
    pthread_cond_init(c, &ga);
    pthread_condattr_destroy(&ga);
    return c;
}
static pthread_cond_t *cnd_get(void *c) { return lazy_get(c, mk_cond); }
static int sh_cond_init(void *c, const int *attr) {
    atomic_store(&((lazy_t *)c)->state, (attr && (*attr & 2)) ? 2u : 0u);
    (void)cnd_get(c);
    return 0;
}
static int sh_cond_destroy(void *c) {
    pthread_cond_t *p = lazy_take(c);
    if (p) { pthread_cond_destroy(p); free(p); }
    return 0;
}
static int sh_cond_signal(void *c) { return pthread_cond_signal(cnd_get(c)); }
static int sh_cond_broadcast(void *c) { return pthread_cond_broadcast(cnd_get(c)); }
static int sh_cond_wait(void *c, void *m) { return pthread_cond_wait(cnd_get(c), mtx_get(m)); }
static int sh_cond_timedwait(void *c, void *m, const struct timespec *ts) {
    return pthread_cond_timedwait(cnd_get(c), mtx_get(m), ts);
}
static int sh_rwlock_init(pthread_rwlock_t *l, const void *attr) { (void)attr; return pthread_rwlock_init(l, NULL); }

/* attr */
typedef struct {
    uint32_t flags;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
    char reserved[16];
} battr_t;
_Static_assert(sizeof(battr_t) == 56, "bionic pthread_attr_t is 56 bytes");

static int sh_attr_init(battr_t *a) {
    memset(a, 0, sizeof *a);
    a->stack_size = 1024 * 1024;
    a->guard_size = 4096;
    return 0;
}
static int sh_attr_destroy(battr_t *a) { (void)a; return 0; }
static int sh_attr_setdetachstate(battr_t *a, int s) { if (s) a->flags |= 1; else a->flags &= ~1u; return 0; }
static int sh_attr_setstacksize(battr_t *a, size_t s) { a->stack_size = s; return 0; }
static int sh_attr_getstack(const battr_t *a, void **base, size_t *size) {
    *base = a->stack_base;
    *size = a->stack_size;
    return 0;
}
static int sh_getattr_np(pthread_t t, battr_t *a) {
    pthread_attr_t ga;
    int r = pthread_getattr_np(t, &ga);
    if (r) return r;
    sh_attr_init(a);
    void *addr = NULL;
    size_t sz = 0;
    pthread_attr_getstack(&ga, &addr, &sz);
    a->stack_base = addr;
    a->stack_size = sz;
    int ds = 0;
    pthread_attr_getdetachstate(&ga, &ds);
    if (ds == PTHREAD_CREATE_DETACHED) a->flags |= 1;
    pthread_attr_destroy(&ga);
    return 0;
}
#define MAX_TRACKED 256
static pthread_t g_thr[MAX_TRACKED];
static void *g_thr_fn[MAX_TRACKED];
static volatile int g_nthr;
int shim_threads(pthread_t *out, void **fn, int max) {
    int n = g_nthr < max ? g_nthr : max;
    for (int i = 0; i < n; i++) { out[i] = g_thr[i]; fn[i] = g_thr_fn[i]; }
    return n;
}
static int sh_pthread_create(pthread_t *th, const battr_t *a, void *(*fn)(void *), void *arg);
typedef struct { void *(*fn)(void *); void *arg; int id; } tramp_t;
static void *thread_tramp(void *p) {
    tramp_t t = *(tramp_t *)p;
    free(p);
    char nm[32] = "?";
    pthread_getname_np(pthread_self(), nm, sizeof nm);
    so_log("thread #%d started", t.id);
    void *r = t.fn(t.arg);
    pthread_getname_np(pthread_self(), nm, sizeof nm);
    so_log("thread #%d (%s) EXITED", t.id, nm);
    return r;
}
static int sh_pthread_create(pthread_t *th, const battr_t *a, void *(*fn)(void *), void *arg) {
    pthread_attr_t ga;
    pthread_attr_init(&ga);
    if (a) {
        if (a->flags & 1) pthread_attr_setdetachstate(&ga, PTHREAD_CREATE_DETACHED);
        if (a->stack_size >= 16384) pthread_attr_setstacksize(&ga, a->stack_size);
    }
    static int next_id;
    int id = __sync_fetch_and_add(&next_id, 1);
    int logging = getenv("UNITY_THREADLOG") != NULL;
    void *(*start)(void *) = fn;
    void *sarg = arg;
    if (logging) {
        tramp_t *t = malloc(sizeof *t);
        t->fn = fn; t->arg = arg; t->id = id;
        start = thread_tramp; sarg = t;
    }
    int r = pthread_create(th, &ga, start, sarg);
    pthread_attr_destroy(&ga);
    if (logging) {
        const char *ln = "?"; unsigned long off = (unsigned long)fn;
        for (so_lib *l = so_first(); l; l = l->next)
            if ((uint8_t *)fn >= l->base && (uint8_t *)fn < l->base + l->size) { ln = l->name; off = (uint8_t *)fn - l->base; break; }
        so_log("pthread_create #%d fn=%s+0x%lx stack=%zu detached=%d -> %d", id, ln, off, a ? (size_t)a->stack_size : 0, a ? (a->flags & 1) : 0, r);
    }
    if (r == 0) {
        int i = __sync_fetch_and_add(&g_nthr, 1);
        if (i < MAX_TRACKED) { g_thr[i] = *th; g_thr_fn[i] = (void *)fn; }
        else g_nthr = MAX_TRACKED;
    }
    return r;
}

/* semaphores: bionic sem_t is 16 bytes, glibc 32 -> keep a heap sem in the first 8 bytes */
static sem_t *sem_of(void *s) { return *(sem_t **)s; }
static int sh_sem_init(void *s, int pshared, unsigned v) {
    sem_t *n = malloc(sizeof *n);
    int r = sem_init(n, pshared, v);
    *(sem_t **)s = n;
    return r;
}
static int sh_sem_destroy(void *s) {
    sem_t *n = sem_of(s);
    if (n) { sem_destroy(n); free(n); *(sem_t **)s = NULL; }
    return 0;
}
static int sh_sem_post(void *s) { return sem_post(sem_of(s)); }
static int sh_sem_wait(void *s) { return sem_wait(sem_of(s)); }
static int sh_sem_getvalue(void *s, int *v) { return sem_getvalue(sem_of(s), v); }

/* ========================================================================== */
/* signals: bionic LP64 sigset_t is a single 64-bit word                         */
/* ========================================================================== */

typedef uint64_t bsigset_t;
struct bsigaction {
    int sa_flags;
    void *b_handler;            /* sa_handler / sa_sigaction share this slot */
    bsigset_t sa_mask;
    void (*sa_restorer)(void);
};

static int sh_sigemptyset(bsigset_t *s) { *s = 0; return 0; }
static int sh_sigfillset(bsigset_t *s) { *s = ~0ULL; return 0; }
static int sh_sigaddset(bsigset_t *s, int n) { if (n < 1 || n > 64) { errno = EINVAL; return -1; } *s |= 1ULL << (n - 1); return 0; }
static int sh_sigdelset(bsigset_t *s, int n) { if (n < 1 || n > 64) { errno = EINVAL; return -1; } *s &= ~(1ULL << (n - 1)); return 0; }
static int sh_sigismember(const bsigset_t *s, int n) { if (n < 1 || n > 64) { errno = EINVAL; return -1; } return (*s >> (n - 1)) & 1; }

static void b2g_set(const bsigset_t *b, sigset_t *g) {
    sigemptyset(g);
    for (int i = 1; i <= 64; i++)
        if ((*b >> (i - 1)) & 1) sigaddset(g, i);
}
static void g2b_set(const sigset_t *g, bsigset_t *b) {
    *b = 0;
    for (int i = 1; i <= 64; i++)
        if (sigismember(g, i) == 1) *b |= 1ULL << (i - 1);
}

static int sh_sigaction(int sig, const struct bsigaction *nb, struct bsigaction *ob) {
    struct sigaction n, o;
    if (nb) {
        memset(&n, 0, sizeof n);
        n.sa_flags = nb->sa_flags;
        n.sa_sigaction = (void (*)(int, siginfo_t *, void *))nb->b_handler;
        b2g_set(&nb->sa_mask, &n.sa_mask);
    }
    int r = sigaction(sig, nb ? &n : NULL, ob ? &o : NULL);
    if (r == 0 && ob) {
        memset(ob, 0, sizeof *ob);
        ob->sa_flags = o.sa_flags;
        ob->b_handler = (void *)o.sa_sigaction;
        g2b_set(&o.sa_mask, &ob->sa_mask);
    }
    return r;
}
static int sh_pthread_sigmask(int how, const bsigset_t *nb, bsigset_t *ob) {
    sigset_t n, o;
    if (nb) b2g_set(nb, &n);
    int r = pthread_sigmask(how, nb ? &n : NULL, ob ? &o : NULL);
    if (r == 0 && ob) g2b_set(&o, ob);
    return r;
}
static int sh_sigsuspend(const bsigset_t *b) {
    sigset_t g;
    b2g_set(b, &g);
    return sigsuspend(&g);
}

/* ========================================================================== */
/* stdio: map bionic's &__sF[n] to glibc stdin/stdout/stderr                    */
/* ========================================================================== */

static int sh_fprintf(void *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(xf(f), fmt, ap);
    va_end(ap);
    return r;
}
static int sh_fscanf(void *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfscanf(xf(f), fmt, ap);
    va_end(ap);
    return r;
}
static int sh_fputc(int c, void *f) { return fputc(c, xf(f)); }
static int sh_fputs(const char *s, void *f) { return fputs(s, xf(f)); }
static size_t sh_fwrite(const void *p, size_t s, size_t n, void *f) { return fwrite(p, s, n, xf(f)); }
static size_t sh_fread(void *p, size_t s, size_t n, void *f) { return fread(p, s, n, xf(f)); }
static char *sh_fgets(char *b, int n, void *f) { return fgets(b, n, xf(f)); }
static int sh_fflush(void *f) { return fflush(f ? xf(f) : NULL); }
static int sh_fclose(void *f) { return fclose(xf(f)); }
static int sh_feof(void *f) { return feof(xf(f)); }
static int sh_ferror(void *f) { return ferror(xf(f)); }
static void sh_clearerr(void *f) { clearerr(xf(f)); }
static int sh_fileno(void *f) { return fileno(xf(f)); }
static int sh_fseek(void *f, long o, int w) { return fseek(xf(f), o, w); }
static long sh_ftell(void *f) { return ftell(xf(f)); }
static int sh_getc(void *f) { return getc(xf(f)); }

/* ========================================================================== */
/* functions missing / inline in old glibc, or with different constants            */
/* ========================================================================== */

/* glibc < 2.33 does not export stat/fstat/lstat; bionic aarch64 struct stat == kernel struct stat */
static int sh_stat(const char *p, void *b) { return (int)syscall(SYS_newfstatat, AT_FDCWD, p, b, 0); }
static int sh_lstat(const char *p, void *b) { return (int)syscall(SYS_newfstatat, AT_FDCWD, p, b, AT_SYMLINK_NOFOLLOW); }
static int sh_fstat(int fd, void *b) { return (int)syscall(SYS_fstat, fd, b); }

static size_t sh_strlcpy(char *dst, const char *src, size_t n) {
    size_t l = strlen(src);
    if (n) {
        size_t c = l >= n ? n - 1 : l;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return l;
}
static int sh_gettid(void) { return (int)syscall(SYS_gettid); }

static long sh_sysconf(int name) {
    switch (name) {
    case 0x27: case 0x28: return sysconf(_SC_PAGESIZE);
    case 0x60: return sysconf(_SC_NPROCESSORS_CONF);
    case 0x61: return sysconf(_SC_NPROCESSORS_ONLN);
    case 0x62: return sysconf(_SC_PHYS_PAGES);
    case 0x63: return sysconf(_SC_AVPHYS_PAGES);
    case 2: return sysconf(_SC_CLK_TCK);
    case 4: return sysconf(_SC_OPEN_MAX);
    default:
        so_log("sysconf(0x%x) unsupported -> -1", name);
        errno = EINVAL;
        return -1;
    }
}

/* networking is intentionally disabled (Firebase/UnityWebRequest simply fail) */
static int sh_getaddrinfo(const char *a, const char *b, const void *c, void **d) { (void)a; (void)b; (void)c; *d = NULL; return 8; /* bionic EAI_NONAME */ }
static void sh_freeaddrinfo(void *p) { (void)p; }
static int sh_getnameinfo(const void *a, unsigned al, char *h, unsigned hl, char *s, unsigned sl, int f) {
    (void)a; (void)al; (void)h; (void)hl; (void)s; (void)sl; (void)f; return 8;
}
static const char *sh_gai_strerror(int e) { (void)e; return "network disabled"; }
static void *sh_null1(void) { return NULL; }
static int sh_neg1(void) { errno = ENOTTY; return -1; }
static int sh_zero(void) { return 0; }
static int sh_fsync(int fd) { (void)fd; return 0; }
static int sh_fdatasync(int fd) { (void)fd; return 0; }

static int sh_cxa_atexit(void (*f)(void *), void *a, void *d) { (void)f; (void)a; (void)d; return 0; }
static void sh_cxa_finalize(void *d) { (void)d; }

/* ========================================================================== */
/* dynamic linker                                                                */
/* ========================================================================== */

static char g_dlerr[256];
static int g_dlerr_set = 0;

static int dl_unavailable(const char *b) {
    static const char *const no[] = {"libvulkan.so", "libmediandk.so", "libSEC_OMX_Core.so",
                                     "libsomxcore.so", NULL};
    for (int i = 0; no[i]; i++)
        if (strcmp(no[i], b) == 0) return 1;
    return 0;
}
static int dl_system(const char *b) {
    static const char *const sy[] = {"libc.so", "libc", "liblibc.so", "libm.so", "libm", "libdl.so", "libdl",
                                     "liblog.so", "liblog", "libstdc++.so", "libz.so", "libz",
                                     "libandroid.so", "libEGL.so", "libGLESv2.so", "libGLESv3.so",
                                     "libOpenSLES.so", "libOpenSLES",
                                     "libFirebaseCppApp-8_7_0.so", "FirebaseCppApp-8_7_0",
                                     "FirebaseCppApp-8_7_0.so", "libFirebaseCppApp.so",
                                     "libFirebaseCppAnalytics.so", "FirebaseCppAnalytics", "FirebaseCppAnalytics.so",
                                     "libFirebaseCppCrashlytics.so", "FirebaseCppCrashlytics", "FirebaseCppCrashlytics.so",
                                     "libcrashlytics.so", "libcrashlytics-common.so",
                                     "libcrashlytics-handler.so", "libcrashlytics-trampoline.so",
                                     NULL};
    for (int i = 0; sy[i]; i++)
        if (strcmp(sy[i], b) == 0) return 1;
    return 0;
}

static void *firebase_stub(void) { return NULL; }

static void *sh_dlopen(const char *name, int flags) {
    (void)flags;
    if (!name) return so_builtin();
    const char *b = strrchr(name, '/');
    b = b ? b + 1 : name;
    if (dl_unavailable(b)) {
        snprintf(g_dlerr, sizeof g_dlerr, "dlopen failed: library \"%s\" not found", name);
        g_dlerr_set = 1;
        so_log("dlopen(%s) -> NULL (unavailable)", name);
        return NULL;
    }
    if (dl_system(b)) {
        so_log("dlopen(%s) -> builtin", name);
        return so_builtin();
    }
    so_lib *l = so_find(b);
    if (!l) {
        char path[640];
        snprintf(path, sizeof path, "%s/%s", so_get_libdir(), b);
        so_log("dlopen(%s) -> loading %s", name, path);
        l = so_load(path, b);
        if (l) so_run_init(l);
    }
    if (!l) {
        snprintf(g_dlerr, sizeof g_dlerr, "dlopen failed: library \"%s\" not found", name);
        g_dlerr_set = 1;
    }
    return l;
}
static void *sh_dlsym(void *h, const char *name) {
    void *p;
    if (h == NULL || h == (void *)-1 || h == so_builtin()) p = so_resolve_global(name, NULL);
    else p = so_lib_sym((so_lib *)h, name);
    if (!p && !strncmp(name, "gl", 2)) p = android_gl_proc(name);   /* libGLESv2 core entry points */
    if (!p && (strstr(name, "Firebase") || strstr(name, "SWIG") || strstr(name, "AppUtil") ||
               strstr(name, "Crashlytics") || strstr(name, "Analytics") ||
               !strcmp(name, "SetLogFunction"))) {
        so_log("[firebase] stubbing %s", name);
        p = (void *)firebase_stub;
    }
    if (!p) {
        snprintf(g_dlerr, sizeof g_dlerr, "undefined symbol: %s", name);
        g_dlerr_set = 1;
    }
    return p;
}
static int sh_dlclose(void *h) { (void)h; return 0; }
static char *sh_dlerror(void) {
    if (!g_dlerr_set) return NULL;
    g_dlerr_set = 0;
    return g_dlerr;
}
static int sh_dladdr(const void *addr, void *info) { (void)addr; memset(info, 0, 32); return 0; }

static int sh_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data) {
    for (so_lib *l = so_first(); l; l = l->next) {
        struct dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = (ElfW(Addr))l->base;
        info.dlpi_name = l->path;
        info.dlpi_phdr = l->phdr;
        info.dlpi_phnum = (ElfW(Half))l->phnum;
        int r = cb(&info, offsetof(struct dl_phdr_info, dlpi_phnum) + sizeof(ElfW(Half)), data);
        if (r) return r;
    }
    return dl_iterate_phdr(cb, data);   /* host modules (main exe, libc, ...) */
}

/* ========================================================================== */
/* registration                                                                */
/* ========================================================================== */

extern int shim_setjmp(void *), shim_longjmp(void *, int);

#define R(n, f) shim_register(n, (void *)(f))

void shim_init(void) {
    static int done;
    if (done) return;
    done = 1;
    init_ctype();
    /* the linker may drop libm & co. as 'unneeded'; make their symbols reachable through RTLD_DEFAULT */
    static const char *const hostlibs[] = {"libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1", NULL};
    for (int i = 0; hostlibs[i]; i++) dlopen(hostlibs[i], RTLD_NOW | RTLD_GLOBAL);
    const char *lv = getenv("SHIM_LOGLEVEL");
    if (lv) g_log_level = atoi(lv);

    /* data */
    R("__sF", shim_sF);
    R("_ctype_", shim_ctype);

    /* android platform */
    R("__android_log_print", sh_android_log_print);
    R("__android_log_vprint", sh_android_log_vprint);
    R("__android_log_write", sh_android_log_write);
    R("__system_property_get", sh_system_property_get);
    R("__google_potentially_blocking_region_begin", sh_noop);
    R("__google_potentially_blocking_region_end", sh_noop);
    R("__errno", __errno_location);

    /* pthread */
    R("pthread_mutex_init", sh_mutex_init);
    R("pthread_mutex_destroy", sh_mutex_destroy);
    R("pthread_mutex_lock", sh_mutex_lock);
    R("pthread_mutex_trylock", sh_mutex_trylock);
    R("pthread_mutex_unlock", sh_mutex_unlock);
    R("pthread_mutexattr_init", sh_mutexattr_init);
    R("pthread_mutexattr_destroy", sh_mutexattr_destroy);
    R("pthread_mutexattr_settype", sh_mutexattr_settype);
    R("pthread_condattr_init", sh_condattr_init);
    R("pthread_condattr_destroy", sh_condattr_destroy);
    R("pthread_condattr_setclock", sh_condattr_setclock);
    R("pthread_cond_init", sh_cond_init);
    R("pthread_atfork", sh_zero);          /* only in libc_nonshared.a on glibc; we never fork */
    R("pthread_cond_destroy", sh_cond_destroy);
    R("pthread_cond_signal", sh_cond_signal);
    R("pthread_cond_broadcast", sh_cond_broadcast);
    R("pthread_cond_wait", sh_cond_wait);
    R("pthread_cond_timedwait", sh_cond_timedwait);
    R("pthread_rwlock_init", sh_rwlock_init);
    R("pthread_attr_init", sh_attr_init);
    R("pthread_attr_destroy", sh_attr_destroy);
    R("pthread_attr_setdetachstate", sh_attr_setdetachstate);
    R("pthread_attr_setstacksize", sh_attr_setstacksize);
    R("pthread_attr_getstack", sh_attr_getstack);
    R("pthread_getattr_np", sh_getattr_np);
    R("pthread_create", sh_pthread_create);
    R("pthread_sigmask", sh_pthread_sigmask);
    R("sem_init", sh_sem_init);
    R("sem_destroy", sh_sem_destroy);
    R("sem_post", sh_sem_post);
    R("sem_wait", sh_sem_wait);
    R("sem_getvalue", sh_sem_getvalue);

    /* setjmp family (bionic jmp_buf is 256 bytes, glibc's is 312) */
    R("setjmp", shim_setjmp);
    R("longjmp", shim_longjmp);

    /* signals */
    R("sigaction", sh_sigaction);
    R("sigemptyset", sh_sigemptyset);
    R("sigfillset", sh_sigfillset);
    R("sigaddset", sh_sigaddset);
    R("sigdelset", sh_sigdelset);
    R("sigismember", sh_sigismember);
    R("sigsuspend", sh_sigsuspend);

    /* stdio */
    R("fprintf", sh_fprintf);
    R("fscanf", sh_fscanf);
    R("fputc", sh_fputc);
    R("fputs", sh_fputs);
    R("fwrite", sh_fwrite);
    R("fread", sh_fread);
    R("fgets", sh_fgets);
    R("fflush", sh_fflush);
    R("fclose", sh_fclose);
    R("feof", sh_feof);
    R("ferror", sh_ferror);
    R("clearerr", sh_clearerr);
    R("fileno", sh_fileno);
    R("fseek", sh_fseek);
    R("ftell", sh_ftell);
    R("getc", sh_getc);

    /* libc gaps */
    R("stat", sh_stat);
    R("lstat", sh_lstat);
    R("fstat", sh_fstat);
    R("strlcpy", sh_strlcpy);
    R("gettid", sh_gettid);
    R("sysconf", sh_sysconf);
    R("getaddrinfo", sh_getaddrinfo);
    R("freeaddrinfo", sh_freeaddrinfo);
    R("getnameinfo", sh_getnameinfo);
    R("gai_strerror", sh_gai_strerror);
    R("gethostbyname", sh_null1);
    R("gethostbyaddr", sh_null1);
    R("getpwuid", sh_null1);
    R("isatty", sh_zero);
    R("tcgetattr", sh_neg1);
    R("tcsetattr", sh_neg1);
    R("tcflush", sh_neg1);
    R("__cxa_atexit", sh_cxa_atexit);
    R("__cxa_finalize", sh_cxa_finalize);
    R("fsync", sh_fsync);
    R("fdatasync", sh_fdatasync);
    R("sync", sh_zero);
    R("syncfs", sh_zero);

    /* dynamic linker */
    R("dlopen", sh_dlopen);
    R("dlsym", sh_dlsym);
    R("dlclose", sh_dlclose);
    R("dlerror", sh_dlerror);
    R("dladdr", sh_dladdr);
    R("dl_iterate_phdr", sh_dl_iterate_phdr);

    /* libz is linked statically into the host */
    R("inflate", inflate);
    R("inflateEnd", inflateEnd);
    R("inflateInit2_", inflateInit2_);

    /* OpenSL ES audio backend */
    opensles_init();
}
