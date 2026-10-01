#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/libs/lz4/lz4.h"

// Host allocator bindings for the bundled LVGL codec.
void* lv_malloc(size_t size) { return malloc(size); }
void lv_free(void* pointer) { free(pointer); }
void* lv_calloc(size_t count, size_t size) { return calloc(count, size); }
void* lv_memcpy(void* dst, const void* src, size_t length) { return memcpy(dst, src, length); }
void* lv_memmove(void* dst, const void* src, size_t length) { return memmove(dst, src, length); }
void lv_memset(void* dst, uint8_t value, size_t length) { memset(dst, value, length); }

int main(int argc, char** argv) {
    if (argc != 4)
        return 1;
    FILE* input = fopen(argv[1], "rb");
    if (!input)
        return 2;
    fseek(input, 0, SEEK_END);
    long length = ftell(input);
    rewind(input);
    char* compressed = malloc(length);
    if (!compressed || fread(compressed, 1, length, input) != (size_t)length)
        return 3;
    fclose(input);
    int size = atoi(argv[3]);
    char* decoded = malloc(size ? size : 1);
    int result = LZ4_decompress_safe(compressed, decoded, length, size);
    if (result != size)
        return 4;
    FILE* output = fopen(argv[2], "wb");
    if (!output || fwrite(decoded, 1, size, output) != (size_t)size)
        return 5;
    fclose(output);
    free(compressed);
    free(decoded);
    return 0;
}
