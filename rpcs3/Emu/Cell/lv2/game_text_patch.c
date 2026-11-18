/* Game text patches (game_text_patch.h).
 *
 * The edits are set by the Patch Manager while no game runs, then read by
 * the PPU loader as the game's executable is loaded: the entries are
 * written first and the count published after them. Nothing is allocated:
 * the text is changed where it is in the game's memory and keeps its
 * length, so a length the game passes with it (Demon's Souls hands its
 * sound setup to a memory stream with its size) still holds. */

#include "game_text_patch.h"

#include <stdio.h>
#include <string.h>
#include <retro_atomic.h>

struct game_text_patch_entry
{
   char anchor[GAME_TEXT_PATCH_TEXT];
   char victim[GAME_TEXT_PATCH_TEXT];
   char key[GAME_TEXT_PATCH_TEXT];
   unsigned long value;
};

static struct game_text_patch_entry s_patches[GAME_TEXT_PATCH_MAX];
static retro_atomic_int_t s_count;

void game_text_patch_clear(void)
{
   retro_atomic_store_release_int(&s_count, 0);
}

static int copy_text(char *dst, const char *src)
{
   size_t n = strlen(src);
   if (!n || n >= GAME_TEXT_PATCH_TEXT)
      return 0;
   memcpy(dst, src, n + 1);
   return 1;
}

int game_text_patch_add(const char *anchor, const char *victim, const char *key, unsigned long value)
{
   int n = retro_atomic_load_acquire_int(&s_count);
   struct game_text_patch_entry *e;

   if (n >= GAME_TEXT_PATCH_MAX)
      return 0;
   e = &s_patches[n];
   if (!copy_text(e->anchor, anchor) || !copy_text(e->victim, victim) || !copy_text(e->key, key))
      return 0;
   e->value = value;
   retro_atomic_store_release_int(&s_count, n + 1);
   return 1;
}

int game_text_patch_count(void)
{
   return retro_atomic_load_acquire_int(&s_count);
}

static int is_blank(char c)
{
   return c == ' ' || c == '\t';
}

int game_text_patch_replace_line(char *text, size_t len, const char *victim, const char *key, unsigned long value)
{
   size_t victim_len = strlen(victim);
   size_t key_len    = strlen(key);
   size_t line       = 0;

   while (line < len)
   {
      size_t end, text_end, p;

      /* The line's text: up to its LF (and a CR before it) or the end */
      for (end = line; end < len && text[end] != '\n'; end++)
         ;
      text_end = (end > line && text[end - 1] == '\r') ? end - 1 : end;

      p = line;
      while (p < text_end && is_blank(text[p]))
         p++;
      if (p + victim_len < text_end && !memcmp(text + p, victim, victim_len)
            && (is_blank(text[p + victim_len]) || text[p + victim_len] == '='))
      {
         size_t q = p + victim_len;

         while (q < text_end && is_blank(text[q]))
            q++;
         if (q < text_end && text[q] == '=')
         {
            char tail[32];
            size_t tail_len, room, k;

            /* "= @value" at the line's end, the key at its start, spaces
             * between: at least one */
            sprintf(tail, "= %lu", value);
            tail_len = strlen(tail);
            room     = text_end - line;
            if (key_len + 1 + tail_len > room)
               return 0;
            memcpy(text + line, key, key_len);
            for (k = line + key_len; k < text_end - tail_len; k++)
               text[k] = ' ';
            memcpy(text + text_end - tail_len, tail, tail_len);
            return 1;
         }
      }
      line = end + 1;
   }
   return 0;
}

/* The first place @needle is in @hay */
static char *find_bytes(char *hay, size_t hay_len, const char *needle, size_t needle_len)
{
   char *p   = hay;
   char *end = hay + hay_len;

   if (!needle_len || hay_len < needle_len)
      return NULL;
   while ((size_t)(end - p) >= needle_len)
   {
      p = (char *)memchr(p, needle[0], (size_t)(end - p) - needle_len + 1);
      if (!p)
         return NULL;
      if (!memcmp(p, needle, needle_len))
         return p;
      p++;
   }
   return NULL;
}

int game_text_patch_apply(char *mem, size_t len)
{
   int n       = retro_atomic_load_acquire_int(&s_count);
   int changed = 0;
   int i;

   for (i = 0; i < n; i++)
   {
      const struct game_text_patch_entry *e = &s_patches[i];
      char *anchor = find_bytes(mem, len, e->anchor, strlen(e->anchor));
      char *start, *stop;

      if (!anchor)
         continue;
      /* The zero-terminated text around the anchor */
      for (start = anchor; start > mem && start[-1] != '\0'; start--)
         ;
      for (stop = anchor; stop < mem + len && *stop != '\0'; stop++)
         ;
      changed += game_text_patch_replace_line(start, (size_t)(stop - start), e->victim, e->key, e->value);
   }
   return changed;
}
