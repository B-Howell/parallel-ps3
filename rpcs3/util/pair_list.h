#ifndef PAIR_LIST_H__
#define PAIR_LIST_H__

/* A short list of pairs, a 32-bit key and a pointer's room, looked along
 * by selects with no branch on where the key is: the first two kept in the
 * list itself, more on the heap as it grows. Taken out, the last moves into
 * the gap (order is not kept). The room is the caller's, moved bitwise with
 * the pair, so a pointer-sized owner (a std::unique_ptr) can live in it.
 * Each 4 MB block of guest addresses has one, holding the render targets
 * that start in it (Emu/RSX/Common/ranged_map.hpp), where each was a
 * std::unordered_map: the one to three a block mostly holds are found
 * faster this way than by a hash and its modulo. One thread at a time (the
 * RSX thread). C89.
 *
 * Plain data: zeroed, a list is empty; freed, it is empty again. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAIR_LIST_INLINE 2

typedef struct pair_list_entry
{
   uint32_t key;
   union
   {
      void         *align;               /* aligned as a pointer */
      unsigned char bytes[sizeof(void*)]; /* storage the caller makes its value in */
   } room;                                /* the caller's, zeroed when added */
} pair_list_entry_t;

typedef struct pair_list
{
   pair_list_entry_t *heap;  /* past PAIR_LIST_INLINE; NULL inline */
   uint32_t           count;
   uint32_t           cap;   /* on the heap; 0 inline */
   pair_list_entry_t  inline_e[PAIR_LIST_INLINE];
} pair_list_t;

/* The entries, count of them */
#define PAIR_LIST_ENTRIES(l) ((l)->heap ? (l)->heap : (l)->inline_e)

/* The entry with @key, or NULL */
pair_list_entry_t *pair_list_find(pair_list_t *l, uint32_t key);

/* A new entry for @key (not looked for: the caller has found it absent),
 * its room zeroed; NULL out of memory, nothing changed. Entries before it
 * may have moved */
pair_list_entry_t *pair_list_add(pair_list_t *l, uint32_t key);

/* The entry at @index (below count) taken out, the last moved into its
 * place */
void pair_list_remove_at(pair_list_t *l, uint32_t index);

/* Its storage freed, the list empty (what the rooms held is the caller's) */
void pair_list_free(pair_list_t *l);

#ifdef __cplusplus
}
#endif

#endif
