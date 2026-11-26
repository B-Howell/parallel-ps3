#ifndef PAIR_LIST_H__
#define PAIR_LIST_H__

/* A short list of pairs, a 32-bit key and a pointer's room, with the keys
 * in an array of their own: the first two kept in the list itself, looked
 * at by selects; more on the heap as it grows, compared four at a time
 * (SSE2, NEON) into one mask, eight or sixteen a pass, so a lookup has no
 * branch on where the key is. Taken out, the last moves into the gap (order is not kept). The room
 * is the caller's, moved bitwise with its key, so a pointer-sized owner (a
 * std::unique_ptr) can live in it. Each 4 MB block of guest addresses has
 * one, holding the render targets that start in it
 * (Emu/RSX/Common/ranged_map.hpp), where each was a std::unordered_map.
 * One thread at a time (the RSX thread). C89.
 *
 * Plain data: zeroed, a list is empty; freed, it is empty again. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAIR_LIST_INLINE 2
#define PAIR_LIST_NONE   0xFFFFFFFFu

typedef union pair_list_room
{
   void         *align;               /* aligned as a pointer */
   unsigned char bytes[sizeof(void*)]; /* storage the caller makes its value in */
} pair_list_room_t;

typedef struct pair_list
{
   uint32_t         *keys;   /* on the heap past PAIR_LIST_INLINE, cap of them; NULL inline */
   pair_list_room_t *rooms;  /* in the same allocation, after the keys */
   uint32_t          count;
   uint32_t          cap;    /* on the heap, eight or a multiple of sixteen; 0 inline */
   uint32_t          inline_keys[PAIR_LIST_INLINE];
   pair_list_room_t  inline_rooms[PAIR_LIST_INLINE];
} pair_list_t;

/* The keys and the rooms, count of each, by the same index */
#define PAIR_LIST_KEYS(l)  ((l)->keys ? (l)->keys : (l)->inline_keys)
#define PAIR_LIST_ROOMS(l) ((l)->keys ? (l)->rooms : (l)->inline_rooms)

/* The index of @key, or PAIR_LIST_NONE */
uint32_t pair_list_find(const pair_list_t *l, uint32_t key);

/* The index of a new pair for @key (not looked for: the caller has found it
 * absent), its room zeroed; PAIR_LIST_NONE out of memory, nothing changed.
 * The pairs may have moved */
uint32_t pair_list_add(pair_list_t *l, uint32_t key);

/* The pair at @index (below count) taken out, the last moved into its
 * place */
void pair_list_remove_at(pair_list_t *l, uint32_t index);

/* Its storage freed, the list empty (what the rooms held is the caller's) */
void pair_list_free(pair_list_t *l);

#ifdef __cplusplus
}
#endif

#endif
