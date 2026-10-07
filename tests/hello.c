#include <stdio.h>
#include <pthread.h>
#include <dlfcn.h>

int main(void) {
    printf("hello from aarch64, pointer size=%zu\n", sizeof(void *));
    printf("sizeof(pthread_mutex_t)=%zu (bionic=40)\n", sizeof(pthread_mutex_t));
    printf("sizeof(pthread_attr_t)=%zu (bionic=56)\n", sizeof(pthread_attr_t));
    printf("sizeof(jmp_buf) glibc differs from bionic\n");
    return 0;
}
