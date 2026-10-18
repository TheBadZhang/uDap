#include "bsp/bsp_print.h"

#include <stddef.h>

int _write(int fd, char *buf, int size);
void *_sbrk(ptrdiff_t incr);

void print_init(uint32_t baudrate) {
    (void)baudrate;
}

__attribute__((used)) int _write(int fd, char *buf, int size) {
    (void)fd;
    (void)buf;
    return size;
}

__attribute__((used)) void *_sbrk(ptrdiff_t incr) {
    extern char _end[];
    extern char _heap_end[];
    static char *curbrk = _end;

    if ((curbrk + incr < _end) || (curbrk + incr > _heap_end)) return (void *)-1;

    curbrk += incr;
    return curbrk - incr;
}
