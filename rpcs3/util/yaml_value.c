#include "yaml_value.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int lower_is(const char *s, size_t len, const char *word)
{
   size_t i;

   for (i = 0; i < len; i++)
   {
      const char c = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] - 'A' + 'a' : s[i]);

      if (!word[i] || c != word[i])
         return 0;
   }

   return !word[len];
}

/* One of three four-byte spellings */
static int is_one_of(const char *s, size_t len, const char *a, const char *b, const char *c)
{
   return len == 4 && (!memcmp(s, a, 4) || !memcmp(s, b, 4) || !memcmp(s, c, 4));
}

int yaml_value_bool(const char *s, size_t len, int *out)
{
   if (lower_is(s, len, "true") || lower_is(s, len, "yes") || lower_is(s, len, "on") || lower_is(s, len, "y"))
   {
      *out = 1;
      return 1;
   }

   if (lower_is(s, len, "false") || lower_is(s, len, "no") || lower_is(s, len, "off") || lower_is(s, len, "n"))
   {
      *out = 0;
      return 1;
   }

   return 0;
}

int yaml_value_int(const char *s, size_t len, uint64_t *magnitude, int *negative)
{
   const char *end = s + len;
   unsigned base   = 10;
   uint64_t value  = 0;

   *negative = 0;

   if (s < end && (*s == '-' || *s == '+'))
   {
      *negative = *s == '-';
      s++;
   }

   if (end - s > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
   {
      base = 16;
      s   += 2;
   }

   if (s == end)
      return 0;

   /* Two loops, so each base is a constant (no division a digit) */
   if (base == 10)
   {
      for (; s < end; s++)
      {
         const unsigned d = (unsigned)(*s - '0');

         if (d > 9 || value > UINT64_MAX / 10 || (value == UINT64_MAX / 10 && d > UINT64_MAX % 10))
            return 0;

         value = value * 10 + d;
      }
   }
   else
   {
      for (; s < end; s++)
      {
         unsigned d;

         if (*s >= '0' && *s <= '9')
            d = (unsigned)(*s - '0');
         else if ((*s | 0x20) >= 'a' && (*s | 0x20) <= 'f')
            d = (unsigned)((*s | 0x20) - 'a' + 10);
         else
            return 0;

         if (value >> 60)
            return 0;

         value = value << 4 | d;
      }
   }

   *magnitude = value;
   return 1;
}

int yaml_value_f64(const char *s, size_t len, double *out)
{
   char small[64];
   char *str = small;
   char *end = NULL;
   int ok;

   /* YAML's spellings, exactly */
   if (is_one_of(s, len, ".inf", ".Inf", ".INF") || (len && *s == '+' && is_one_of(s + 1, len - 1, ".inf", ".Inf", ".INF")))
   {
      *out = HUGE_VAL;
      return 1;
   }

   if (len && *s == '-' && is_one_of(s + 1, len - 1, ".inf", ".Inf", ".INF"))
   {
      *out = -HUGE_VAL;
      return 1;
   }

   if (is_one_of(s, len, ".nan", ".NaN", ".NAN"))
   {
      /* Quiet NaN, as std::numeric_limits gave */
      *out = strtod("nan", NULL);
      return 1;
   }

   if (!len)
      return 0;

   /* strtod wants a terminated string: on the stack, the heap only for
    * a number longer than any written */
   if (len >= sizeof(small) && !(str = (char*)malloc(len + 1)))
      return 0;

   memcpy(str, s, len);
   str[len] = '\0';
   *out     = strtod(str, &end);
   ok       = end == str + len;

   if (str != small)
      free(str);

   return ok;
}
