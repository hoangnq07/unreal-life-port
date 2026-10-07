#ifndef LOADER_H
#define LOADER_H

#include <elf.h>
#include <stddef.h>
#include <stdint.h>

/* A bionic (Android) aarch64 shared object loaded into our glibc process. */
typedef struct so_lib {
    char name[96];
    char path[512];
    uint8_t *base;              /* load bias */
    size_t size;
    const Elf64_Phdr *phdr;     /* program headers (inside the mapped image) */
    int phnum;
    const Elf64_Dyn *dyn;
    const Elf64_Sym *symtab;
    const char *strtab;
    const uint32_t *hash;       /* DT_HASH */
    size_t nsyms;
    void (*init)(void);
    void (**init_array)(void);
    size_t init_count;
    int builtin;                /* pseudo handle for libc/libm/... */
    int initialized;
    struct so_lib *next;
} so_lib;

void so_set_libdir(const char *dir);
const char *so_get_libdir(void);
void so_set_args(int argc, char **argv);

/* Maps + relocates an Android .so (does not run constructors). NEEDED app libs are loaded first. */
so_lib *so_load(const char *path, const char *name);
void so_run_init(so_lib *lib);
void *so_lib_sym(so_lib *lib, const char *name);
so_lib *so_find(const char *basename);
so_lib *so_first(void);
so_lib *so_builtin(void);

/* Search shim table, then loaded libs, then host glibc. Returns NULL when unresolved. */
void *so_resolve_global(const char *name, so_lib *skip);

/* shim table (shim.c) */
void *shim_lookup(const char *name);
int shim_is_deferred(const char *name);

void so_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void so_log_flush(void);
extern int so_verbose;

#endif
