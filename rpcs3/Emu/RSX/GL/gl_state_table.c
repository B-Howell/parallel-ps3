/* What the OpenGL renderer last set of the driver's state (gl_state_table.h). C89 */

#include "gl_state_table.h"

static unsigned slot_of(uint32_t key, unsigned bits)
{
   return (unsigned)((key * 2654435761u) >> (32 - bits));
}

/* The key's slot, or where it would go; -1 where the table is full */
static int find(const uint32_t *keys, unsigned size, unsigned bits, uint32_t key)
{
   const uint32_t k = key + 1;
   unsigned       i = slot_of(key, bits), n;

   for (n = 0; n < size; n++, i = (i + 1) & (size - 1))
      if (keys[i] == k || keys[i] == 0)
         return (int)i;
   return -1;
}

int gl_state_test_and_set(gl_state_table_t *t, uint32_t key, uint64_t v)
{
   const int i = find(t->key, GL_STATE_SLOTS, 7, key);

   if (i < 0)
      return 0;
   if (t->key[i] && t->val[i] == v)
      return 1;
   t->key[i] = key + 1;
   t->val[i] = v;
   return 0;
}

int gl_state_test_and_set_indexed(gl_state_table_t *t, uint32_t key, unsigned index, uint64_t v)
{
   const int i = find(t->ikey, GL_STATE_INDEXED, 5, key);

   if (i < 0 || index > 3)
      return 0;
   if (!t->ikey[i])
   {
      t->ikey[i]    = key + 1;
      t->ival[i][0] = t->ival[i][1] = t->ival[i][2] = t->ival[i][3] = 0;
   }
   else if (t->ival[i][index] == v)
      return 1;
   t->ival[i][index] = v;
   return 0;
}

uint32_t gl_state_bound_texture(const gl_state_table_t *t, unsigned unit, uint32_t target)
{
   unsigned j;

   if (unit >= GL_STATE_UNITS)
      return 0;
   for (j = 0; j < GL_STATE_UNIT_TARGETS && t->target[unit][j]; j++)
      if (t->target[unit][j] == target + 1)
         return t->name[unit][j];
   return 0;
}

int gl_state_bind_texture(gl_state_table_t *t, unsigned unit, uint32_t target, uint32_t name)
{
   unsigned j;

   if (unit >= GL_STATE_UNITS)
      return 0;
   for (j = 0; j < GL_STATE_UNIT_TARGETS; j++)
   {
      if (!t->target[unit][j])
      {
         /* First seen: none bound before, as the map's default */
         t->target[unit][j] = target + 1;
         t->name[unit][j]   = name;
         return name == 0;
      }
      if (t->target[unit][j] == target + 1)
      {
         if (t->name[unit][j] == name)
            return 1;
         t->name[unit][j] = name;
         return 0;
      }
   }
   return 0;
}

void gl_state_unbind_texture(gl_state_table_t *t, uint32_t target, uint32_t name)
{
   unsigned u, j;

   for (u = 0; u < GL_STATE_UNITS; u++)
      for (j = 0; j < GL_STATE_UNIT_TARGETS && t->target[u][j]; j++)
         if (t->target[u][j] == target + 1 && t->name[u][j] == name)
         {
            t->name[u][j] = 0;
            return;
         }
}
