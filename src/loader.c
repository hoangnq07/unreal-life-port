/*
 * Minimal ELF loader for Android (bionic) aarch64 shared objects running on glibc.
 *
 * Supports exactly what the Unity 2018.4 libs need (verified from the APK):
 *   - DT_RELA / DT_JMPREL with R_AARCH64_{RELATIVE,ABS64,GLOB_DAT,JUMP_SLOT}
 *   - DT_HASH symbol lookup, DT_INIT / DT_INIT_ARRAY
 *   - no TLS segments, no RELR, no packed Android relocations
 */
#define _GNU_SOURCE
#include "loader.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>

#define PAGE_SZ 4096UL
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

#ifndef R_AARCH64_ABS64
#define R_AARCH64_ABS64 257
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE 1027
#define R_AARCH64_IRELATIVE 1032
#endif

int so_verbose = 0;
static so_lib *g_libs = NULL;
static so_lib *g_tail = NULL;
static char g_libdir[512] = ".";
static int g_argc = 0;
static char **g_argv = NULL;
static so_lib g_builtin = {.name = "<builtin>", .builtin = 1, .initialized = 1};

void so_log(const char *fmt, ...) {
    /* One formatted buffer + one fwrite instead of four unbuffered stdio calls.
     * stderr is block-buffered (see main.c), so this costs ~1 syscall per 64 KB
     * instead of 1 per line - the log lives on the SD card, and an unbuffered
     * write per line is a measurable stall in the render loop. */
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(buf, sizeof buf, "[so] ");
    n += vsnprintf(buf + n, sizeof buf - n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    fwrite(buf, 1, n, stderr);
}

/* Flush the (block-buffered) log; called at milestones and before any exit. */
void so_log_flush(void) { fflush(stderr); }

void so_set_libdir(const char *dir) { snprintf(g_libdir, sizeof g_libdir, "%s", dir); }
const char *so_get_libdir(void) { return g_libdir; }
void so_set_args(int argc, char **argv) { g_argc = argc; g_argv = argv; }
so_lib *so_first(void) { return g_libs; }
so_lib *so_builtin(void) { return &g_builtin; }

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

so_lib *so_find(const char *name) {
    name = base_name(name);
    for (so_lib *l = g_libs; l; l = l->next)
        if (strcmp(l->name, name) == 0) return l;
    return NULL;
}

/* ---- stubs for unresolved imports: log the name and abort when called ---- */
typedef struct {
    uint32_t code[8];           /* 32 bytes: ldr x0,#16 ; ldr x16,#20 ; br x16 ; nop ; .quad name ; .quad handler */
} stub_t;

static void unresolved_called(const char *name) {
    fprintf(stderr, "\n[so] FATAL: call to unresolved import '%s'\n", name);
    fflush(stderr);
    abort();
}

static stub_t *g_stub_pool = NULL;
static size_t g_stub_used = 0, g_stub_cap = 0;

static void *make_stub(const char *name) {
    if (!g_stub_pool || g_stub_used == g_stub_cap) {
        g_stub_cap = 2048;
        g_stub_pool = mmap(NULL, g_stub_cap * sizeof(stub_t), PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_stub_pool == MAP_FAILED) { perror("mmap stub pool"); abort(); }
        g_stub_used = 0;
    }
    stub_t *s = &g_stub_pool[g_stub_used++];
    s->code[0] = 0x58000080;    /* ldr x0, [pc, #16] */
    s->code[1] = 0x580000B0;    /* ldr x16, [pc, #20] */
    s->code[2] = 0xD61F0200;    /* br  x16 */
    s->code[3] = 0xD503201F;    /* nop */
    char *dup = strdup(name);
    uint64_t *lit = (uint64_t *)&s->code[4];
    lit[0] = (uint64_t)(uintptr_t)dup;
    lit[1] = (uint64_t)(uintptr_t)unresolved_called;
    __builtin___clear_cache((char *)s, (char *)(s + 1));
    return s;
}

/* ---- symbol lookup ---- */
static uint32_t sysv_hash(const char *s) {
    uint32_t h = 0, g;
    for (; *s; s++) {
        h = (h << 4) + (unsigned char)*s;
        g = h & 0xf0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

void *so_lib_sym(so_lib *lib, const char *name) {
    if (!lib || lib->builtin || !lib->hash) return NULL;
    uint32_t nb = lib->hash[0];
    const uint32_t *bucket = lib->hash + 2;
    const uint32_t *chain = bucket + nb;
    for (uint32_t i = bucket[sysv_hash(name) % nb]; i; i = chain[i]) {
        const Elf64_Sym *s = &lib->symtab[i];
        if (s->st_shndx == SHN_UNDEF) continue;
        int bind = ELF64_ST_BIND(s->st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        if (strcmp(lib->strtab + s->st_name, name) == 0) return lib->base + s->st_value;
    }
    return NULL;
}

void *so_resolve_global(const char *name, so_lib *skip) {
    void *p = shim_lookup(name);
    if (p) return p;
    for (so_lib *l = g_libs; l; l = l->next) {
        if (l == skip) continue;
        p = so_lib_sym(l, name);
        if (p) return p;
    }
    return dlsym(RTLD_DEFAULT, name);
}

/* ---- loading ---- */
static int is_system_lib(const char *n) {
    static const char *const sys[] = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libstdc++.so", "libz.so",
                                      "libandroid.so", "libEGL.so", "libGLESv2.so", "libGLESv3.so", "libOpenSLES.so",
                                      "libmediandk.so", "libvulkan.so", NULL};
    for (int i = 0; sys[i]; i++)
        if (strcmp(sys[i], n) == 0) return 1;
    return 0;
}

static int prot_of(uint32_t f) {
    return ((f & PF_R) ? PROT_READ : 0) | ((f & PF_W) ? PROT_WRITE : 0) | ((f & PF_X) ? PROT_EXEC : 0);
}

static int apply_rela(so_lib *lib, const Elf64_Rela *r, size_t count, size_t *unresolved) {
    for (size_t i = 0; i < count; i++, r++) {
        uint32_t type = ELF64_R_TYPE(r->r_info);
        uint32_t symi = ELF64_R_SYM(r->r_info);
        uint64_t *where = (uint64_t *)(lib->base + r->r_offset);
        uint64_t S = 0;
        if (type == R_AARCH64_RELATIVE) {
            *where = (uint64_t)(uintptr_t)lib->base + r->r_addend;
            continue;
        }
        if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT) {
            so_log("%s: unsupported relocation type %u at 0x%lx", lib->name, type, (unsigned long)r->r_offset);
            return -1;
        }
        if (symi) {
            const Elf64_Sym *sym = &lib->symtab[symi];
            const char *nm = lib->strtab + sym->st_name;
            if (sym->st_shndx != SHN_UNDEF) {
                S = (uint64_t)(uintptr_t)(lib->base + sym->st_value);
            } else {
                void *p = so_resolve_global(nm, lib);
                if (!p) {
                    if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
                        p = NULL;
                    } else {
                        p = make_stub(nm);
                        (*unresolved)++;
                        if (so_verbose || !shim_is_deferred(nm))
                            so_log("%s: UNRESOLVED import '%s' -> stub", lib->name, nm);
                    }
                } else if (so_verbose > 1) {
                    so_log("%s: %s -> %p", lib->name, nm, p);
                }
                S = (uint64_t)(uintptr_t)p;
            }
        }
        *where = S + (type == R_AARCH64_JUMP_SLOT ? 0 : r->r_addend);
    }
    return 0;
}

so_lib *so_load(const char *path, const char *name) {
    const char *bn = base_name(name ? name : path);
    so_lib *existing = so_find(bn);
    if (existing) return existing;

    int fd = open(path, O_RDONLY);
    if (fd < 0) { so_log("open(%s): %s", path, strerror(errno)); return NULL; }

    Elf64_Ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) != sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_machine != EM_AARCH64 || eh.e_type != ET_DYN) {
        so_log("%s: not an aarch64 ET_DYN ELF", path);
        close(fd);
        return NULL;
    }
    size_t phsz = (size_t)eh.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *ph = malloc(phsz);
    if (pread(fd, ph, phsz, eh.e_phoff) != (ssize_t)phsz) { close(fd); free(ph); return NULL; }

    uint64_t lo = ~0ULL, hi = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
    }
    lo = ALIGN_DOWN(lo, PAGE_SZ);
    size_t span = ALIGN_UP(hi - lo, PAGE_SZ);
    const size_t ALIGN = 0x10000;

    uint8_t *res = mmap(NULL, span + ALIGN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (res == MAP_FAILED) { so_log("%s: reserve %zu bytes failed: %s", path, span, strerror(errno)); close(fd); free(ph); return NULL; }
    uint8_t *base = (uint8_t *)ALIGN_UP((uintptr_t)res, ALIGN) - lo;

    for (int i = 0; i < eh.e_phnum; i++) {
        const Elf64_Phdr *p = &ph[i];
        if (p->p_type != PT_LOAD) continue;
        uint64_t seg = ALIGN_DOWN(p->p_vaddr, PAGE_SZ);
        uint64_t off = ALIGN_DOWN(p->p_offset, PAGE_SZ);
        uint64_t fsz = p->p_filesz + (p->p_vaddr - seg);
        int prot = prot_of(p->p_flags);
        if (p->p_filesz) {
            void *m = mmap(base + seg, ALIGN_UP(fsz, PAGE_SZ), prot | ((p->p_flags & PF_W) ? 0 : 0),
                           MAP_PRIVATE | MAP_FIXED, fd, off);
            if (m == MAP_FAILED) { so_log("%s: mmap segment %d failed: %s", path, i, strerror(errno)); close(fd); free(ph); return NULL; }
        }
        if (p->p_memsz > p->p_filesz) {
            uint64_t bss_start = p->p_vaddr + p->p_filesz;
            uint64_t bss_end = p->p_vaddr + p->p_memsz;
            uint64_t map_from = ALIGN_UP(bss_start, PAGE_SZ);
            if ((p->p_flags & PF_W) && (bss_start & (PAGE_SZ - 1)))
                memset(base + bss_start, 0, map_from - bss_start);
            if (ALIGN_UP(bss_end, PAGE_SZ) > map_from) {
                void *m = mmap(base + map_from, ALIGN_UP(bss_end, PAGE_SZ) - map_from, prot,
                               MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
                if (m == MAP_FAILED) { so_log("%s: bss map failed", path); close(fd); free(ph); return NULL; }
            }
        }
    }
    close(fd);

    so_lib *lib = calloc(1, sizeof *lib);
    snprintf(lib->name, sizeof lib->name, "%s", bn);
    snprintf(lib->path, sizeof lib->path, "%s", path);
    lib->base = base;
    lib->size = span;
    lib->phnum = eh.e_phnum;
    lib->phdr = (const Elf64_Phdr *)(base + eh.e_phoff);   /* phdrs live inside the first PT_LOAD for these libs */

    const Elf64_Rela *rela = NULL, *jmprel = NULL;
    size_t relasz = 0, pltrelsz = 0, init_arraysz = 0;
    uint64_t init_array_off = 0;
    for (int i = 0; i < eh.e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) lib->dyn = (const Elf64_Dyn *)(base + ph[i].p_vaddr);
    free(ph);
    if (!lib->dyn) { so_log("%s: no PT_DYNAMIC", path); return NULL; }

    const char *needed[32];
    uint64_t needed_off[32];
    int nneeded = 0;
    uint64_t strtab_off = 0;
    for (const Elf64_Dyn *d = lib->dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB: lib->strtab = (const char *)(base + d->d_un.d_ptr); strtab_off = d->d_un.d_ptr; break;
        case DT_SYMTAB: lib->symtab = (const Elf64_Sym *)(base + d->d_un.d_ptr); break;
        case DT_HASH: lib->hash = (const uint32_t *)(base + d->d_un.d_ptr); lib->nsyms = lib->hash[1]; break;
        case DT_RELA: rela = (const Elf64_Rela *)(base + d->d_un.d_ptr); break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = (const Elf64_Rela *)(base + d->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        case DT_INIT: lib->init = (void (*)(void))(base + d->d_un.d_ptr); break;
        case DT_INIT_ARRAY: init_array_off = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: init_arraysz = d->d_un.d_val; break;
        case DT_NEEDED: if (nneeded < 32) needed_off[nneeded++] = d->d_un.d_val; break;
        default: break;
        }
    }
    (void)strtab_off;
    if (init_array_off) {
        lib->init_array = (void (**)(void))(base + init_array_off);
        lib->init_count = init_arraysz / sizeof(void *);
    }
    for (int i = 0; i < nneeded; i++) needed[i] = lib->strtab + needed_off[i];

    /* register before resolving deps so self/mutual lookups work */
    if (g_tail) g_tail->next = lib; else g_libs = lib;
    g_tail = lib;

    for (int i = 0; i < nneeded; i++) {
        if (is_system_lib(needed[i])) continue;
        if (so_find(needed[i])) continue;
        char dep[640];
        snprintf(dep, sizeof dep, "%s/%s", g_libdir, needed[i]);
        so_log("%s needs %s -> loading", lib->name, needed[i]);
        if (!so_load(dep, needed[i])) so_log("WARNING: could not load dependency %s", needed[i]);
    }

    size_t unresolved = 0;
    if (rela && apply_rela(lib, rela, relasz / sizeof(Elf64_Rela), &unresolved)) return NULL;
    if (jmprel && apply_rela(lib, jmprel, pltrelsz / sizeof(Elf64_Rela), &unresolved)) return NULL;

    so_log("loaded %-28s base=%p size=%zu MB unresolved=%zu", lib->name, (void *)base, span >> 20, unresolved);
    return lib;
}

void so_run_init(so_lib *lib) {
    if (!lib || lib->initialized || lib->builtin) return;
    lib->initialized = 1;
    extern char **environ;
    typedef void (*init_fn)(int, char **, char **);
    if (lib->init) {
        if (so_verbose) so_log("%s: DT_INIT %p", lib->name, (void *)lib->init);
        ((init_fn)lib->init)(g_argc, g_argv, environ);
    }
    for (size_t i = 0; i < lib->init_count; i++) {
        init_fn fn = (init_fn)lib->init_array[i];
        if (!fn || (intptr_t)fn == -1) continue;
        if (so_verbose) so_log("%s: init_array[%zu] %p", lib->name, i, (void *)fn);
        fn(g_argc, g_argv, environ);
    }
    so_log("initialized %s (%zu constructors)", lib->name, lib->init_count);
}
