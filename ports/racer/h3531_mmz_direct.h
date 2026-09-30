#ifndef H3531_MMZ_DIRECT_H
#define H3531_MMZ_DIRECT_H

/*
 * Minimal /dev/mmz_userdev ABI used by classic 32-bit HiSilicon SDKs.
 *
 * Two layouts are supported because older Hi35xx kernels use a 16-byte
 * MMB name (80-byte ioctl payload), while newer OSAL trees use 32 bytes
 * (96-byte payload).  Runtime probes both and falls back cleanly.
 */
#include <stdint.h>
#include <sys/ioctl.h>

#define H3531_MMZ_PROT_FLAGS 0x00000103U /* PROT_READ|PROT_WRITE + MAP_SHARED<<8 */

typedef struct __attribute__((aligned(8))) {
    uint32_t phys_addr;
    uint32_t align;
    uint32_t size;
    uint32_t order;
    uint32_t mapped;
    uint32_t w32_stuf;
    char mmb_name[16];
    char mmz_name[32];
    uint32_t gfp;
    uint32_t pad;
} h3531_mmb80_t;

typedef struct __attribute__((aligned(8))) {
    uint32_t phys_addr;
    uint32_t align;
    uint32_t size;
    uint32_t order;
    uint32_t mapped;
    uint32_t w32_stuf;
    char mmb_name[32];
    char mmz_name[32];
    uint32_t gfp;
    uint32_t pad;
} h3531_mmb96_t;

typedef struct {
    uint32_t dirty_phys_start;
    uint32_t dirty_virt_start;
    uint32_t dirty_size;
} h3531_mmz_dirty_t;

_Static_assert(sizeof(h3531_mmb80_t)==80,"MMZ legacy ABI");
_Static_assert(sizeof(h3531_mmb96_t)==96,"MMZ OSAL ABI");
_Static_assert(sizeof(h3531_mmz_dirty_t)==12,"MMZ dirty ABI");

#define H3531_MMB80_ALLOC       _IOWR('m',10,h3531_mmb80_t)
#define H3531_MMB80_FREE        _IOW ('m',12,h3531_mmb80_t)
#define H3531_MMB80_ALLOC_V2    _IOWR('m',13,h3531_mmb80_t)
#define H3531_MMB80_REMAP       _IOWR('m',20,h3531_mmb80_t)
#define H3531_MMB80_REMAP_CACHE _IOWR('m',21,h3531_mmb80_t)
#define H3531_MMB80_UNMAP       _IOWR('m',22,h3531_mmb80_t)

#define H3531_MMB96_ALLOC       _IOWR('m',10,h3531_mmb96_t)
#define H3531_MMB96_FREE        _IOW ('m',12,h3531_mmb96_t)
#define H3531_MMB96_ALLOC_V2    _IOWR('m',13,h3531_mmb96_t)
#define H3531_MMB96_REMAP       _IOWR('m',20,h3531_mmb96_t)
#define H3531_MMB96_REMAP_CACHE _IOWR('m',21,h3531_mmb96_t)
#define H3531_MMB96_UNMAP       _IOWR('m',22,h3531_mmb96_t)

#define H3531_MMZ_FLUSH_ALL     _IO('c',40)
#define H3531_MMZ_FLUSH_DIRTY   _IOW('d',50,h3531_mmz_dirty_t)

#endif
