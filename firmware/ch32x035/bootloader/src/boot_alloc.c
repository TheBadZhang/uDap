/*
 * bootloader 专用的极简堆分配器。
 *
 * bootuf2（CherryDAP 子模块）用 malloc/free 生成 INFO_UF2.TXT 的内容，
 * 如果直接用 newlib 的 nano-malloc，会连带拉进 _reent / 锁 / cleanup 等
 * 一整套代码（实测约 1.2 KiB），对只有 16 KiB 预算的 bootloader 太贵。
 *
 * 这里的语义足够 bootuf2 使用：
 *   malloc   从 _end 往 _heap_end（栈底）线性分配，只增不减
 *   free     空操作（只有一次 SetReason 会释放上一次的字符串，泄漏量可忽略）
 *   calloc   置零
 *   realloc  保留旧内容的前 min(old, new) 字节
 *
 * 每次分配带 8 字节头记录长度，用于 realloc。没有 free 链表，也没有碎片问题。
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern char _end[];
extern char _heap_end[];

#define BOOT_ALLOC_ALIGN 8u
#define BOOT_ALLOC_HEADER 8u

static char *s_brk;

static size_t align_up(size_t value) {
    return (value + (BOOT_ALLOC_ALIGN - 1u)) & ~(size_t)(BOOT_ALLOC_ALIGN - 1u);
}

static void *boot_alloc(size_t size) {
    char *block;
    size_t need;

    if (s_brk == NULL) {
        s_brk = (char *)(((uintptr_t)_end + BOOT_ALLOC_ALIGN - 1u) & ~(uintptr_t)(BOOT_ALLOC_ALIGN - 1u));
    }

    need = align_up(size);
    if (need > (size_t)(_heap_end - s_brk) - BOOT_ALLOC_HEADER) {
        return NULL;
    }

    block = s_brk;
    s_brk += BOOT_ALLOC_HEADER + need;

    *(size_t *)block = size;
    return block + BOOT_ALLOC_HEADER;
}

void *malloc(size_t size) {
    return boot_alloc(size);
}

void free(void *ptr) {
    // 线性分配器：不回收。bootloader 生命周期内只有几次分配。
    (void)ptr;
}

void *calloc(size_t count, size_t size) {
    size_t total = count * size;
    void *ptr = boot_alloc(total);

    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    size_t old_size;
    void *fresh;

    if (ptr == NULL) {
        return boot_alloc(size);
    }
    if (size == 0u) {
        return NULL;
    }

    old_size = *(size_t *)((char *)ptr - BOOT_ALLOC_HEADER);
    fresh = boot_alloc(size);
    if (fresh == NULL) {
        return NULL;
    }

    memcpy(fresh, ptr, old_size < size ? old_size : size);
    return fresh;
}
