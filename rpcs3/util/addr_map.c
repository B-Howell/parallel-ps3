/* Ranges by their 32-bit start address, kept sorted (addr_map.h). C89
 *
 * The arrays keep room at both ends: a range added or taken out moves the
 * side of it with fewer entries, half as many on average as moving the
 * rest down. */

#include "addr_map.h"

#include <stdlib.h>
#include <string.h>

#define ADDR_MAP_MIN 8

/* The number of ranges starting at or below @addr. The halving search has
 * no branch on the comparison (a select), so a lookup does not depend on
 * guessing which half */
static uint32_t upper_of(const addr_map_t *m, uint32_t addr)
{
   const uint32_t *a    = m->addrs;
   uint32_t        n    = m->count;
   uint32_t        base = 0;

   if (!n)
      return 0;
   while (n > 1)
   {
      const uint32_t half = n / 2;
      base = a[base + half - 1] <= addr ? base + half : base;
      n   -= half;
   }
   return base + (a[base] <= addr);
}

uint32_t addr_map_floor(const addr_map_t *m, uint32_t addr)
{
   const uint32_t up = upper_of(m, addr);
   return up ? up - 1 : ADDR_MAP_NONE;
}

uint32_t addr_map_find(const addr_map_t *m, uint32_t addr)
{
   const uint32_t i = addr_map_floor(m, addr);
   return i != ADDR_MAP_NONE && m->addrs[i] == addr ? i : ADDR_MAP_NONE;
}

/* Entries [from, to) moved by @by (one either way) */
static void shift(addr_map_t *m, uint32_t from, uint32_t to, int by)
{
   if (from < to)
   {
      memmove(m->addrs + from + by, m->addrs + from, (to - from) * sizeof(*m->addrs));
      memmove(m->e + from + by, m->e + from, (to - from) * sizeof(*m->e));
   }
}

/* Twice the room, the entries in the middle with a gap at @at */
static int grow(addr_map_t *m, uint32_t at)
{
   const uint32_t    cap   = m->cap ? m->cap * 2 : ADDR_MAP_MIN;
   const uint32_t    front = (cap - m->count - 1) / 2;
   uint32_t         *addrs = (uint32_t*)malloc(cap * sizeof(*addrs));
   addr_map_entry_t *e     = (addr_map_entry_t*)malloc(cap * sizeof(*e));

   if (!addrs || !e)
   {
      free(addrs);
      free(e);
      return 0;
   }
   if (m->count)
   {
      memcpy(addrs + front, m->addrs, at * sizeof(*addrs));
      memcpy(addrs + front + at + 1, m->addrs + at, (m->count - at) * sizeof(*addrs));
      memcpy(e + front, m->e, at * sizeof(*e));
      memcpy(e + front + at + 1, m->e + at, (m->count - at) * sizeof(*e));
   }
   if (m->cap)
   {
      free(m->addrs - m->front);
      free(m->e - m->front);
   }
   m->addrs = addrs + front;
   m->e     = e + front;
   m->front = front;
   m->cap   = cap;
   return 1;
}

uint32_t addr_map_insert(addr_map_t *m, uint32_t addr, uint32_t size)
{
   const uint32_t up   = upper_of(m, addr);
   const uint32_t back = m->cap - m->front - m->count;

   if (up && m->addrs[up - 1] == addr)
      return ADDR_MAP_NONE;

   if (m->front && (up < m->count - up || !back))
   {
      /* Those below move down into the room before */
      m->addrs--;
      m->e--;
      m->front--;
      shift(m, 1, up + 1, -1);
   }
   else if (back)
      shift(m, up, m->count, 1);
   else if (!grow(m, up))
      return ADDR_MAP_NONE;

   m->addrs[up]     = addr;
   m->e[up].size    = size;
   m->e[up].data[0] = NULL;
   m->e[up].data[1] = NULL;
   m->count++;
   return up;
}

void addr_map_erase(addr_map_t *m, uint32_t index)
{
   if (index < m->count - index - 1)
   {
      /* Those below move up over it */
      shift(m, 0, index, 1);
      m->addrs++;
      m->e++;
      m->front++;
   }
   else
      shift(m, index + 1, m->count, -1);
   m->count--;
}

void addr_map_free(addr_map_t *m)
{
   if (m->cap)
   {
      free(m->addrs - m->front);
      free(m->e - m->front);
   }
   m->addrs = NULL;
   m->e     = NULL;
   m->count = 0;
   m->cap   = 0;
   m->front = 0;
}
