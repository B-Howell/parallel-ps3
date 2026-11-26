#ifndef ADDR_MAP_H__
#define ADDR_MAP_H__

/* Ranges by their 32-bit start address, kept sorted: the addresses in one
 * array of their own, searched without branches, exactly or for the last
 * starting at or below an address, and gone through in address order. Each
 * holds its size and two pointers' room for the caller, moved bitwise as
 * entries move. It was an std::map, a node allocated for each range (a guest
 * memory block's allocations and their shm handles, Emu/Memory/vm.cpp); this
 * allocates only as it grows. Whoever changes it alone at a time
 * (vm::writer_lock). C89.
 *
 * Plain data: zeroed, a map is empty; freed, it is empty again. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADDR_MAP_NONE 0xFFFFFFFFu

typedef struct addr_map_entry
{
   uint32_t size;
   void    *data[2];  /* the caller's, zeroed when added */
} addr_map_entry_t;

typedef struct addr_map
{
   uint32_t         *addrs;  /* count of them, rising */
   addr_map_entry_t *e;      /* each address's range */
   uint32_t          count;
   uint32_t          cap;
   uint32_t          front;  /* room kept before the first, so the shorter side moves */
} addr_map_t;

/* The index of the range starting at @addr, or ADDR_MAP_NONE */
uint32_t addr_map_find(const addr_map_t *m, uint32_t addr);

/* The index of the last range starting at or below @addr, or ADDR_MAP_NONE */
uint32_t addr_map_floor(const addr_map_t *m, uint32_t addr);

/* The index of a range added in order, its data zeroed; ADDR_MAP_NONE where
 * @addr is already there, or out of memory, nothing changed. The indices
 * from it on now name the next range up */
uint32_t addr_map_insert(addr_map_t *m, uint32_t addr, uint32_t size);

/* The range at @index (below count) taken out; those after move down one
 * index */
void addr_map_erase(addr_map_t *m, uint32_t index);

/* Its storage freed, the map empty (what it pointed to is the caller's) */
void addr_map_free(addr_map_t *m);

#ifdef __cplusplus
}
#endif

#endif
