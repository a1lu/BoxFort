/*
 * The MIT License (MIT)
 *
 * Copyright © 2016 Franklin "Snaipe" Mathieu <http://snai.pe/>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "boxfort.h"
#include "common.h"

#ifndef _WIN32
# include <fcntl.h>
# include <sys/mman.h>
# include <sys/stat.h>
# include <unistd.h>

# include "timestamp.h"
#endif

#define PAGE_SIZE 4096
#define GROWTH_RATIO (1.61)
#define MAP_RETRIES 3

#if BXF_BITS == 32
# define SPTR 4
static void *mmap_max = (void *)0xf0000000;
#elif BXF_BITS == 64
# define SPTR 6

/* On Linux it seems that you cannot map > 48-bit addresses */
static void *mmap_max = (void *)0x7f0000000000;
#else
# error Platform not supported
#endif

static unsigned int mmap_seed;
static void *mmap_base = (void*) ((uintptr_t)1 << (SPTR * 8 - 3));
static intptr_t mmap_off = ((intptr_t)1 << ((SPTR / 2) * 8));

static inline void *ptr_add(void *ptr, size_t off)
{
    return (char *) ptr + off;
}

#define chunk_next(Chunk) \
    ((struct bxfi_arena_chunk *)((Chunk)->next ? ptr_add(*arena, (Chunk)->next) : NULL))

#define get_free_chunks(arena) (ptr_add(*arena, (*arena)->free_chunks))

int bxf_arena_init(size_t initial, int flags, bxf_arena *arena)
{
    initial = align2_up(initial, PAGE_SIZE);
    if (!initial)
        initial = 32 * PAGE_SIZE;

#ifdef _WIN32
    SECURITY_ATTRIBUTES inherit = {
        .nLength = sizeof (SECURITY_ATTRIBUTES),
        .bInheritHandle = TRUE,
    };

    /* Reserve whole address space (minus kernel-reserved) for possible max
     * heap size */
    LARGE_INTEGER sz = { .QuadPart = 1llu << (sizeof (void *) * 8 - 2) };

    HANDLE hndl = CreateFileMapping(INVALID_HANDLE_VALUE, &inherit,
            PAGE_READWRITE | SEC_RESERVE, sz.HighPart, sz.LowPart, NULL);

    if (!hndl)
        return -EINVAL;

    struct bxf_arena *a = MapViewOfFile(hndl, FILE_MAP_WRITE, 0, 0, initial);

    if (!a)
        goto error;

    if (!VirtualAlloc(a, initial, MEM_COMMIT, PAGE_READWRITE))
        goto error;

#else
    char name[sizeof ("/bxf_arena_") + 21];
    snprintf(name, sizeof (name), "/bxf_arena_%d", getpid());

    int fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);

    if (fd == -1)
        goto error;

    shm_unlink(name);

    if (ftruncate(fd, initial) == -1)
        goto error;

    if (!mmap_seed)
        mmap_seed = bxfi_timestamp_monotonic();

    intptr_t r;
    struct bxf_arena *a;
    int tries = 0;

    for (tries = 0; tries < MAP_RETRIES;) {
        r = rand_r(&mmap_seed);

        void *base = ptr_add(mmap_base, r * mmap_off);
        if (base > mmap_max || base < mmap_base)
            continue;

        for (void *addr = base; addr < ptr_add(base, initial);
                addr = ptr_add(addr, PAGE_SIZE)) {
            int rc = msync(addr, PAGE_SIZE, 0);
            if (rc != -1 || errno != ENOMEM)
                goto retry;
        }

        a = mmap(base, initial, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED, fd, 0);

        if (a == MAP_FAILED)
            goto error;

        if ((void *)a < mmap_max && (void *)a > mmap_base)
            break;
        munmap(a, initial);
        ++tries;
retry:;
    }
    if (tries == MAP_RETRIES)
        goto error;

#endif

    a->flags = flags;
    a->size = initial;
    a->addr = a;
    a->free_chunks = sizeof (*a);

#ifdef _WIN32
    a->handle = hndl;
#else
    a->handle = fd;
#endif

    struct bxfi_arena_chunk *first = ptr_add(a, a->free_chunks);
    *first = (struct bxfi_arena_chunk) {
        .size = initial - sizeof (*a),
    };

    *arena = a;
    return 0;

error:;
#ifdef _WIN32
    CloseHandle(hndl);
    return -ENOMEM;
#else
    int errnum = errno;
    if (fd != -1)
        close(fd);
    return -errnum;
#endif
}

int bxfi_arena_inherit(bxf_fhandle hndl, int flags, bxf_arena *arena)
{
    void *base = NULL;
    if (flags & BXF_ARENA_IDENTITY)
        base = *arena;

#ifdef _WIN32
    DWORD prot;
    if (flags & BXF_ARENA_IMMUTABLE)
        prot = FILE_MAP_READ;
    else
        prot = FILE_MAP_COPY;

    struct bxf_arena *a = MapViewOfFile(hndl, prot, 0, 0, sizeof (*a));

    if (!a)
        return -ENOMEM;

    size_t size = a->size;
    UnmapViewOfFile(a);

    a = MapViewOfFileEx(hndl, prot, 0, 0, size, base);
    if (!a)
        return -ENOMEM;
#else
    int mmapfl = MAP_PRIVATE;
    if (flags & BXF_ARENA_IDENTITY)
        mmapfl |= MAP_FIXED;

    int prot = PROT_READ;
    if (!(flags & BXF_ARENA_IMMUTABLE))
        prot |= PROT_WRITE;

    struct bxf_arena *a = mmap(NULL, sizeof (*a), prot, MAP_PRIVATE, hndl, 0);

    if (a == MAP_FAILED)
        return -errno;

    size_t size = a->size;
    munmap(a, sizeof (*a));

    a = mmap(base, size, prot, mmapfl, hndl, 0);

    if (a == MAP_FAILED)
        return -errno;
#endif
    *arena = a;
    return 0;
}


int bxf_arena_copy(bxf_arena orig, int flags, bxf_arena *arena)
{
    int rc = bxf_arena_init(orig->size, flags, arena);
    if (rc > 0) {
        memcpy(*arena + 1, orig + 1, orig->size - sizeof (orig));
        (*arena)->free_chunks = orig->free_chunks;
    }
    return rc;
}

int bxf_arena_term(bxf_arena *arena)
{
#ifdef _WIN32
    CloseHandle((*arena)->handle);
    UnmapViewOfFile(*arena);
#else
    close((*arena)->handle);
    munmap(*arena, (*arena)->size);
#endif
    *arena = NULL;
    return 0;
}

static inline int arena_valid(bxf_arena arena)
{
    return arena && arena->addr == arena;
}

static int arena_resize(bxf_arena *arena, size_t newsize)
{
#ifdef _WIN32
    size_t size = (*arena)->size;

    void *base = ptr_add(*arena, (*arena)->size);
    LARGE_INTEGER off = { .QuadPart = (*arena)->size };

    void *addr = MapViewOfFileEx((*arena)->handle, FILE_MAP_WRITE, 
            off.HighPart, off.LowPart, newsize - size, base);

    if (addr != base) {
        if (!((*arena)->flags & BXF_ARENA_MAYMOVE))
            return -ENOMEM;

        FlushViewOfFile(*arena, size);
        struct bxf_arena *a = MapViewOfFile((*arena)->handle, FILE_MAP_WRITE,
                0, 0, newsize);

        if (!a)
            return -ENOMEM;

        addr = ptr_add(a, size);

        a->addr = a;
        UnmapViewOfFile(*arena);
        *arena = a;
    }

    if (!VirtualAlloc(addr, newsize - size, MEM_COMMIT, PAGE_READWRITE))
        return -ENOMEM;
    return 0;
#else
    if (ftruncate((*arena)->handle, newsize) < 0)
        return -ENOMEM;

# if defined (HAVE_MREMAP)
    int flags = (*arena)->flags & BXF_ARENA_MAYMOVE ? MREMAP_MAYMOVE : 0;

    struct bxf_arena *a = mremap(*arena, (*arena)->size, newsize, flags);
    if (a == MAP_FAILED)
        return -errno;

    a->addr = a;
    *arena = a;
# else
    size_t remsz = newsize - (*arena)->size;
    char *addr_hi = ptr_add(*arena, (*arena)->size);
    int move = 0;
    for (char *addr = addr_hi; remsz; remsz -= PAGE_SIZE, addr += PAGE_SIZE) {
        int rc = msync(addr, PAGE_SIZE, MS_ASYNC);
        if (rc != -1 || errno != ENOMEM) {
            move = 1;
            break;
        }
    }

    if (move) {
        if (!((*arena)->flags & BXF_ARENA_MAYMOVE))
            return -ENOMEM;

        msync(*arena, (*arena)->size, MS_SYNC);
        struct bxf_arena *a = mmap(*arena, newsize, PROT_READ | PROT_WRITE,
                MAP_SHARED, (*arena)->handle, 0);
        if (a == MAP_FAILED)
            return -ENOMEM;

        a->addr = a;
        munmap(*arena, (*arena)->size);
        *arena = a;
    } else {
        size_t remsz = newsize - (*arena)->size;
        void *raddr = mmap(addr_hi, remsz, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED, (*arena)->handle, (*arena)->size);
        if (raddr == MAP_FAILED)
            return -errno;
    }
# endif

    (*arena)->size = newsize;
    return 0;
#endif
    return -ENOMEM;
}

bxf_ptr bxf_arena_alloc(bxf_arena *arena, size_t size)
{
    if (!arena_valid(*arena))
        return -EINVAL;

    size = align2_up(size + sizeof (struct bxfi_arena_chunk), sizeof (void *));

    intptr_t *nptr = &(*arena)->free_chunks;
    intptr_t *nptr_best = NULL;

    struct bxfi_arena_chunk *best = NULL;
    struct bxfi_arena_chunk *c;
    for (c = get_free_chunks(arena); c; c = chunk_next(c)) {
        if ((c->size >= size && (!best || best->size > c->size))
                || (!c->next && !best)) {
            best = c;
            nptr_best = nptr;
        }
        if (c->size == size)
            break;
        nptr = &c->next;
    }

    if (best->size < size) {
        if (!((*arena)->flags & BXF_ARENA_RESIZE))
            return -ENOMEM;

        size_t oksize = (*arena)->size + size - best->size + sizeof (*best);
        size_t newsize = (*arena)->size;

        while (newsize < oksize)
            newsize *= GROWTH_RATIO;

        newsize = align2_up(newsize, PAGE_SIZE);
        int rc = arena_resize(arena, newsize);
        if (rc < 0)
            return rc;
    }

    size_t remsz = best->size - size;
    best->size = size;

    struct bxfi_arena_chunk *next = ptr_add(best, best->size);
    *next = (struct bxfi_arena_chunk) {
        .size = remsz,
        .next = best->next,
    };

    *nptr_best = (intptr_t) next - (intptr_t)*arena;

    best->addr = (intptr_t)(best + 1) - (intptr_t)*arena;
    return best->addr;
}

bxf_ptr bxf_arena_realloc(bxf_arena *arena, bxf_ptr ptr, size_t size)
{
    if (!arena_valid(*arena))
        return -EINVAL;

    if (!ptr)
        return bxf_arena_alloc(arena, size);

    void *p = ptr_add(*arena, ptr);

    if (p <= ptr_add(*arena, sizeof (struct bxfi_arena_chunk))
            || p >= ptr_add(*arena, (*arena)->size))
        return -EFAULT;

    struct bxfi_arena_chunk *chunk = p;
    --chunk;

    if (ptr_add(*arena, chunk->addr) != p)
        return -EFAULT;

    int rc = bxf_arena_grow(arena, ptr, size);
    if (rc == -ENOMEM) {
        /* If we can't call free, give up and let the user call alloc */
        if (!((*arena)->flags & BXF_ARENA_DYNAMIC))
            return rc;

        rc = bxf_arena_alloc(arena, size);
        if (rc > 0) {
            void *np = ptr_add(*arena, rc);
            memcpy(np, p, chunk->size);
            bxf_arena_free(arena, ptr);
        }
    }
    return rc;
}

int bxf_arena_grow(bxf_arena *arena, bxf_ptr p, size_t size)
{
    if (!arena_valid(*arena))
        return -EINVAL;

    void *ptr = ptr_add(*arena, p);

    size = align2_up(size + sizeof (struct bxfi_arena_chunk), sizeof (void *));

    if (!ptr || ptr <= ptr_add(*arena, sizeof (struct bxfi_arena_chunk))
             || ptr >= ptr_add(*arena, (*arena)->size))
        return -EFAULT;

    struct bxfi_arena_chunk *chunk = ptr;
    --chunk;

    if (ptr_add(*arena, chunk->addr) != ptr)
        return -EFAULT;

    struct bxfi_arena_chunk *next = ptr_add(chunk, chunk->size);

    if (next->addr)
        return -ENOMEM;

    if (ptr_add(next, size) > ptr_add(*arena, (*arena)->size)) {
        if (!((*arena)->flags & BXF_ARENA_RESIZE))
            return -ENOMEM;

        size_t oksize = (*arena)->size + size - next->size + sizeof (*next);
        size_t newsize = (*arena)->size;

        while (newsize < oksize)
            newsize *= GROWTH_RATIO;

        size_t oldsize = (*arena)->size;

        intptr_t off = (intptr_t)*arena;

        newsize = align2_up(newsize, PAGE_SIZE);
        int rc = arena_resize(arena, newsize);
        if (rc < 0)
            return rc;

        off = (intptr_t)*arena - off;

        ptr = ptr_add(ptr, off);
        chunk = ptr_add(chunk, off);
        next = ptr_add(next, off);

        next->size += newsize - oldsize;
    }

    if (next->size < size - chunk->size)
        return -ENOMEM;

    /* Remove the next chunk from the free list */
    intptr_t *nptr = &(*arena)->free_chunks;
    for (struct bxfi_arena_chunk *c = get_free_chunks(arena);
            c; c = chunk_next(c)) {
        if (c == next)
            break;
        nptr = &c->next;
    }
    *nptr = next->next;

    chunk->size += next->size;

    /* If there is enough space to fit another chunk at the end of the newly
     * merged block, do so */
    size_t asize = align2_down(chunk->size, sizeof (void *));
    if (chunk->size - asize >= align2_up(sizeof (*chunk) + 1, sizeof (void *))
            && asize >= size) {
        struct bxfi_arena_chunk *next = ptr_add(chunk, size);
        *next = (struct bxfi_arena_chunk) {
            .next = *nptr,
        };
        *nptr = (intptr_t)next - (intptr_t)*arena;
    }
    return 0;
}

int bxf_arena_free(bxf_arena *arena, bxf_ptr p)
{
    if (!arena_valid(*arena))
        return -EINVAL;

    if (!((*arena)->flags & BXF_ARENA_DYNAMIC))
        return -ENOTSUP;

    if (!p)
        return 0;

    void *ptr = ptr_add(*arena, p);

    if (ptr <= ptr_add(*arena, sizeof (struct bxfi_arena_chunk))
            || ptr >= ptr_add(*arena, (*arena)->size))
        return -EFAULT;

    struct bxfi_arena_chunk *chunk = ptr;
    --chunk;

    if (ptr_add(*arena, chunk->addr) != ptr)
        return -EFAULT;

    intptr_t *nptr = &(*arena)->free_chunks;
    struct bxfi_arena_chunk *prev = NULL;
    for (struct bxfi_arena_chunk *c = get_free_chunks(arena);
            c; c = chunk_next(c)) {
        if (c > chunk)
            break;
        nptr = &c->next;
        prev = c;
    }

    chunk->next = *nptr;
    *nptr = (intptr_t)chunk -(intptr_t)*arena;

    if (prev) {
        prev->size += chunk->size;
        prev->next = chunk->next;
        chunk = prev;
    }
    if (chunk->next) {
        chunk->size += chunk_next(chunk)->size;
        chunk->next = chunk->next;
    }
    chunk->addr = 0;
    return 0;
}

int bxf_arena_iter(bxf_arena arena, bxf_arena_fn *fn, void *user)
{
    struct bxfi_arena_chunk *c = (void *)(arena + 1);
    for (; (void*)c < ptr_add(arena, arena->size);
            c = ptr_add(c, c->size)) {
        if (c->addr) {
            int rc = fn(ptr_add(arena, c->addr), c->size - sizeof (*c), user);
            if (rc)
                return rc;
        }
    }
    return 0;
}

void *bxf_arena_ptr(bxf_arena arena, bxf_ptr ptr)
{
    return ptr_add(arena, ptr);
}
