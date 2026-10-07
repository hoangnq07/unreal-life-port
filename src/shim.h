#ifndef SHIM_H
#define SHIM_H

#include <pthread.h>

void shim_init(void);
void shim_register(const char *name, void *addr);
const char *shim_property(const char *name);
/* guest-created threads (for the debugging watchdog) */
int shim_threads(pthread_t *out, void **fn, int max);

#endif
