/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stddef.h>

#if SD_BOOT
#  include "efi-string.h"
#else
#  include <string.h>
#endif

#include "macro-fundamental.h"

#if !SD_BOOT && HAVE_EXPLICIT_BZERO
static inline void *explicit_bzero_safe(void *p, size_t l) {
        if (p && l > 0)
                explicit_bzero(p, l);

        return p;
}
#else
static inline void *explicit_bzero_safe(void *p, size_t l) {
        if (p && l > 0) {
                memset(p, 0, l);
                __asm__ __volatile__("" : : "r"(p) : "memory");
        }
        return p;
}
#endif

struct VarEraser {
        /* NB: This is a pointer to memory to erase in case of CLEANUP_ERASE(). Pointer to pointer to memory
         * to erase in case of CLEANUP_ERASE_PTR() */
        void *p;
        size_t size;
};

static inline void erase_var(struct VarEraser *e) {
        explicit_bzero_safe(e->p, e->size);
}

/* Mark var to be erased when leaving scope. */
#define CLEANUP_ERASE(var)                                              \
        _cleanup_(erase_var) _unused_ struct VarEraser CONCATENATE(_eraser_, UNIQ) = { \
                .p = &(var),                                            \
                .size = sizeof(var),                                    \
        }

static inline void erase_varp(struct VarEraser *e) {

        /* Very similar to erase_var(), but assumes `p` is a pointer to a pointer whose memory shall be destructed. */
        if (!e->p)
                return;

        explicit_bzero_safe(*(void**) e->p, e->size);
}

/* Mark pointer so that memory pointed to is erased when leaving scope. Note: this takes a pointer to the
 * specified pointer, instead of just a copy of it. This is to allow callers to invalidate the pointer after
 * use, if they like, disabling our automatic erasure (for example because they succeeded with whatever they
 * wanted to do and now intend to return the allocated buffer to their caller without it being erased). */
#define CLEANUP_ERASE_PTR(ptr, sz)                                      \
        _cleanup_(erase_varp) _unused_ struct VarEraser CONCATENATE(_eraser_, UNIQ) = { \
                .p = (ptr),                                             \
                .size = (sz),                                           \
        }
