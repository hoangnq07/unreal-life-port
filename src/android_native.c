/*
 * libandroid / libEGL / libGLESv2 replacements.
 *
 * UNITY_GL=null (default under qemu): a "null" GLES driver. Every GL entry point is a no-op, but the
 * queries Unity makes during start-up (versions, limits, shader/program/framebuffer status, object names)
 * return plausible values. That lets the whole engine + il2cpp + game scripts run headless so everything
 * except actual rasterisation can be tested on a PC.
 *
 * UNITY_GL=sdl (real device): EGL/GL calls are forwarded to SDL2 (added in milestone 3).
 */
#define _GNU_SOURCE
#include "android_native.h"
#include "fakejni.h"
#include "loader.h"
#include "shim.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

int g_screen_w = 640, g_screen_h = 480;

/* ========================================================================== */
/* Frame / memory profiler (UNITY_PERF=1, on by default)                        */
/*                                                                             */
/* Tells us *where* the frame budget goes: is the GPU/VSYNC stalling (large     */
/* 16/33 ms buckets), is the CPU saturated (cpu% > 100 means >1 core busy), or  */
/* is the process swapping (VmSwap growing, RSS at the limit)?                  */
/* ========================================================================== */

static int      perf_on = -1;
static uint64_t perf_last_us, perf_sum_us, perf_min_us = ~0ULL, perf_max_us;
static unsigned perf_buckets[8];
static unsigned perf_n;
static struct rusage perf_last_ru;
static int perf_first = 1;
/* Stall forensics: a long frame is either (a) waiting on disk/swap I/O - read_bytes jumps and the
 * CPU is idle - or (b) burning CPU (compression, shader compile, GC). /proc/self/io separates them. */
static unsigned perf_stall_n;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

static long proc_kb(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    long v = -1;
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) == 0) { v = strtol(line + klen, NULL, 10); break; }
    }
    fclose(f);
    return v;
}

/* read_bytes from /proc/self/io: bytes this process actually fetched from *storage* (block layer).
 * rchar counts bytes returned by read()/pread() regardless of cache, so it also catches reads that
 * were served from page cache. Comparing the two tells "real disk" from "just cache". */
static unsigned long long proc_io_field(const char *field) {
    FILE *f = fopen("/proc/self/io", "r");
    if (!f) return 0;
    char line[256];
    unsigned long long v = 0;
    size_t fl = strlen(field);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, field, fl)) { v = strtoull(line + fl, NULL, 10); break; }
    fclose(f);
    return v;
}

/* Per-thread CPU + state sampler. During a long frame we want to know WHICH thread burned the time
 * and whether it was running (R) or blocked in the kernel on I/O (D). A thread in R with big utime
 * is compute (GC marking, asset decode); a thread in D is waiting on storage/swap. Reads
 * /proc/self/task/<tid>/stat for each task. */
typedef struct { int tid; unsigned long long cpu; char comm[17]; char state; } thr_stat;
static thr_stat g_thr_prev[64];
static int g_nthr_prev;

static int read_thr(int tid, char *comm, size_t cs, char *state, unsigned long long *cpu) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return 0; }
    fclose(f);
    /* format: pid (comm) state ppid ... utime(14) stime(15) ... (1-based fields) */
    char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
    if (!lp || !rp || rp < lp) return 0;
    size_t n = (size_t)(rp - lp - 1);
    if (n >= cs) n = cs - 1;
    memcpy(comm, lp + 1, n); comm[n] = 0;
    char *p = rp + 2;                       /* skip ") " */
    *state = *p;
    /* advance to field 14 (utime) and 15 (stime): fields after comm are state=3, ppid=4, ... */
    for (int i = 0; i < 11 && *p; i++) { while (*p && *p != ' ') p++; while (*p == ' ') p++; }
    unsigned long long ut = strtoull(p, &p, 10);
    unsigned long long st = strtoull(p, NULL, 10);
    *cpu = ut + st;
    return 1;
}

static void perf_thread_report(unsigned long long *total_cpu) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    struct dirent *e;
    thr_stat now[64]; int nn = 0;
    unsigned long long tot = 0;
    while ((e = readdir(d)) && nn < 64) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int tid = atoi(e->d_name);
        char comm[17], state; unsigned long long cpu;
        if (!read_thr(tid, comm, sizeof comm, &state, &cpu)) continue;
        now[nn].tid = tid; now[nn].cpu = cpu; now[nn].state = state;
        snprintf(now[nn].comm, sizeof now[nn].comm, "%s", comm);
        nn++;
        tot += cpu;
    }
    closedir(d);
    *total_cpu = tot;
    /* Find the top-2 by CPU *delta* since the previous stall, so we see who is eating THIS frame. */
    for (int k = 0; k < 2; k++) {
        int best = -1; unsigned long long bd = 0;
        for (int i = 0; i < nn; i++) {
            unsigned long long prev = 0;
            for (int j = 0; j < g_nthr_prev; j++)
                if (g_thr_prev[j].tid == now[i].tid) { prev = g_thr_prev[j].cpu; break; }
            unsigned long long dlt = now[i].cpu - prev;
            if (dlt > bd) { bd = dlt; best = i; }
        }
        if (best < 0 || bd < 50000) break;    /* <50ms of CPU: not interesting */
        so_log("[perf]   hot thread: %s tid=%d state=%c cpu=+%llums",
               now[best].comm, now[best].tid, now[best].state, bd / 1000);
        now[best].cpu = 0;                     /* don't pick it again */
    }
    memcpy(g_thr_prev, now, (size_t)nn * sizeof now[0]);
    g_nthr_prev = nn;
}

/* Per-long-frame forensics. For every frame slower than 100 ms, log where the time went:
 *   utime  = userspace CPU (asset decompress, GC, shader compile, engine logic)
 *   stime  = kernel CPU (zram compress/decompress, page reclaim, syscalls)
 *   rchar  = bytes read via read() (even if served from page cache)
 *   read_bytes = bytes actually fetched from the block device (microSD / swap)
 * A stall that is pure stime => swap/zram pressure. Pure utime => the game decompressing or GCing.
 * Big read_bytes => real disk. Gated by UNITY_STALL_LOG. */
static void perf_stall_probe(uint64_t dt) {
    static int on = -1;
    static uint64_t last_ut, last_st;
    static unsigned long long last_rc, last_rb, last_wc, last_wb;
    static long last_maj;
    if (on < 0) { const char *e = getenv("UNITY_STALL_LOG"); on = !(e && (!strcmp(e, "0") || !strcmp(e, "off"))); }
    if (!on) return;
    /* Sample every frame (a few /proc reads, microseconds) so the deltas are per-frame. */
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    uint64_t ut = (uint64_t)ru.ru_utime.tv_sec * 1000000ULL + ru.ru_utime.tv_usec;
    uint64_t st = (uint64_t)ru.ru_stime.tv_sec * 1000000ULL + ru.ru_stime.tv_usec;
    long maj = ru.ru_majflt;
    unsigned long long rc = proc_io_field("rchar:");
    unsigned long long rb = proc_io_field("read_bytes:");
    unsigned long long wc = proc_io_field("wchar:");
    unsigned long long wb = proc_io_field("write_bytes:");
    uint64_t ut_dt = ut - last_ut, st_dt = st - last_st;
    unsigned long long rc_dt = rc - last_rc, rb_dt = rb - last_rb;
    unsigned long long wc_dt = wc - last_wc, wb_dt = wb - last_wb;
    long maj_dt = maj - last_maj;
    last_ut = ut; last_st = st; last_rc = rc; last_rb = rb; last_wc = wc; last_wb = wb; last_maj = maj;
    if (dt < 100000) return;
    long swp = proc_kb("/proc/self/status", "VmSwap:");
    perf_stall_n++;
    /* First 60 stalls, then every 25th, to keep the log small. */
    if (perf_stall_n <= 60 || perf_stall_n % 25 == 0) {
        so_log("[perf] STALL #%u frame=%.0fms utime=%.0f stime=%.0f majflt=%ld "
               "rchar=+%lluKB read_bytes=+%lluKB wchar=+%lluKB write_bytes=+%lluKB vmswap=%ldMB",
               perf_stall_n, dt / 1000.0, ut_dt / 1000.0, st_dt / 1000.0, maj_dt,
               rc_dt / 1024, rb_dt / 1024, wc_dt / 1024, wb_dt / 1024, swp / 1024);
        unsigned long long tot;
        perf_thread_report(&tot);
    }
}

/* ---- per-mapping memory breakdown: where does the ~800 MB actually live? ---- */
#define MEM_MAX 128
typedef struct { char name[72]; unsigned long rss, swap, pss; } memrec;
static memrec g_mem[MEM_MAX];
static int g_memn;

static const char *smaps_name(const char *line) {
    const char *p = line;
    for (int i = 0; i < 5; i++) {                 /* addr perms offset dev inode */
        while (*p && *p != ' ' && *p != '\t') p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    return (*p && *p != '\n') ? p : "[anon]";
}

static void mem_add(const char *name, unsigned long rss, unsigned long swap, unsigned long pss) {
    if (!rss && !swap) return;
    const char *b = strrchr(name, '/');           /* keep it readable: basename only */
    if (b && b[1]) name = b + 1;
    for (int i = 0; i < g_memn; i++)
        if (!strcmp(g_mem[i].name, name)) {
            g_mem[i].rss += rss; g_mem[i].swap += swap; g_mem[i].pss += pss; return;
        }
    if (g_memn < MEM_MAX) {
        snprintf(g_mem[g_memn].name, sizeof g_mem[g_memn].name, "%s", name);
        g_mem[g_memn].rss = rss; g_mem[g_memn].swap = swap; g_mem[g_memn].pss = pss;
        g_memn++;
    }
}

/* Walks /proc/self/smaps and prints the biggest mappings. A mapping whose RSS is
 * mostly file-backed is evictable; a big anonymous one is Unity's own heap. */
static void perf_mem_report(void) {
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f) return;
    g_memn = 0;
    char cur[72] = "[anon]", line[512];
    unsigned long rss = 0, swp = 0, pss = 0;
    int have = 0;
    while (fgets(line, sizeof line, f)) {
        char c = line[0];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {   /* mapping header */
            if (have) mem_add(cur, rss, swp, pss);
            const char *nm = smaps_name(line);
            size_t n = strcspn(nm, "\n");
            if (n >= sizeof cur) n = sizeof cur - 1;
            memcpy(cur, nm, n); cur[n] = 0;
            rss = swp = pss = 0; have = 1;
            continue;
        }
        if (!have) continue;
        if (!strncmp(line, "Rss:", 4))       rss = strtoul(line + 4, NULL, 10);
        else if (!strncmp(line, "Swap:", 5)) swp = strtoul(line + 5, NULL, 10);
        else if (!strncmp(line, "Pss:", 4))  pss = strtoul(line + 4, NULL, 10);
    }
    if (have) mem_add(cur, rss, swp, pss);
    fclose(f);

    for (int i = 0; i < g_memn; i++) {            /* selection sort, biggest RSS first */
        int m = i;
        for (int j = i + 1; j < g_memn; j++)
            if (g_mem[j].rss > g_mem[m].rss) m = j;
        if (m != i) { memrec t = g_mem[i]; g_mem[i] = g_mem[m]; g_mem[m] = t; }
    }
    char buf[900];
    int n = 0;
    for (int i = 0; i < g_memn && i < 8; i++)
        n += snprintf(buf + n, sizeof buf - n, "%s%s=%lu/%lu", i ? ", " : "",
                      g_mem[i].name, g_mem[i].rss / 1024, g_mem[i].swap / 1024);
    so_log("[perf] top mem rss/swap MB: %s", buf);
}

static void perf_tick(void) {
    if (perf_on < 0) {
        const char *e = getenv("UNITY_PERF");
        perf_on = !(e && (!strcmp(e, "0") || !strcmp(e, "off")));
        if (perf_on) {
            long rss = proc_kb("/proc/self/status", "VmRSS:");
            so_log("[perf] profiling on: screen=%dx%d rss=%ldkB", g_screen_w, g_screen_h, rss);
        }
    }
    if (!perf_on) return;

    uint64_t now = now_us();
    if (!perf_last_us) { perf_last_us = now; getrusage(RUSAGE_SELF, &perf_last_ru); return; }
    uint64_t dt = now - perf_last_us;
    perf_last_us = now;

    perf_sum_us += dt;
    perf_n++;
    if (dt < perf_min_us) perf_min_us = dt;
    if (dt > perf_max_us) perf_max_us = dt;
    int b = dt < 8000 ? 0 : dt < 16000 ? 1 : dt < 24000 ? 2 : dt < 33000 ? 3
          : dt < 50000 ? 4 : dt < 100000 ? 5 : dt < 200000 ? 6 : 7;
    perf_buckets[b]++;
    perf_stall_probe(dt);

    if (perf_n < 300) return;

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    uint64_t du = (uint64_t)(ru.ru_utime.tv_sec - perf_last_ru.ru_utime.tv_sec) * 1000000ULL
                + (uint64_t)(ru.ru_utime.tv_usec - perf_last_ru.ru_utime.tv_usec)
                + (uint64_t)(ru.ru_stime.tv_sec - perf_last_ru.ru_stime.tv_sec) * 1000000ULL
                + (uint64_t)(ru.ru_stime.tv_usec - perf_last_ru.ru_stime.tv_usec);
    /* capture the fault deltas before perf_last_ru is overwritten */
    long d_maj = ru.ru_majflt - perf_last_ru.ru_majflt;
    long d_min = ru.ru_minflt - perf_last_ru.ru_minflt;
    perf_last_ru = ru;

    long rss   = proc_kb("/proc/self/status", "VmRSS:");
    long swp   = proc_kb("/proc/self/status", "VmSwap:");
    long stot  = proc_kb("/proc/meminfo",     "SwapTotal:");
    long sfree = proc_kb("/proc/meminfo",     "SwapFree:");

    static const char *const lbl[8] = {"<8", "8-16", "16-24", "24-33", "33-50", "50-100", "100-200", ">200"};
    char hist[256];
    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (!perf_buckets[i]) continue;
        n += snprintf(hist + n, sizeof hist - n, "%s=%u%% ", lbl[i], perf_buckets[i] * 100 / perf_n);
    }
    (void)perf_first;

    so_log("[perf] %u frames | fps=%.1f avg=%.1fms min=%.1f max=%.1f | cpu=%.0f%% | rss=%ldMB swap=%ldMB",
           perf_n, 1e6 * perf_n / (double)perf_sum_us, perf_sum_us / 1000.0 / perf_n,
           perf_min_us / 1000.0, perf_max_us / 1000.0, 100.0 * du / perf_sum_us,
           rss / 1024, swp / 1024);
    /* majflt = pages faulted in from swap/SD. High count => the frame budget is being spent
     * waiting for the SD card, not rendering. minflt = normal page/cache activity. */
    so_log("[perf] page faults: major=%ld (%.2f/frame) minor=%ld (%.1f/frame)",
           d_maj, (double)d_maj / perf_n, d_min, (double)d_min / perf_n);
    so_log("[perf] frame-time histogram (ms): %s", hist);
    if (stot > 0) so_log("[perf] system swap: used %ldMB / %ldMB", (stot - sfree) / 1024, stot / 1024);
    perf_mem_report();

    /* Also drains the block-buffered log, so a hard crash loses at most ~300 frames of output. */
    so_log_flush();

    perf_sum_us = 0; perf_min_us = ~0ULL; perf_max_us = 0; perf_n = 0;
    memset(perf_buckets, 0, sizeof perf_buckets);
}

/* ========================================================================== */
/* ALooper / ASensor / ANativeWindow                                            */
/* ========================================================================== */

static void *an_looper_prepare(int opts) { (void)opts; return (void *)0x1000; }
static void *an_looper_for_thread(void) { return (void *)0x1000; }

static void *an_sensor_manager(void) { return (void *)0x2000; }
static void *an_sensor_null(void) { return NULL; }
static int an_sensor_zero(void) { return 0; }
static void *an_sensor_queue(void) { return (void *)0x2100; }
static const char *an_sensor_str(void) { return ""; }

static void *an_window_from_surface(void *env, void *surface) { (void)env; (void)surface; return (void *)0x3000; }
static int an_window_w(void *w) { (void)w; return g_screen_w; }
static int an_window_h(void *w) { (void)w; return g_screen_h; }
static int an_zero(void) { return 0; }
static void an_void(void) { }

/* ========================================================================== */
/* null GLES                                                                     */
/* ========================================================================== */

static atomic_uint g_gl_name = 0;
static uint64_t gl_noop(void) { return 0; }

static void gl_gen(int n, uint32_t *ids) { for (int i = 0; i < n; i++) ids[i] = ++g_gl_name; }
static uint32_t gl_create(uint32_t type) { (void)type; return ++g_gl_name; }

static const char *gl_get_string(uint32_t name) {
    switch (name) {
    case 0x1F00: return "NullGL";
    case 0x1F01: return "NullGL (Mali-G31 emulation)";
    case 0x1F02: return "OpenGL ES 3.0 NullGL";
    case 0x1F03:
        return "GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_vertex_array_object "
               "GL_EXT_color_buffer_half_float GL_KHR_texture_compression_astc_ldr GL_EXT_texture_filter_anisotropic";
    case 0x8B8C: return "OpenGL ES GLSL ES 3.00";
    default: return "";
    }
}
static const char *gl_get_stringi(uint32_t name, uint32_t i) { (void)name; (void)i; return NULL; }

static void gl_get_integerv(uint32_t pname, int32_t *d) {
    int32_t v = 0;
    switch (pname) {
    case 0x0D33: case 0x851C: case 0x84E8: v = 4096; break;       /* MAX_TEXTURE_SIZE, CUBE, RENDERBUFFER */
    case 0x8869: case 0x8872: case 0x8B4C: v = 16; break;         /* VERTEX_ATTRIBS, TEXTURE_IMAGE_UNITS, VERTEX_TEX */
    case 0x8B4D: v = 32; break;                                   /* COMBINED_TEXTURE_IMAGE_UNITS */
    case 0x8DFB: case 0x8DFD: v = 256; break;                     /* UNIFORM_VECTORS */
    case 0x8DFC: v = 15; break;                                   /* VARYING_VECTORS */
    case 0x8824: case 0x8CDF: case 0x8D57: v = 4; break;          /* DRAW_BUFFERS, COLOR_ATTACHMENTS, SAMPLES */
    case 0x821B: v = 3; break;                                    /* MAJOR_VERSION */
    case 0x88FF: case 0x8073: v = 256; break;                     /* MAX_ARRAY_TEXTURE_LAYERS, MAX_3D_TEXTURE_SIZE */
    case 0x0BA2: d[0] = 0; d[1] = 0; d[2] = g_screen_w; d[3] = g_screen_h; return;   /* VIEWPORT */
    case 0x0C10: d[0] = 0; d[1] = 0; d[2] = g_screen_w; d[3] = g_screen_h; return;   /* SCISSOR_BOX */
    case 0x0D3A: d[0] = d[1] = 4096; return;                      /* MAX_VIEWPORT_DIMS */
    case 0x0D57: v = 8; break;                                    /* ... bits */
    default: v = 0; break;
    }
    d[0] = v;
}
static void gl_get_floatv(uint32_t pname, float *d) {
    int32_t i[4] = {0, 0, 0, 0};
    gl_get_integerv(pname, i);
    d[0] = (float)i[0];
}
static void gl_get_shaderiv(uint32_t s, uint32_t pname, int32_t *p) { (void)s; *p = (pname == 0x8B81) ? 1 : 0; }  /* COMPILE_STATUS */
static void gl_get_programiv(uint32_t s, uint32_t pname, int32_t *p) {
    (void)s;
    *p = (pname == 0x8B82 || pname == 0x8B83) ? 1 : 0;   /* LINK_STATUS, VALIDATE_STATUS */
}
static uint32_t gl_check_fb(uint32_t t) { (void)t; return 0x8CD5; }   /* FRAMEBUFFER_COMPLETE */
static int32_t gl_loc(void) { return 0; }
static void gl_get_info_log(uint32_t s, int32_t max, int32_t *len, char *log) {
    (void)s; if (len) *len = 0; if (max > 0 && log) log[0] = 0;
}
static uint8_t gl_is_true(void) { return 1; }

typedef struct { const char *name; void *fn; } glent;
static const glent g_gl_table[] = {
    {"glGetString", gl_get_string}, {"glGetStringi", gl_get_stringi}, {"glGetIntegerv", gl_get_integerv},
    {"glGetFloatv", gl_get_floatv}, {"glGetShaderiv", gl_get_shaderiv}, {"glGetProgramiv", gl_get_programiv},
    {"glCheckFramebufferStatus", gl_check_fb}, {"glGetUniformLocation", gl_loc}, {"glGetAttribLocation", gl_loc},
    {"glGetShaderInfoLog", gl_get_info_log}, {"glGetProgramInfoLog", gl_get_info_log},
    {"glGenTextures", gl_gen}, {"glGenBuffers", gl_gen}, {"glGenFramebuffers", gl_gen},
    {"glGenRenderbuffers", gl_gen}, {"glGenVertexArrays", gl_gen}, {"glGenQueries", gl_gen},
    {"glGenSamplers", gl_gen}, {"glGenTransformFeedbacks", gl_gen},
    {"glCreateShader", gl_create}, {"glCreateProgram", gl_create},
    {"glIsTexture", gl_is_true}, {"glIsBuffer", gl_is_true}, {"glIsFramebuffer", gl_is_true},
    {"glIsRenderbuffer", gl_is_true}, {"glIsProgram", gl_is_true}, {"glIsShader", gl_is_true},
    {NULL, NULL}};

static void *null_gl_get_proc(const char *name) {
    if (strncmp(name, "gl", 2) != 0) return NULL;
    for (int i = 0; g_gl_table[i].name; i++)
        if (strcmp(g_gl_table[i].name, name) == 0) return g_gl_table[i].fn;
    return (void *)gl_noop;
}
void *android_gl_proc(const char *name) {
    if (sdl_gl_active()) return sdl_gl_get_proc(name);
    return null_gl_get_proc(name);
}

/* ========================================================================== */
/* EGL                                                                           */
/* ========================================================================== */

static __thread void *t_cur_ctx, *t_cur_surf;
#define EGL_SUCCESS 0x3000

static void *egl_get_display(void *native) { (void)native; return (void *)0x4000; }
static unsigned egl_initialize(void *dpy, int *maj, int *min) { (void)dpy; if (maj) *maj = 1; if (min) *min = 4; return 1; }
static const char *egl_query_string(void *dpy, int name) {
    (void)dpy;
    return name == 0x3055 /* EGL_EXTENSIONS */ ? "EGL_KHR_surfaceless_context" : name == 0x3053 ? "NullEGL" : "1.4 NullEGL";
}
static unsigned egl_choose_config(void *dpy, const int *attrs, void **configs, int size, int *num) {
    (void)dpy; (void)attrs;
    if (size > 0 && configs) configs[0] = (void *)0x4100;
    if (num) *num = 1;
    return 1;
}
static unsigned egl_get_config_attrib(void *dpy, void *cfg, int attr, int *v) {
    (void)dpy; (void)cfg;
    switch (attr) {
    case 0x3024: case 0x3023: case 0x3022: case 0x3021: *v = 8; break;    /* R G B A size */
    case 0x3025: *v = 24; break;                                          /* depth */
    case 0x3026: *v = 8; break;                                           /* stencil */
    case 0x302E: *v = 1; break;                                           /* native visual id */
    case 0x3040: *v = 0x44; break;                                        /* renderable: ES2 | ES3 */
    case 0x3033: *v = 0x5; break;                                         /* surface type: window|pbuffer */
    default: *v = 0; break;
    }
    return 1;
}
static void *egl_create_context(void *d, void *c, void *share, const int *a) {
    (void)d; (void)c; (void)share; (void)a;
    if (sdl_gl_active()) return sdl_gl_create_context();
    return (void *)0x4200;
}
static void *egl_create_surface(void) { return (void *)0x4300; }
static unsigned egl_make_current(void *d, void *draw, void *read, void *ctx) {
    (void)d; (void)read;
    if (sdl_gl_active() && !sdl_gl_make_current(ctx)) return 0;
    t_cur_surf = draw; t_cur_ctx = ctx;
    return 1;
}
static void *egl_get_current_context(void) { return t_cur_ctx; }
static void *egl_get_current_surface(int which) { (void)which; return t_cur_surf; }
static int s_cur_swap_interval = 1;

static unsigned egl_swap_buffers(void *d, void *s) {
    (void)d; (void)s;
    perf_tick();

    static int s_target_fps = -1;
    static uint64_t s_target_frame_us = 0;
    static uint64_t s_last_swap_us = 0;

    if (s_target_fps == -1) {
        const char *env = getenv("UNITY_FPS");
        s_target_fps = env ? atoi(env) : 0;
        if (s_target_fps == 30) s_cur_swap_interval = 2;
        const char *swap_env = getenv("UNITY_SWAP_INTERVAL");
        if (swap_env) s_cur_swap_interval = atoi(swap_env);

        if (s_cur_swap_interval >= 2) {
            so_log("[egl] Hardware %d FPS VSYNC active (SwapInterval=%d), software limiter disabled",
                   60 / s_cur_swap_interval, s_cur_swap_interval);
            s_target_frame_us = 0;
        } else if (s_target_fps > 0) {
            s_target_frame_us = 1000000ULL / s_target_fps;
            so_log("[egl] Frame limiter enabled: %d FPS (%llu us/frame)", s_target_fps, (unsigned long long)s_target_frame_us);
        } else {
            so_log("[egl] Frame limiter disabled (native 60 FPS VSYNC)");
        }
    }

    /* If SwapInterval is already >= 2 (hardware 30 FPS VSYNC), let the display driver handle
     * frame pacing directly. Double-sleeping with nanosleep pushes the swap past the 2nd VBLANK
     * and causes severe frame drops to 15 FPS!
     */
    if (s_cur_swap_interval < 2 && s_target_frame_us > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
        if (s_last_swap_us > 0) {
            uint64_t elapsed = now_us - s_last_swap_us;
            if (elapsed < s_target_frame_us) {
                uint64_t sleep_us = s_target_frame_us - elapsed;
                if (sleep_us > 2000) {
                    struct timespec req;
                    req.tv_sec = (sleep_us - 1000) / 1000000ULL;
                    req.tv_nsec = ((sleep_us - 1000) % 1000000ULL) * 1000;
                    nanosleep(&req, NULL);
                }
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        s_last_swap_us = (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
    }

    if (sdl_gl_active()) { sdl_gl_swap(); return 1; }
    return 1;
}
static unsigned egl_query_surface(void *d, void *s, int attr, int *v) {
    (void)d; (void)s;
    *v = attr == 0x3057 ? g_screen_w : attr == 0x3056 ? g_screen_h : 0;
    return 1;
}
static unsigned egl_one(void) { return 1; }
static unsigned egl_swap_interval(void *d, int n) {
    (void)d;
    const char *fps_env = getenv("UNITY_FPS");
    int fps = fps_env ? atoi(fps_env) : 0;
    int want = n;
    if (fps == 30) want = 2;
    const char *swap_env = getenv("UNITY_SWAP_INTERVAL");
    if (swap_env) want = atoi(swap_env);
    if (want != s_cur_swap_interval)
        so_log("[egl] engine requested SwapInterval=%d -> using %d%s", n, want,
               swap_env ? " (forced by UNITY_SWAP_INTERVAL)" : fps == 30 ? " (forced by UNITY_FPS=30)" : "");
    s_cur_swap_interval = want;
    if (sdl_gl_active()) sdl_gl_swap_interval(want);
    return 1;
}
static int egl_get_error(void) { return EGL_SUCCESS; }
static void *egl_get_proc_address(const char *name) {
    void *p = android_gl_proc(name);
    if (!p) so_log("[egl] eglGetProcAddress(%s) -> NULL", name);
    return p;
}

/* ========================================================================== */

#define R(n, f) shim_register(n, (void *)(f))

void android_native_init(void) {
    R("ALooper_prepare", an_looper_prepare);
    R("ALooper_forThread", an_looper_for_thread);
    R("ASensorManager_getInstance", an_sensor_manager);
    R("ASensorManager_getDefaultSensor", an_sensor_null);
    R("ASensorManager_getSensorList", an_sensor_zero);
    R("ASensorManager_createEventQueue", an_sensor_queue);
    R("ASensorManager_destroyEventQueue", an_zero);
    R("ASensorEventQueue_enableSensor", an_zero);
    R("ASensorEventQueue_disableSensor", an_zero);
    R("ASensorEventQueue_setEventRate", an_zero);
    R("ASensorEventQueue_hasEvents", an_zero);
    R("ASensorEventQueue_getEvents", an_zero);
    R("ASensor_getMinDelay", an_zero);
    R("ASensor_getName", an_sensor_str);
    R("ASensor_getVendor", an_sensor_str);
    R("ASensor_getType", an_zero);
    R("ASensor_getResolution", an_zero);
    R("ANativeWindow_fromSurface", an_window_from_surface);
    R("ANativeWindow_getWidth", an_window_w);
    R("ANativeWindow_getHeight", an_window_h);
    R("ANativeWindow_setBuffersGeometry", an_zero);
    R("ANativeWindow_acquire", an_void);
    R("ANativeWindow_release", an_void);

    R("eglGetDisplay", egl_get_display);
    R("eglInitialize", egl_initialize);
    R("eglTerminate", egl_one);
    R("eglQueryString", egl_query_string);
    R("eglChooseConfig", egl_choose_config);
    R("eglGetConfigAttrib", egl_get_config_attrib);
    R("eglCreateContext", egl_create_context);
    R("eglDestroyContext", egl_one);
    R("eglCreateWindowSurface", egl_create_surface);
    R("eglCreatePbufferSurface", egl_create_surface);
    R("eglDestroySurface", egl_one);
    R("eglMakeCurrent", egl_make_current);
    R("eglGetCurrentContext", egl_get_current_context);
    R("eglGetCurrentSurface", egl_get_current_surface);
    R("eglSwapBuffers", egl_swap_buffers);
    R("eglSwapInterval", egl_swap_interval);
    R("eglQuerySurface", egl_query_surface);
    R("eglGetError", egl_get_error);
    R("eglGetProcAddress", egl_get_proc_address);
}
