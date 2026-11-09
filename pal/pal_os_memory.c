/*
 * pal_os_memory.c
 *
 * Pico port of the Linux PAL pal_os_memory.c (the NEW_PAL_TEMPLATE does
 * not ship one). The Pico SDK's newlib-nano provides the standard
 * allocator; the heap is carved from RAM at startup (see PICO_HEAP_SIZE).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pal_os_memory.h"

void *pal_os_malloc(uint32_t block_size) {
    return (malloc(block_size));
}

void *pal_os_calloc(uint32_t number_of_blocks, uint32_t block_size) {
    return (calloc(number_of_blocks, block_size));
}

void pal_os_free(void *p_block) {
    free(p_block);
}

void pal_os_memcpy(void *p_destination, const void *p_source, uint32_t size) {
    memcpy(p_destination, p_source, size);
}

void pal_os_memset(void *p_buffer, uint32_t value, uint32_t size) {
    memset(p_buffer, (int)value, size);
}
