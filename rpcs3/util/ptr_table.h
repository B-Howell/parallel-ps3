#ifndef PTR_TABLE_H__
#define PTR_TABLE_H__

/* Pointers by a 64-bit key in an open-addressed table: linear probing, a
 * slot empty while its pointer is NULL (so NULL is never kept), taken out
 * by shifting the entries after back. It grows to keep it at most half
 * full. For more entries than a key_list is quick for, as the images an
 * overlay draws (a save list's icons, VKOverlays.h, GLOverlays.h). The
 * owner's thread alone. C89.
 *
 * Plain data: zeroed, a table is empty; freed, it is empty again. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ptr_table
{
   uint64_t *keys;
   void    **vals;
   uint32_t  cap;    /* a power of two, or 0 before the first add */
   uint32_t  count;
   uint32_t  shift;  /* 64 less log2(cap): a key's hash moved down to a slot */
} ptr_table_t;

void *ptr_table_find(const ptr_table_t *t, uint64_t key);

/* 1: added (the key not looked for: the caller has found it absent); 0:
 * out of memory, or @val NULL, nothing changed */
int ptr_table_add(ptr_table_t *t, uint64_t key, void *val);

/* @key's entry taken out: its pointer, or NULL where it is not there */
void *ptr_table_remove(ptr_table_t *t, uint64_t key);

/* The pointer in slot @index (below cap), or NULL for an empty slot. To go
 * through and take out: ptr_table_remove_at the slot, then look at the same
 * slot again, as the entries after shift back into it */
void *ptr_table_slot(const ptr_table_t *t, uint32_t index);
void *ptr_table_remove_at(ptr_table_t *t, uint32_t index);

/* Its storage freed, the table empty (what it pointed to is the caller's) */
void ptr_table_free(ptr_table_t *t);

#ifdef __cplusplus
}
#endif

#endif
