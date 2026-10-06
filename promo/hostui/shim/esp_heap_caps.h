#pragma once
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM (1<<10)
#define MALLOC_CAP_8BIT (1<<2)
#define MALLOC_CAP_INTERNAL (1<<11)
#define MALLOC_CAP_DMA (1<<3)
#define MALLOC_CAP_DEFAULT (1<<12)
static inline void* heap_caps_malloc(size_t n, uint32_t) { return malloc(n); }
static inline void* heap_caps_calloc(size_t a, size_t b, uint32_t) { return calloc(a, b); }
static inline void* heap_caps_realloc(void* p, size_t n, uint32_t) { return realloc(p, n); }
static inline void* heap_caps_aligned_alloc(size_t al, size_t n, uint32_t) { return aligned_alloc(al, (n + al - 1) / al * al); }
static inline void heap_caps_free(void* p) { free(p); }
static inline size_t heap_caps_get_free_size(uint32_t) { return 32u * 1024 * 1024; }
static inline size_t heap_caps_get_largest_free_block(uint32_t) { return 16u * 1024 * 1024; }
static inline size_t heap_caps_get_minimum_free_size(uint32_t) { return 8u * 1024 * 1024; }
