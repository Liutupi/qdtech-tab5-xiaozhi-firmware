#pragma once
#include <cstddef>
#include <cstdlib>

#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
inline void* heap_caps_malloc(size_t size, unsigned) { return std::malloc(size); }
inline void heap_caps_free(void* p) { std::free(p); }
inline size_t heap_caps_get_free_size(unsigned) { return 0; }
