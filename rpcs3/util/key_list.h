#ifndef KEY_LIST_H__
#define KEY_LIST_H__

/* A short list of pointers by a 64-bit key, searched in turn: the first
 * four kept in the list itself, more on the heap as it grows. For a few
 * entries a look through them is quicker than a hash, and a list of four
 * or fewer allocates nothing (an image's views, vkutils/image.h). C89.
 *
 * Plain data: a list is moved by key_list_move, and once moved from, or
 * freed, it is empty. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEY_LIST_INLINE 4

typedef struct key_list
{
   uint32_t count;
   uint32_t cap;      /* on the heap past KEY_LIST_INLINE; 0 inline */
   uint64_t *keys;
   void    **vals;
   uint64_t ikeys[KEY_LIST_INLINE];
   void    *ivals[KEY_LIST_INLINE];
} key_list_t;

void *key_list_find(const key_list_t *l, uint64_t key);

/* 1: added (the key not looked for: the caller has found it absent); 0:
 * out of memory, nothing changed */
int key_list_add(key_list_t *l, uint64_t key, void *val);

/* @key's pointer made @val: the one it had, or NULL (nothing added) where
 * the key is not there */
void *key_list_replace(key_list_t *l, uint64_t key, void *val);

void *key_list_at(const key_list_t *l, uint32_t index);

/* Its storage freed, the list empty (what it pointed to is the caller's) */
void key_list_free(key_list_t *l);

/* @src's entries to @dst, which is overwritten; @src left empty */
void key_list_move(key_list_t *dst, key_list_t *src);

#ifdef __cplusplus
}
#endif

#endif
