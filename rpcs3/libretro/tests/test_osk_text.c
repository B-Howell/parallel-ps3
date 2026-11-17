/* The on-screen keyboard's text (osk_text.c): set, read and cleared, and
 * under ThreadSanitizer two threads setting texts 50000 times each while
 * two read them - every text read is one whole text set, never half of
 * one and half of another, and the pool never runs dry. C89. */

#include "../../Emu/Cell/Modules/osk_text.h"
#include "test_common.h"

#include <rthreads/rthreads.h>

#define SETS 50000

static osk_text_t text;
static retro_atomic_int_t bad, writers_left;

/* A text of @len characters, all @c */
static void fill(uint16_t *buf, uint16_t c, unsigned len)
{
   unsigned i;
   for (i = 0; i < len; i++)
      buf[i] = c;
   buf[len] = 0;
}

/* Whether @s is one text fill() made: one character, then 0 to the end */
static int whole(const uint16_t *s)
{
   unsigned i = 0;
   const uint16_t c = s[0];
   while (i < OSK_TEXT_CHARS && s[i] == c && c)
      i++;
   for (; i < OSK_TEXT_CHARS; i++)
      if (s[i])
         return 0;
   return 1;
}

static void writer(void *data)
{
   uint16_t buf[OSK_TEXT_CHARS];
   const uint16_t base = (uint16_t)(uintptr_t)data;
   unsigned n;
   for (n = 0; n < SETS; n++)
   {
      fill(buf, (uint16_t)(base + (n & 7)), 1 + (n * 37) % (OSK_TEXT_CHARS - 2));
      osk_text_set(&text, buf, OSK_TEXT_CHARS);
   }
   retro_atomic_fetch_add_int(&writers_left, -1);
}

static void reader(void *data)
{
   (void)data;
   while (retro_atomic_load_acquire_int(&writers_left))
   {
      const uint16_t *s = osk_text_enter(&text);
      if (!whole(s))
         retro_atomic_inc_int(&bad);
      osk_text_leave(&text);
   }
}

int main(void)
{
   uint16_t buf[OSK_TEXT_CHARS];
   const uint16_t *s;
   sthread_t *t[4];
   int i;

   osk_text_init(&text);

   /* Empty at first */
   s = osk_text_enter(&text);
   TEST_CHECK(s[0] == 0 && s[OSK_TEXT_CHARS - 1] == 0);
   osk_text_leave(&text);

   /* Set, cut at its 0 and at @n */
   fill(buf, 'a', 3);
   osk_text_set(&text, buf, OSK_TEXT_CHARS);
   s = osk_text_enter(&text);
   TEST_CHECK(s[0] == 'a' && s[2] == 'a' && s[3] == 0);
   osk_text_leave(&text);
   fill(buf, 'b', 10);
   osk_text_set(&text, buf, 4);
   s = osk_text_enter(&text);
   TEST_CHECK(s[3] == 'b' && s[4] == 0);
   osk_text_leave(&text);

   /* Read while it is set again: the text read stays as it was until the
    * leave */
   s = osk_text_enter(&text);
   fill(buf, 'c', 5);
   osk_text_set(&text, buf, OSK_TEXT_CHARS);
   TEST_CHECK(s[0] == 'b' && s[4] == 0);
   osk_text_leave(&text);

   /* Cleared */
   osk_text_set(&text, NULL, 0);
   s = osk_text_enter(&text);
   TEST_CHECK(s[0] == 0);
   osk_text_leave(&text);

   /* Many more sets than nodes: each comes back to the pool as no reader
    * is in */
   for (i = 0; i < 100; i++)
   {
      fill(buf, 'd', 1 + i);
      osk_text_set(&text, buf, OSK_TEXT_CHARS);
   }
   s = osk_text_enter(&text);
   TEST_CHECK(s[0] == 'd' && s[99] == 'd' && s[100] == 0);
   osk_text_leave(&text);

   /* Two setting, two reading */
   retro_atomic_int_init(&bad, 0);
   retro_atomic_int_init(&writers_left, 2);
   t[0] = sthread_create(writer, (void*)(uintptr_t)'A');
   t[1] = sthread_create(writer, (void*)(uintptr_t)'Q');
   t[2] = sthread_create(reader, NULL);
   t[3] = sthread_create(reader, NULL);
   for (i = 0; i < 4; i++)
      sthread_join(t[i]);
   TEST_CHECK(retro_atomic_load_acquire_int(&bad) == 0);

   if (test_failures)
      return 1;
   printf("test_osk_text: whole texts only, %d sets read by two threads\n", 2 * SETS);
   return 0;
}
