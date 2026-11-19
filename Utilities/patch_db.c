/* RPCS3's game patches, read and applied (patch_db.h). C89. */

#include "patch_db.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <formats/ryaml.h>

#include "../rpcs3/util/yaml_value.h"

#define NONE PATCH_DB_NONE
#define BLOCK_SIZE 65536
#define MAX_LOAD_DEPTH 16
#define PPU_NOP 0x60000000u

static const char *const s_type_names[PATCH_TYPE_COUNT] = {
   "invalid", "load", "alloc", "calloc", "jump", "jumpl", "jumpf", "byte",
   "le16", "le32", "le64", "lef32", "lef64", "be16", "be32", "bd32", "be64",
   "bd64", "bef32", "bef64", "bpex", "utf8", "cutf8", "move_file", "hide_file"
};

static const char *const s_config_type_names[4] = {
   "double_range", "double_enum", "long_range", "long_enum"
};

/* ------------------------------------------------------------------ */
/* arrays and blocks                                                  */
/* ------------------------------------------------------------------ */

/* Room for @need elements in the array at @arrp (a pointer to its
 * pointer), doubling: 1, or 0 with the array as it was */
static int grow(void *arrp, size_t *cap, size_t need, size_t elem)
{
   void *old;
   void *moved;
   size_t n;
   if (need <= *cap)
      return 1;
   n = *cap ? *cap : 16;
   while (n < need)
      n *= 2;
   memcpy(&old, arrp, sizeof(old));
   moved = realloc(old, n * elem);
   if (!moved)
      return 0;
   memcpy(arrp, &moved, sizeof(moved));
   *cap = n;
   return 1;
}

#define RESERVE(db, arr) grow(&(db)->arr, &(db)->cap_##arr, \
      (db)->num_##arr + 1, sizeof(*(db)->arr))

static int keep_block(patch_db_t *db, char *block)
{
   if (!RESERVE(db, blocks))
      return 0;
   db->blocks[db->num_blocks++] = block;
   return 1;
}

/* @len bytes of the database's own, or NULL */
static char *db_alloc(patch_db_t *db, size_t len)
{
   char *p;
   if (len > db->block_left)
   {
      const size_t size = len >= BLOCK_SIZE / 4 ? len : BLOCK_SIZE;
      char *block = (char*)malloc(size ? size : 1);
      if (!block || !keep_block(db, block))
      {
         free(block);
         return NULL;
      }
      if (size != BLOCK_SIZE)
         return block;
      db->block_pos = block;
      db->block_left = size;
   }
   p = db->block_pos;
   db->block_pos += len;
   db->block_left -= len;
   return p;
}

/* ------------------------------------------------------------------ */
/* strings and messages                                               */
/* ------------------------------------------------------------------ */

static int str_eq(patch_str_t a, const char *s, size_t len)
{
   return a.len == len && (!len || memcmp(a.s, s, len) == 0);
}

static int str_same(patch_str_t a, patch_str_t b)
{
   return str_eq(a, b.s, b.len);
}

int patch_db_str_is(patch_str_t str, const char *s)
{
   return str_eq(str, s, strlen(s));
}

typedef struct msgbuf
{
   char s[1024];
   size_t n;
} msgbuf_t;

static void mb_put(msgbuf_t *m, const char *s, size_t len)
{
   const size_t room = sizeof(m->s) - 1 - m->n;
   if (len > room)
      len = room;
   if (len)
      memcpy(m->s + m->n, s, len);
   m->n += len;
   m->s[m->n] = '\0';
}

static void mb_u64(msgbuf_t *m, uint64_t v, unsigned base, int width)
{
   char digits[24];
   int n = 0;
   do
   {
      digits[n++] = "0123456789abcdef"[v % base];
      v /= base;
   } while (v);
   while (n < width)
      digits[n++] = '0';
   while (n)
      mb_put(m, &digits[--n], 1);
}

/* %s a C string, %S a patch_str_t, %u an unsigned long, %x one as 8 hex
 * digits, %X a uint64_t in hex, %d one as signed, %f a double */
static void mb_vfmt(msgbuf_t *m, const char *fmt, va_list ap)
{
   const char *p;
   for (p = fmt; *p; p++)
   {
      if (*p != '%')
      {
         const char *end = strchr(p, '%');
         const size_t len = end ? (size_t)(end - p) : strlen(p);
         mb_put(m, p, len);
         p += len - 1;
         continue;
      }
      switch (*++p)
      {
         case 's':
         {
            const char *s = va_arg(ap, const char*);
            mb_put(m, s ? s : "", s ? strlen(s) : 0);
            break;
         }
         case 'S':
         {
            const patch_str_t s = va_arg(ap, patch_str_t);
            mb_put(m, s.s, s.len);
            break;
         }
         case 'u':
            mb_u64(m, va_arg(ap, unsigned long), 10, 0);
            break;
         case 'x':
            mb_u64(m, va_arg(ap, unsigned long) & 0xffffffffu, 16, 8);
            break;
         case 'X':
            mb_u64(m, va_arg(ap, uint64_t), 16, 0);
            break;
         case 'd':
         {
            const uint64_t v = va_arg(ap, uint64_t);
            if (v >> 63)
            {
               mb_put(m, "-", 1);
               mb_u64(m, 0 - v, 10, 0);
            }
            else
               mb_u64(m, v, 10, 0);
            break;
         }
         case 'f':
         {
            /* %f of a double is 317 characters at most */
            char buf[400];
            sprintf(buf, "%f", va_arg(ap, double));
            mb_put(m, buf, strlen(buf));
            break;
         }
         case '\0':
            return;
         default:
            mb_put(m, p, 1);
            break;
      }
   }
}

static void mb_fmt(msgbuf_t *m, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   mb_vfmt(m, fmt, ap);
   va_end(ap);
}

static void db_log(const patch_db_t *db, int level, const char *fmt, ...)
{
   msgbuf_t m;
   va_list ap;
   if (!db->log)
      return;
   m.n = 0;
   m.s[0] = '\0';
   va_start(ap, fmt);
   mb_vfmt(&m, fmt, ap);
   va_end(ap);
   db->log(db->log_user, level, m.s, m.n);
}

#define ERR (PATCH_LOG_ERROR | PATCH_LOG_USER)

/* ------------------------------------------------------------------ */
/* types and versions                                                 */
/* ------------------------------------------------------------------ */

const char *patch_db_type_name(int type)
{
   return type >= 0 && type < PATCH_TYPE_COUNT ? s_type_names[type] : "unknown";
}

int patch_db_type_from_name(const char *s, size_t len)
{
   uint64_t v = 0;
   size_t i = 0;
   unsigned base = 10;
   int type;
   for (type = 0; type < PATCH_TYPE_COUNT; type++)
   {
      if (strlen(s_type_names[type]) == len && memcmp(s_type_names[type], s, len) == 0)
         return type;
   }
   /* Or its number, as RPCS3's settings read enums: decimal or 0x hex */
   if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
   {
      base = 16;
      i = 2;
   }
   if (i == len)
      return PATCH_INVALID;
   for (; i < len; i++)
   {
      const char c = s[i];
      unsigned d;
      if (c >= '0' && c <= '9')
         d = (unsigned)(c - '0');
      else if (base == 16 && c >= 'a' && c <= 'f')
         d = (unsigned)(c - 'a' + 10);
      else if (base == 16 && c >= 'A' && c <= 'F')
         d = (unsigned)(c - 'A' + 10);
      else
         return PATCH_INVALID;
      v = v * base + d;
      if (v >= PATCH_TYPE_COUNT)
         return PATCH_INVALID;
   }
   return (int)v;
}

int patch_db_compare_versions(patch_str_t a, patch_str_t b, int *ok)
{
   size_t i, j;
   for (i = 0; i < a.len; i++)
      if (a.s[i] != '.' && (a.s[i] < '0' || a.s[i] > '9'))
         break;
   for (j = 0; j < b.len; j++)
      if (b.s[j] != '.' && (b.s[j] < '0' || b.s[j] > '9'))
         break;
   *ok = i == a.len && j == b.len;
   if (!*ok)
      return -1;
   for (i = 0, j = 0; i < a.len || j < b.len; i++, j++)
   {
      unsigned long na = 0, nb = 0;
      for (; i < a.len && a.s[i] != '.'; i++)
         na = na * 10 + (unsigned long)(a.s[i] - '0');
      for (; j < b.len && b.s[j] != '.'; j++)
         nb = nb * 10 + (unsigned long)(b.s[j] - '0');
      if (na != nb)
         return na > nb ? 1 : -1;
   }
   return 0;
}

static int uses_hex_offset(int type)
{
   return type >= PATCH_ALLOC && type <= PATCH_C_UTF8;
}

/* ------------------------------------------------------------------ */
/* containers                                                         */
/* ------------------------------------------------------------------ */

static size_t hash_bytes(const char *s, size_t len)
{
   uint32_t h = 0x811c9dc5u;
   size_t i;
   for (i = 0; i < len; i++)
   {
      h ^= (unsigned char)s[i];
      h *= 0x01000193u;
   }
   return h;
}

size_t patch_db_find(const patch_db_t *db, const char *hash, size_t len)
{
   size_t mask, h;
   if (!db->table_size)
      return NONE;
   mask = db->table_size - 1;
   for (h = hash_bytes(hash, len) & mask; db->table[h]; h = (h + 1) & mask)
   {
      const size_t c = db->table[h] - 1;
      if (str_eq(db->containers[c].hash, hash, len))
         return c;
   }
   return NONE;
}

static int table_grow(patch_db_t *db)
{
   const size_t size = db->table_size ? db->table_size * 2 : 256;
   size_t *table = (size_t*)calloc(size, sizeof(*table));
   size_t c;
   if (!table)
      return 0;
   for (c = 0; c < db->num_containers; c++)
   {
      size_t h = hash_bytes(db->containers[c].hash.s, db->containers[c].hash.len) & (size - 1);
      while (table[h])
         h = (h + 1) & (size - 1);
      table[h] = c + 1;
   }
   free(db->table);
   db->table = table;
   db->table_size = size;
   return 1;
}

static size_t container_get(patch_db_t *db, patch_str_t hash)
{
   size_t c = patch_db_find(db, hash.s, hash.len);
   size_t h;
   patch_container_t *pc;
   if (c != NONE)
      return c;
   if ((db->num_containers + 1) * 2 > db->table_size && !table_grow(db))
      return NONE;
   if (!RESERVE(db, containers))
      return NONE;
   c = db->num_containers++;
   pc = &db->containers[c];
   pc->hash = hash;
   pc->version.s = "";
   pc->version.len = 0;
   pc->first = pc->last = NONE;
   pc->count = 0;
   for (h = hash_bytes(hash.s, hash.len) & (db->table_size - 1); db->table[h];
         h = (h + 1) & (db->table_size - 1))
      ;
   db->table[h] = c + 1;
   return c;
}

size_t patch_db_find_patch(const patch_db_t *db, size_t container,
      const char *description, size_t len)
{
   size_t p;
   if (container == NONE)
      return NONE;
   for (p = db->containers[container].first; p != NONE; p = db->patches[p].next)
   {
      if (str_eq(db->patches[p].description, description, len))
         return p;
   }
   return NONE;
}

/* ------------------------------------------------------------------ */
/* values of nodes                                                    */
/* ------------------------------------------------------------------ */

static int is_scalar(const ryaml_t *y, int n)
{
   return ryaml_type(y, n) == RYAML_TYPE_SCALAR && ryaml_has_val(y, n);
}

static const char *type_name(const ryaml_t *y, int n)
{
   if (ryaml_is_map(y, n))
      return "Map";
   if (ryaml_is_seq(y, n))
      return "Sequence";
   if (is_scalar(y, n))
      return "Scalar";
   return "Null";
}

static patch_str_t raw_val(const ryaml_t *y, int n)
{
   patch_str_t s;
   s.len = 0;
   s.s = ryaml_val(y, n, &s.len);
   if (!s.s)
   {
      s.s = "";
      s.len = 0;
   }
   return s;
}

static int child(const ryaml_t *y, int n, int index)
{
   int c = ryaml_first_child(y, n);
   while (c >= 0 && index--)
      c = ryaml_next_sibling(y, c);
   return c;
}

static int find(const ryaml_t *y, int n, const char *key)
{
   return ryaml_find_child_len(y, n, key, strlen(key));
}

/* An integer of @bits bits as RPCS3 reads one (decimal or 0x hex, a sign
 * where the type has one, in range): 1, or 0 with the reason in @err.
 * A signed value comes sign-extended. */
static int node_int(const ryaml_t *y, int n, int bits, int is_signed,
      uint64_t *out, msgbuf_t *err)
{
   const patch_str_t s = raw_val(y, n);
   uint64_t mag = 0;
   int neg = 0;
   int ok;
   const uint64_t max = bits >= 64 ? (~(uint64_t)0 >> is_signed)
      : ((uint64_t)1 << (bits - is_signed)) - 1;
   *out = 0;
   if (!is_scalar(y, n))
   {
      mb_fmt(err, "expected a scalar, found %s", type_name(y, n));
      return 0;
   }
   ok = yaml_value_int(s.s, s.len, &mag, &neg) != 0;
   if (ok && neg)
   {
      if (is_signed)
      {
         ok = mag <= max + 1;
         *out = 0 - mag;
      }
      else
         ok = mag == 0;
   }
   else if (ok)
   {
      ok = mag <= max;
      *out = mag;
   }
   if (!ok)
   {
      *out = 0;
      mb_fmt(err, "bad conversion: '%S' is not a %s", s,
            is_signed ? "signed integer in range" : "unsigned integer in range");
   }
   return ok;
}

static int node_f64(const ryaml_t *y, int n, double *out, msgbuf_t *err)
{
   const patch_str_t s = raw_val(y, n);
   *out = 0;
   if (!is_scalar(y, n))
   {
      mb_fmt(err, "expected a scalar, found %s", type_name(y, n));
      return 0;
   }
   if (!yaml_value_f64(s.s, s.len, out))
   {
      *out = 0;
      mb_fmt(err, "bad conversion: '%S' is not a number", s);
      return 0;
   }
   return 1;
}

/* A value as the messages show it: "?" for none */
static patch_str_t or_q(patch_str_t s)
{
   if (!s.len)
   {
      s.s = "?";
      s.len = 1;
   }
   return s;
}

static uint32_t node_u32_or(const ryaml_t *y, int n, uint32_t fallback)
{
   msgbuf_t err;
   uint64_t v;
   err.n = 0;
   return node_int(y, n, 32, 0, &v, &err) ? (uint32_t)v : fallback;
}

static int node_bool_or(const ryaml_t *y, int n, int fallback)
{
   const patch_str_t s = raw_val(y, n);
   int v = 0;
   return is_scalar(y, n) && yaml_value_bool(s.s, s.len, &v) ? v != 0 : fallback;
}

static double node_f64_or(const ryaml_t *y, int n, double fallback)
{
   msgbuf_t err;
   double v;
   err.n = 0;
   return node_f64(y, n, &v, &err) ? v : fallback;
}

/* static_cast<u64> of a configurable value, kept to @bits */
static uint64_t f64_to_bits(double v, int bits)
{
   uint64_t u;
   if (v != v)
      u = 0;
   else if (v < 0)
      u = (uint64_t)(int64_t)v;
   else
      u = (uint64_t)v;
   return bits >= 64 ? u : u & (((uint64_t)1 << bits) - 1);
}

/* ------------------------------------------------------------------ */
/* reading a file                                                     */
/* ------------------------------------------------------------------ */

typedef struct reader
{
   patch_db_t *db;
   const ryaml_t *y;
   const char *text;
   size_t len;
   const char *path;
   patch_str_t path_str;
   patch_str_t hash;
   const ryaml_t *cy;           /* patch_config.yml, or NULL */
   int croot;
   int ok;                      /* nothing wrong so far */
   int oom;
} reader_t;

/* Where it lies when in the text, else a copy */
static patch_str_t rd_str(reader_t *r, const char *s, size_t len)
{
   patch_str_t out;
   out.s = "";
   out.len = 0;
   if (!s || !len)
      return out;
   if ((uintptr_t)s >= (uintptr_t)r->text && (uintptr_t)s - (uintptr_t)r->text <= r->len
         && len <= r->len - ((uintptr_t)s - (uintptr_t)r->text))
      out.s = s;
   else
   {
      char *copy = db_alloc(r->db, len);
      if (!copy)
      {
         r->oom = 1;
         return out;
      }
      memcpy(copy, s, len);
      out.s = copy;
   }
   out.len = len;
   return out;
}

static patch_str_t rd_key(reader_t *r, int n)
{
   size_t len = 0;
   const char *k = ryaml_key(r->y, n, &len);
   return rd_str(r, k, len);
}

static patch_str_t rd_val(reader_t *r, int n)
{
   const patch_str_t s = raw_val(r->y, n);
   return rd_str(r, s.s, s.len);
}

static const char *loc(const ryaml_t *y, int n, char *buf)
{
   size_t line = 0, col = 0;
   if (!ryaml_location(y, n, &line, &col))
      return "unknown";
   sprintf(buf, "line %lu, column %lu", (unsigned long)line, (unsigned long)col);
   return buf;
}

static size_t config_named(const patch_db_t *db, const patch_info_t *info,
      const char *name, size_t len)
{
   size_t k;
   for (k = info->first_config; k < info->first_config + info->num_configs; k++)
   {
      if (str_eq(db->configs[k].name, name, len))
         return k;
   }
   return NONE;
}

/* A configurable value's Value, Min, Max or allowed value, read as its
 * type says */
static double rd_config_number(reader_t *r, const patch_config_value_t *cv,
      int node, int cnode)
{
   msgbuf_t err;
   double v = 0;
   int ok;
   err.n = 0;
   err.s[0] = '\0';
   if (cv->type == PATCH_CONFIG_LONG_RANGE || cv->type == PATCH_CONFIG_LONG_ENUM)
   {
      uint64_t i = 0;
      ok = node_int(r->y, node, 64, 1, &i, &err);
      v = (double)(int64_t)i;
   }
   else
      ok = node_f64(r->y, node, &v, &err);
   if (!ok)
   {
      char lb[64];
      db_log(r->db, ERR, "Error: Invalid data type found in configurable value: %s (key: %S, location: %s, file: %s)",
            err.s, r->hash, loc(r->y, cnode, lb), r->path);
      r->ok = 0;
   }
   return v;
}

static void rd_config_value(reader_t *r, patch_info_t *info, int c)
{
   patch_db_t *db = r->db;
   const ryaml_t *y = r->y;
   const patch_str_t name = rd_key(r, c);
   patch_config_value_t *cv;
   size_t k;
   int tn, t, vn, an;
   char lb[64];

   k = config_named(db, info, name.s, name.len);
   if (k == NONE)
   {
      if (!RESERVE(db, configs))
      {
         r->oom = 1;
         return;
      }
      k = db->num_configs++;
      info->num_configs++;
      cv = &db->configs[k];
      memset(cv, 0, sizeof(*cv));
      cv->name = name;
   }
   cv = &db->configs[k];

   tn = find(y, c, "Type");
   if (tn == RYAML_NONE || !is_scalar(y, tn))
   {
      db_log(db, ERR, "Error: Invalid or missing configurable type (key: %S, location: %s, file: %s)",
            r->hash, loc(y, c, lb), r->path);
      r->ok = 0;
      return;
   }
   {
      const patch_str_t ts = raw_val(y, tn);
      for (t = 0; t < 4; t++)
         if (patch_db_str_is(ts, s_config_type_names[t]))
            break;
   }
   if (t == 4)
   {
      db_log(db, ERR, "Error: Invalid configurable type (key: %S, location: %s, file: %s)",
            r->hash, loc(y, tn, lb), r->path);
      r->ok = 0;
      return;
   }
   cv->type = t;

   vn = find(y, c, "Value");
   if (vn != RYAML_NONE && is_scalar(y, vn))
      cv->value = rd_config_number(r, cv, vn, c);
   else
   {
      db_log(db, ERR, "Error: Invalid or missing configurable value (key: %S, location: %s, file: %s)",
            r->hash, loc(y, c, lb), r->path);
      r->ok = 0;
   }

   if (t == PATCH_CONFIG_DOUBLE_RANGE || t == PATCH_CONFIG_LONG_RANGE)
   {
      const int mn = find(y, c, "Min");
      const int mx = find(y, c, "Max");
      if (mn != RYAML_NONE && is_scalar(y, mn))
         cv->min = rd_config_number(r, cv, mn, c);
      else
      {
         db_log(db, ERR, "Error: Invalid or missing configurable min (key: %S, location: %s, file: %s)",
               r->hash, loc(y, c, lb), r->path);
         r->ok = 0;
      }
      if (mx != RYAML_NONE && is_scalar(y, mx))
         cv->max = rd_config_number(r, cv, mx, c);
      else
      {
         db_log(db, ERR, "Error: Invalid or missing configurable max (key: %S, location: %s, file: %s)",
               r->hash, loc(y, c, lb), r->path);
         r->ok = 0;
      }
      if (cv->min >= cv->max)
      {
         db_log(db, ERR, "Error: Configurable max has to be larger than configurable min (key: %S, location: %s, file: %s)",
               r->hash, loc(y, c, lb), r->path);
         r->ok = 0;
      }
      if (cv->value < cv->min || cv->value > cv->max)
      {
         db_log(db, ERR, "Error: Configurable value out of range (key: %S, location: %s, file: %s)",
               r->hash, loc(y, c, lb), r->path);
         r->ok = 0;
      }
      return;
   }

   an = find(y, c, "Allowed Values");
   if (an == RYAML_NONE || !ryaml_is_map(y, an))
   {
      db_log(db, ERR, "Error: Invalid or missing configurable allowed values (key: %S, location: %s, file: %s)",
            r->hash, loc(y, c, lb), r->path);
      r->ok = 0;
      return;
   }
   {
      int a;
      size_t i;
      int found = 0;
      cv->first_allowed = db->num_allowed;
      cv->num_allowed = 0;
      for (a = ryaml_first_child(y, an); a != RYAML_NONE; a = ryaml_next_sibling(y, a))
      {
         patch_allowed_value_t av;
         int dup = 0;
         if (!is_scalar(y, a))
         {
            db_log(db, ERR, "Error: Skipping configurable allowed value (patch: %S, key: %S, location: %s, file: %s)",
                  info->description, r->hash, loc(y, a, lb), r->path);
            r->ok = 0;
            continue;
         }
         av.label = rd_key(r, a);
         av.value = rd_config_number(r, cv, a, c);
         for (i = cv->first_allowed; i < cv->first_allowed + cv->num_allowed; i++)
            if (db->allowed[i].value == av.value || str_same(db->allowed[i].label, av.label))
               dup = 1;
         if (dup)
         {
            db_log(db, ERR, "Error: Skipping configurable allowed value. Another entry with the same label or value already exists. (patch: %S, key: %S, location: %s, file: %s)",
                  info->description, r->hash, loc(y, a, lb), r->path);
            r->ok = 0;
            continue;
         }
         if (!RESERVE(db, allowed))
         {
            r->oom = 1;
            return;
         }
         db->allowed[db->num_allowed++] = av;
         cv->num_allowed++;
      }
      if (cv->num_allowed < 2)
      {
         db_log(db, ERR, "Error: Configurable allowed values need at least 2 entries (key: %S, location: %s, file: %s)",
               r->hash, loc(y, an, lb), r->path);
         r->ok = 0;
      }
      for (i = cv->first_allowed; i < cv->first_allowed + cv->num_allowed; i++)
         if (db->allowed[i].value == cv->value)
            found = 1;
      if (!found)
      {
         db_log(db, ERR, "Error: Configurable value was not found in allowed values (key: %S, location: %s, file: %s)",
               r->hash, loc(y, an, lb), r->path);
         r->ok = 0;
      }
   }
}

/* What patch_config.yml says of a target: on or off, and its values */
static void rd_target_config(reader_t *r, const patch_info_t *info, patch_target_t *t)
{
   patch_db_t *db = r->db;
   const ryaml_t *cy = r->cy;
   int n;
   t->enabled = 0;
   t->first_override = db->num_overrides;
   t->num_overrides = 0;
   if (!cy)
      return;
   n = ryaml_find_child_len(cy, r->croot, info->hash.s, info->hash.len);
   n = ryaml_find_child_len(cy, n, info->description.s, info->description.len);
   n = ryaml_find_child_len(cy, n, t->title.s, t->title.len);
   n = ryaml_find_child_len(cy, n, t->serial.s, t->serial.len);
   n = ryaml_find_child_len(cy, n, t->app_version.s, t->app_version.len);
   if (n == RYAML_NONE)
      return;
   if (!ryaml_is_map(cy, n))
   {
      /* Legacy: on or off alone */
      t->enabled = node_bool_or(cy, n, 0);
      return;
   }
   {
      const int e = find(cy, n, "Enabled");
      const int values = find(cy, n, "Configurable Values");
      int v;
      if (e != RYAML_NONE)
         t->enabled = node_bool_or(cy, e, 0);
      for (v = ryaml_first_child(cy, values); v != RYAML_NONE; v = ryaml_next_sibling(cy, v))
      {
         size_t len = 0;
         const char *name = ryaml_key(cy, v, &len);
         const size_t k = name ? config_named(db, info, name, len) : NONE;
         if (k == NONE)
            continue; /* not one of the patch's */
         if (!RESERVE(db, overrides))
         {
            r->oom = 1;
            return;
         }
         db->overrides[db->num_overrides].config = k;
         db->overrides[db->num_overrides].value = node_f64_or(cy, v, 0.0);
         db->num_overrides++;
         t->num_overrides++;
      }
   }
}

static int valid_serial(patch_str_t s)
{
   size_t i;
   if (s.len != 9)
      return 0;
   for (i = 0; i < s.len; i++)
   {
      const char c = s.s[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
         return 0;
   }
   return 1;
}

/* "All", or two digits, a dot and two digits */
static int valid_app_version(patch_str_t s)
{
   static const char digit[] = { 1, 1, 0, 1, 1 };
   size_t i;
   if (patch_db_str_is(s, "All"))
      return 1;
   if (s.len != 5)
      return 0;
   for (i = 0; i < 5; i++)
   {
      if (digit[i] ? (s.s[i] < '0' || s.s[i] > '9') : s.s[i] != '.')
         return 0;
   }
   return 1;
}

/* The Games of a patch: 0 when it is not a map (the patch is skipped) */
static int rd_games(reader_t *r, patch_info_t *info, int games)
{
   patch_db_t *db = r->db;
   const ryaml_t *y = r->y;
   const patch_str_t desc = info->description;
   char lb[64];
   int game;

   if (!ryaml_is_map(y, games))
   {
      db_log(db, ERR, "Error: Skipping Games key: expected Map, found %s (patch: %S, key: %S, location: %s, file: %s)",
            type_name(y, games), desc, r->hash, loc(y, games, lb), r->path);
      r->ok = 0;
      return 0;
   }

   for (game = ryaml_first_child(y, games); game != RYAML_NONE; game = ryaml_next_sibling(y, game))
   {
      const patch_str_t title = rd_key(r, game);
      int title_is_all, sn;
      if (!title.len)
      {
         db_log(db, ERR, "Error: Empty game title (key: %S, location: %s, file: %s)",
               r->hash, loc(y, game, lb), r->path);
         r->ok = 0;
         continue;
      }
      if (!ryaml_is_map(y, game))
      {
         db_log(db, ERR, "Error: Skipping game %S: expected Map, found %s (patch: %S, key: %S, location: %s, file: %s)",
               title, type_name(y, game), desc, r->hash, loc(y, game, lb), r->path);
         r->ok = 0;
         continue;
      }
      title_is_all = patch_db_str_is(title, "All");

      for (sn = ryaml_first_child(y, game); sn != RYAML_NONE; sn = ryaml_next_sibling(y, sn))
      {
         const patch_str_t serial = rd_key(r, sn);
         const size_t first = db->num_targets;
         int vn;
         if (!serial.len)
         {
            db_log(db, ERR, "Error: Using empty serial (title: %S, patch: %S, key: %S, location: %s, file: %s)",
                  title, desc, r->hash, loc(y, sn, lb), r->path);
            r->ok = 0;
            continue;
         }
         if (patch_db_str_is(serial, "All"))
         {
            if (!title_is_all)
            {
               db_log(db, ERR, "Error: Using 'All' as serial is not allowed for titles other than 'All' (title: %S, patch: %S, key: %S, location: %s, file: %s)",
                     title, desc, r->hash, loc(y, sn, lb), r->path);
               r->ok = 0;
               continue;
            }
         }
         else if (title_is_all)
         {
            db_log(db, ERR, "Error: Only 'All' is allowed as serial if the title is 'All' (serial: %S, patch: %S, key: %S, location: %s, file: %s)",
                  serial, desc, r->hash, loc(y, sn, lb), r->path);
            r->ok = 0;
            continue;
         }
         else if (!valid_serial(serial))
         {
            db_log(db, ERR, "Error: Serial '%S' invalid (patch: %S, key: %S, location: %s, file: %s)",
                  serial, desc, r->hash, loc(y, sn, lb), r->path);
            r->ok = 0;
            continue;
         }
         if (!ryaml_is_seq(y, sn))
         {
            db_log(db, ERR, "Error: Skipping %S: expected Sequence, found %s (title: %S, patch: %S, key: %S, location: %s, file: %s)",
                  serial, type_name(y, sn), title, desc, r->hash, loc(y, sn, lb), r->path);
            r->ok = 0;
            continue;
         }

         for (vn = ryaml_first_child(y, sn); vn != RYAML_NONE; vn = ryaml_next_sibling(y, vn))
         {
            const patch_str_t app_version = rd_val(r, vn);
            patch_target_t *t;
            size_t i;
            int dup = 0;
            if (!valid_app_version(app_version))
            {
               db_log(db, ERR, "Error: Skipping invalid app version '%S' (title: %S, serial: %S, patch: %S, key: %S, location: %s, file: %s)",
                     app_version, title, serial, desc, r->hash, loc(y, sn, lb), r->path);
               r->ok = 0;
               continue;
            }
            for (i = first; i < db->num_targets; i++)
               if (str_same(db->targets[i].app_version, app_version))
                  dup = 1;
            if (dup)
            {
               db_log(db, ERR, "Error: Skipping duplicate app version '%S' (title: %S, serial: %S, patch: %S, key: %S, location: %s, file: %s)",
                     app_version, title, serial, desc, r->hash, loc(y, sn, lb), r->path);
               r->ok = 0;
               continue;
            }
            if (!RESERVE(db, targets))
            {
               r->oom = 1;
               return 1;
            }
            t = &db->targets[db->num_targets++];
            info->num_targets++;
            t->title = title;
            t->serial = serial;
            t->app_version = app_version;
            rd_target_config(r, info, t);
         }

         if (db->num_targets == first)
         {
            db_log(db, ERR, "Error: Skipping %S: empty Sequence (title: %S, patch: %S, key: %S, location: %s, file: %s)",
                  serial, title, desc, r->hash, loc(y, sn, lb), r->path);
            r->ok = 0;
         }
      }
   }
   return 1;
}

static int patch_type_of(const ryaml_t *y, int n)
{
   patch_str_t s;
   if (n == RYAML_NONE || !is_scalar(y, n))
      return PATCH_INVALID;
   s = raw_val(y, n);
   return patch_db_type_from_name(s.s, s.len);
}

/* One entry of a patch, [ type, address, value ], or a load of an anchor's
 * entries: 1, or 0 when it was skipped */
static int rd_data(reader_t *r, patch_info_t *info, int node, uint32_t modifier, int depth)
{
   patch_db_t *db = r->db;
   const ryaml_t *y = r->y;
   const patch_str_t desc = info->description;
   char lb[64];
   int tn, an, vn, type;
   patch_data_t d;
   msgbuf_t err;
   int ok = 1;

   if (node == RYAML_NONE || !ryaml_is_seq(y, node))
   {
      db_log(db, ERR, "Skipping invalid patch node %S. (key: %S, location: %s, file: %s)",
            desc, r->hash, loc(y, node, lb), r->path);
      return 0;
   }

   tn = child(y, node, 0);
   an = child(y, node, 1);
   vn = child(y, node, 2);
   type = patch_type_of(y, tn);

   if (type == PATCH_INVALID)
   {
      db_log(db, ERR, "Skipping patch node %S: type '%S' is invalid. (key: %S, location: %s, file: %s)",
            desc, is_scalar(y, tn) ? raw_val(y, tn) : raw_val(y, RYAML_NONE), r->hash, loc(y, node, lb), r->path);
      return 0;
   }

   if (type == PATCH_LOAD)
   {
      /* An anchor's entries (a named sequence), at an offset */
      uint32_t mod;
      int item;
      if (!ryaml_is_seq(y, an))
      {
         db_log(db, ERR, "Skipping patch node %S: expected Sequence, found %s (key: %S, location: %s, file: %s)",
               desc, type_name(y, an), r->hash, loc(y, node, lb), r->path);
         return 0;
      }
      if (depth >= MAX_LOAD_DEPTH)
      {
         db_log(db, ERR, "Skipping patch node %S: loads nested too deep (key: %S, location: %s, file: %s)",
               desc, r->hash, loc(y, node, lb), r->path);
         return 0;
      }
      mod = node_u32_or(y, vn, 0);
      for (item = ryaml_first_child(y, an); item != RYAML_NONE; item = ryaml_next_sibling(y, item))
      {
         if (!rd_data(r, info, item, mod, depth + 1))
            ok = 0;
      }
      return ok;
   }

   if (!is_scalar(y, vn))
   {
      db_log(db, ERR, "Skipping patch node %S. Value element has wrong type %s. (key: %S, location: %s, file: %s)",
            desc, type_name(y, vn), r->hash, loc(y, node, lb), r->path);
      return 0;
   }

   memset(&d, 0, sizeof(d));
   d.type = type;
   d.original_offset = rd_val(r, an);
   if (uses_hex_offset(type) && !(d.original_offset.len >= 2
            && d.original_offset.s[0] == '0' && d.original_offset.s[1] == 'x'))
   {
      db_log(db, ERR, "Skipping patch node %S. Address element has wrong format %S. (key: %S, location: %s, file: %s)",
            desc, d.original_offset, r->hash, loc(y, node, lb), r->path);
      return 0;
   }
   d.offset = node_u32_or(y, an, 0) + modifier;
   d.original_value = rd_val(r, vn);
   d.config = config_named(db, info, d.original_value.s, d.original_value.len);

   err.n = 0;
   err.s[0] = '\0';

   /* The offset, but for paths */
   if (type != PATCH_MOVE_FILE && type != PATCH_HIDE_FILE)
   {
      uint64_t v;
      if (!node_int(y, an, 32, 0, &v, &err))
      {
         db_log(db, ERR, "Skipping patch data entry: [ %s, 0x%x, %S ] (key: %S, location: %s, file: %s) Invalid patch offset '%S' (not a valid u32 or overflow)",
               s_type_names[type], (unsigned long)d.offset, or_q(d.original_value),
               r->hash, loc(y, node, lb), r->path, d.original_offset);
         return 0;
      }
      if (0xffffffffu - modifier < d.offset)
      {
         db_log(db, ERR, "Skipping patch data entry: [ %s, 0x%x, %S ] (key: %S, location: %s, file: %s) Invalid combination of patch offset 0x%x and modifier 0x%x (overflow)",
               s_type_names[type], (unsigned long)d.offset, or_q(d.original_value),
               r->hash, loc(y, node, lb), r->path, (unsigned long)d.offset, (unsigned long)modifier);
         return 0;
      }
   }

   /* The value */
   {
      const double cfg = d.config != NONE ? db->configs[d.config].value : 0;
      int bits = 0;
      switch (type)
      {
         case PATCH_BEF32:
         case PATCH_LEF32:
            if (d.config != NONE)
               d.value.d = cfg;
            else if (node_f64(y, vn, &d.value.d, &err))
               d.value.d = (double)(float)d.value.d;
            break;
         case PATCH_BEF64:
         case PATCH_LEF64:
            if (d.config != NONE)
               d.value.d = cfg;
            else
               node_f64(y, vn, &d.value.d, &err);
            break;
         case PATCH_BYTE:
            bits = 8;
            break;
         case PATCH_LE16:
         case PATCH_BE16:
            bits = 16;
            break;
         case PATCH_LE32:
         case PATCH_BE32:
         case PATCH_BD32:
            bits = 32;
            break;
         case PATCH_ALLOC:
         case PATCH_CODE_ALLOC:
         case PATCH_JUMP:
         case PATCH_JUMP_LINK:
         case PATCH_LE64:
         case PATCH_BE64:
         case PATCH_BD64:
            bits = 64;
            break;
         default:
            break;
      }
      if (bits)
      {
         if (d.config != NONE)
            d.value.u = f64_to_bits(cfg, bits);
         else if (!node_int(y, vn, bits, 0, &d.value.u, &err))
         {
            /* Unsigned or else signed, as RPCS3 reads them */
            err.n = 0;
            err.s[0] = '\0';
            node_int(y, vn, bits, 1, &d.value.u, &err);
         }
      }
   }

   if (err.n)
   {
      db_log(db, ERR, "Skipping patch data entry: [ %s, 0x%x, %S ] (key: %S, location: %s, file: %s) %s",
            s_type_names[type], (unsigned long)d.offset, or_q(d.original_value),
            r->hash, loc(y, node, lb), r->path, err.s);
      return 0;
   }

   if (!RESERVE(db, data))
   {
      r->oom = 1;
      return 0;
   }
   db->data[db->num_data++] = d;
   info->num_data++;
   return 1;
}

/* Drops what a skipped patch added at the ends of the arrays */
typedef struct marks
{
   size_t data, targets, configs, allowed, overrides;
} marks_t;

static void mark(const patch_db_t *db, marks_t *m)
{
   m->data = db->num_data;
   m->targets = db->num_targets;
   m->configs = db->num_configs;
   m->allowed = db->num_allowed;
   m->overrides = db->num_overrides;
}

static void rollback(patch_db_t *db, const marks_t *m)
{
   db->num_data = m->data;
   db->num_targets = m->targets;
   db->num_configs = m->configs;
   db->num_allowed = m->allowed;
   db->num_overrides = m->overrides;
}

static void rd_patch(reader_t *r, size_t ci, patch_str_t version, int entry)
{
   patch_db_t *db = r->db;
   const ryaml_t *y = r->y;
   patch_info_t info;
   marks_t m;
   char lb[64];
   int n;
   size_t existing;

   memset(&info, 0, sizeof(info));
   info.description = rd_key(r, entry);
   if (!info.description.len)
   {
      db_log(db, ERR, "Error: Empty patch name (key: %S, location: %s, file: %s)",
            r->hash, loc(y, entry, lb), r->path);
      r->ok = 0;
      return;
   }
   if (!ryaml_is_map(y, entry))
   {
      db_log(db, ERR, "Error: Skipping Patch key %S: expected Map, found %s (key: %S, location: %s, file: %s)",
            info.description, type_name(y, entry), r->hash, loc(y, entry, lb), r->path);
      r->ok = 0;
      return;
   }

   info.hash = r->hash;
   info.version = version;
   info.source_path = r->path_str;
   info.next = NONE;
   mark(db, &m);
   info.first_data = db->num_data;
   info.first_target = db->num_targets;
   info.first_config = db->num_configs;

   if ((n = find(y, entry, "Author")) != RYAML_NONE)
      info.author = rd_val(r, n);
   if ((n = find(y, entry, "Patch Version")) != RYAML_NONE)
      info.patch_version = rd_val(r, n);
   if ((n = find(y, entry, "Notes")) != RYAML_NONE)
   {
      if (!ryaml_is_seq(y, n))
         info.notes = rd_val(r, n);
      else
      {
         /* Notes in pieces, joined */
         size_t total = 0;
         int note;
         char *joined;
         for (note = ryaml_first_child(y, n); note != RYAML_NONE; note = ryaml_next_sibling(y, note))
         {
            if (is_scalar(y, note))
               total += raw_val(y, note).len;
            else
            {
               db_log(db, ERR, "Error: Skipping sequenced Note (patch: %S, key: %S, location: %s, file: %s)",
                     info.description, r->hash, loc(y, note, lb), r->path);
               r->ok = 0;
            }
         }
         if (total && (joined = db_alloc(db, total)) != NULL)
         {
            info.notes.s = joined;
            info.notes.len = total;
            for (note = ryaml_first_child(y, n); note != RYAML_NONE; note = ryaml_next_sibling(y, note))
            {
               if (is_scalar(y, note))
               {
                  const patch_str_t piece = raw_val(y, note);
                  memcpy(joined, piece.s, piece.len);
                  joined += piece.len;
               }
            }
         }
         else if (total)
            r->oom = 1;
      }
   }
   if ((n = find(y, entry, "Group")) != RYAML_NONE)
      info.group = rd_val(r, n);

   /* The configurable values before the games, which patch_config.yml
    * sets them for */
   if ((n = find(y, entry, "Configurable Values")) != RYAML_NONE)
   {
      if (!ryaml_is_map(y, n) || ryaml_num_children(y, n) == 0)
      {
         db_log(db, ERR, "Error: Skipping configurable values: expected Map, found %s (patch: %S, key: %S, location: %s, file: %s)",
               type_name(y, n), info.description, r->hash, loc(y, n, lb), r->path);
         r->ok = 0;
      }
      else
      {
         int c;
         for (c = ryaml_first_child(y, n); c != RYAML_NONE && !r->oom; c = ryaml_next_sibling(y, c))
         {
            if (!ryaml_is_map(y, c))
            {
               db_log(db, ERR, "Error: Skipping configurable value %S: expected Map, found %s (patch: %S, key: %S, location: %s, file: %s)",
                     rd_key(r, c), type_name(y, c), info.description, r->hash, loc(y, c, lb), r->path);
               r->ok = 0;
               continue;
            }
            rd_config_value(r, &info, c);
         }
      }
   }

   if ((n = find(y, entry, "Games")) != RYAML_NONE && !rd_games(r, &info, n))
   {
      rollback(db, &m);
      return;
   }

   if ((n = find(y, entry, "Patch")) != RYAML_NONE)
   {
      int valid = 1;
      if (!ryaml_is_seq(y, n))
      {
         db_log(db, ERR, "Skipping patch node %S: expected Sequence, found %s (key: %S, location: %s)",
               info.description, type_name(y, n), r->hash, loc(y, n, lb));
         valid = 0;
      }
      else
      {
         int item;
         for (item = ryaml_first_child(y, n); item != RYAML_NONE && !r->oom; item = ryaml_next_sibling(y, item))
            if (!rd_data(r, &info, item, 0, 0))
               valid = 0;
      }
      if (!valid)
      {
         db_log(db, ERR, "Skipping invalid patch node %S: (key: %S, location: %s, file: %s)",
               info.description, r->hash, loc(y, n, lb), r->path);
         r->ok = 0;
      }
   }

   if (r->oom)
   {
      rollback(db, &m);
      return;
   }

   /* A patch already there stays unless this is a higher version */
   existing = patch_db_find_patch(db, ci, info.description.s, info.description.len);
   if (existing != NONE)
   {
      patch_info_t *old = &db->patches[existing];
      int vok;
      const int cmp = patch_db_compare_versions(info.patch_version, old->patch_version, &vok);
      if (!vok || cmp <= 0)
      {
         db_log(db, PATCH_LOG_WARNING | PATCH_LOG_USER, "A higher or equal patch version already exists ('%S' vs '%S') for %S: %S (in file %s)",
               info.patch_version, old->patch_version, r->hash, info.description, r->path);
         rollback(db, &m);
         return;
      }
      db_log(db, PATCH_LOG_WARNING, "A lower patch version was found ('%S' vs '%S') for %S: %S (in file %S)",
            old->patch_version, info.patch_version, r->hash, info.description, old->source_path);
      info.next = old->next;
      *old = info;
      return;
   }

   if (!RESERVE(db, patches))
   {
      r->oom = 1;
      rollback(db, &m);
      return;
   }
   db->patches[db->num_patches] = info;
   {
      patch_container_t *c = &db->containers[ci];
      if (c->last == NONE)
         c->first = db->num_patches;
      else
         db->patches[c->last].next = db->num_patches;
      c->last = db->num_patches;
      c->count++;
   }
   db->num_patches++;
}

void patch_db_init(patch_db_t *db, patch_db_log_t log, void *log_user)
{
   memset(db, 0, sizeof(*db));
   db->log = log;
   db->log_user = log_user;
}

void patch_db_free(patch_db_t *db)
{
   size_t i;
   for (i = 0; i < db->num_blocks; i++)
      free(db->blocks[i]);
   free(db->blocks);
   free(db->containers);
   free(db->patches);
   free(db->data);
   free(db->targets);
   free(db->configs);
   free(db->allowed);
   free(db->overrides);
   free(db->table);
   free(db->groups);
   patch_db_init(db, db->log, db->log_user);
}

int patch_db_load(patch_db_t *db, const char *path, char *text, size_t len,
      const char *config, size_t config_len)
{
   reader_t r;
   ryaml_t *y;
   ryaml_t *cy = NULL;
   size_t eline = 0, ecol = 0;
   char lb[64];
   int root, vn, pair;
   patch_str_t version;

   memset(&r, 0, sizeof(r));
   if (text && !keep_block(db, text))
   {
      free(text);
      db_log(db, PATCH_LOG_FATAL, "Failed to load patch file %s: out of memory", path);
      return 0;
   }

   y = ryaml_parse_ex(text ? text : "", text ? len : 0, &eline, &ecol);
   if (!y)
   {
      db_log(db, PATCH_LOG_QUIET | PATCH_LOG_USER, "Fatal Error: Failed to load file!");
      if (eline)
         db_log(db, PATCH_LOG_FATAL, "Failed to load patch file %s:\nYAML: cannot read line %u, column %u",
               path, (unsigned long)eline, (unsigned long)ecol);
      else
         db_log(db, PATCH_LOG_FATAL, "Failed to load patch file %s:\nYAML: out of memory", path);
      return 0;
   }

   if (config)
   {
      cy = ryaml_parse(config, config_len);
      if (!cy)
         db_log(db, PATCH_LOG_FATAL, "Failed to load patch config file (for %s)", path);
   }

   r.db = db;
   r.y = y;
   r.text = text ? text : "";
   r.len = text ? len : 0;
   r.path = path;
   r.cy = cy;
   r.croot = cy ? ryaml_root(cy) : RYAML_NONE;
   r.ok = 1;
   root = ryaml_root(y);

   /* The file's path, once for all its patches */
   {
      const size_t plen = strlen(path);
      char *copy = db_alloc(db, plen);
      if (copy || !plen)
      {
         if (plen)
            memcpy(copy, path, plen);
         r.path_str.s = copy ? copy : "";
         r.path_str.len = plen;
      }
      else
         r.oom = 1;
   }

   vn = find(y, root, "Version");
   if (vn == RYAML_NONE)
   {
      db_log(db, ERR, "Error: No 'Version' entry found. Patch engine version = %s (file: %s)",
            PATCH_DB_ENGINE_VERSION, path);
      r.ok = 0;
      goto done;
   }
   version = rd_val(&r, vn);
   if (!patch_db_str_is(version, PATCH_DB_ENGINE_VERSION))
   {
      db_log(db, ERR, "Error: File version %S does not match patch engine target version %s (location: %s, file: %s)",
            version, PATCH_DB_ENGINE_VERSION, loc(y, vn, lb), path);
      r.ok = 0;
      goto done;
   }

   for (pair = ryaml_first_child(y, root); pair != RYAML_NONE && !r.oom; pair = ryaml_next_sibling(y, pair))
   {
      const patch_str_t main_key = rd_key(&r, pair);
      size_t ci;
      int entry;

      if (patch_db_str_is(main_key, "Version"))
         continue;
      if (!main_key.len)
      {
         db_log(db, ERR, "Error: Skipping empty key (location: %s, file: %s)", loc(y, pair, lb), path);
         r.ok = 0;
         continue;
      }
      if (!ryaml_is_map(y, pair))
      {
         db_log(db, ERR, "Error: Skipping key %S: expected Map, found %s (location: %s, file: %s)",
               main_key, type_name(y, pair), loc(y, pair, lb), path);
         r.ok = 0;
         continue;
      }
      if (patch_db_str_is(main_key, "Anchors"))
         continue;

      ci = container_get(db, main_key);
      if (ci == NONE)
      {
         r.oom = 1;
         break;
      }
      db->containers[ci].version = version;
      r.hash = main_key;

      for (entry = ryaml_first_child(y, pair); entry != RYAML_NONE && !r.oom; entry = ryaml_next_sibling(y, entry))
         rd_patch(&r, ci, version, entry);
   }

done:
   if (r.oom)
   {
      db_log(db, PATCH_LOG_FATAL | PATCH_LOG_USER, "Fatal Error: out of memory reading %s", path);
      r.ok = 0;
   }
   ryaml_free(cy);
   ryaml_free(y);
   return r.ok;
}

/* ------------------------------------------------------------------ */
/* applying                                                           */
/* ------------------------------------------------------------------ */

int patch_u32s_push(patch_u32s_t *a, uint32_t value)
{
   if (a->n == a->cap && !grow(&a->v, &a->cap, a->n + 1, sizeof(*a->v)))
      return 0;
   a->v[a->n++] = value;
   return 1;
}

static void put_le(uint8_t *p, uint64_t v, int bytes)
{
   int i;
   for (i = 0; i < bytes; i++)
      p[i] = (uint8_t)(v >> (8 * i));
}

static void put_be(uint8_t *p, uint64_t v, int bytes)
{
   int i;
   for (i = 0; i < bytes; i++)
      p[i] = (uint8_t)(v >> (8 * (bytes - 1 - i)));
}

static uint64_t f32_bits(double d)
{
   const float f = (float)d;
   uint32_t u;
   memcpy(&u, &f, sizeof(u));
   return u;
}

static uint64_t f64_bits(double d)
{
   uint64_t u;
   memcpy(&u, &d, sizeof(u));
   return u;
}

static int is_float_type(int type)
{
   return type == PATCH_BEF32 || type == PATCH_LEF32 || type == PATCH_BEF64 || type == PATCH_LEF64;
}

/* A configurable value set from patch_config.yml, where it is allowed */
static void set_config(const patch_db_t *db, const patch_config_value_t *cv,
      double v, double *actual)
{
   if (cv->type == PATCH_CONFIG_DOUBLE_ENUM || cv->type == PATCH_CONFIG_LONG_ENUM)
   {
      size_t i;
      for (i = cv->first_allowed; i < cv->first_allowed + cv->num_allowed; i++)
         if (db->allowed[i].value == v)
            break;
      if (i == cv->first_allowed + cv->num_allowed)
      {
         db_log(db, PATCH_LOG_ERROR, "Can't set configurable enumerated value '%S' to %f. Using default value %f",
               cv->name, v, *actual);
         return;
      }
   }
   else if (v < cv->min || v > cv->max)
   {
      db_log(db, PATCH_LOG_ERROR, "Can't set configurable range value '%S' to %f. Using default value %f",
            cv->name, v, *actual);
      return;
   }
   *actual = v;
}

static uint32_t be32_at(const uint8_t *p)
{
   return p ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3] : 0;
}

/* One patch, with the values patch_config.yml gives @t: returns how many
 * entries it added to @applied */
static size_t apply_one(patch_db_t *db, patch_u32s_t *applied, size_t pi, size_t ti,
      const patch_apply_ops_t *ops, uint32_t filesz, uint32_t min_addr)
{
   const patch_info_t *info = &db->patches[pi];
   const patch_target_t *t = &db->targets[ti];
   const size_t old = applied->n;
   double stack_values[16];
   double *values = stack_values;
   size_t k, i;
   uint32_t cave = 0;

   /* The configurable values: the defaults, then patch_config.yml's */
   if (info->num_configs > sizeof(stack_values) / sizeof(stack_values[0]))
   {
      values = (double*)malloc(info->num_configs * sizeof(*values));
      if (!values)
         return 0;
   }
   for (k = 0; k < info->num_configs; k++)
      values[k] = db->configs[info->first_config + k].value;
   for (k = t->first_override; k < t->first_override + t->num_overrides; k++)
   {
      const size_t c = db->overrides[k].config;
      set_config(db, &db->configs[c], db->overrides[k].value, &values[c - info->first_config]);
   }
   for (i = info->first_data; i < info->first_data + info->num_data; i++)
   {
      const patch_data_t *p = &db->data[i];
      double v;
      if (p->config == NONE)
         continue;
      v = values[p->config - info->first_config];
      if (is_float_type(p->type))
         db_log(db, PATCH_LOG_NOTICE, "Using configurable value (key='%S', value=%f, index=%u, hash='%S', description='%S', author='%S', patch_version='%S', file_version='%S')",
               p->original_value, v, (unsigned long)(i - info->first_data), info->hash, info->description, info->author, info->patch_version, info->version);
      else
         db_log(db, PATCH_LOG_NOTICE, "Using configurable value (key='%S', value=0x%X=%d, index=%u, hash='%S', description='%S', author='%S', patch_version='%S', file_version='%S')",
               p->original_value, f64_to_bits(v, 64), f64_to_bits(v, 64), (unsigned long)(i - info->first_data), info->hash, info->description, info->author, info->patch_version, info->version);
   }

   /* Memory first: all of it, or none and no entry at all */
   for (i = info->first_data; i < info->first_data + info->num_data; i++)
   {
      patch_data_t *p = &db->data[i];
      uint32_t addr;
      if (p->type != PATCH_ALLOC)
         continue;
      addr = ops->alloc ? ops->alloc(ops->user, p) : 0;
      if (addr && patch_u32s_push(applied, (uint32_t)(i - info->first_data)))
      {
         p->alloc_addr = addr;
         continue;
      }
      if (addr && ops->dealloc)
         ops->dealloc(ops->user, addr);
      for (k = old; k < applied->n; k++)
      {
         patch_data_t *done = &db->data[info->first_data + applied->v[k]];
         const uint32_t a = done->alloc_addr;
         done->alloc_addr = 0;
         if (ops->dealloc)
            ops->dealloc(ops->user, a);
      }
      applied->n = old;
      goto out;
   }
   for (k = old; k < applied->n; k++)
      applied->v[k] = 0xffffffffu;

   for (i = info->first_data; i < info->first_data + info->num_data; i++)
   {
      patch_data_t *p = &db->data[i];
      uint32_t offset = p->offset;
      uint32_t size = 0;
      uint32_t resval = 0xffffffffu;
      uint64_t u = p->value.u;
      double d = p->value.d;
      uint8_t *ptr;
      int in_cave;

      if (p->config != NONE)
      {
         const double v = values[p->config - info->first_config];
         if (is_float_type(p->type))
            d = v;
         else
            u = f64_to_bits(v, 64);
      }

      /* A code cave's entries go on while it has NOPs to fill */
      if (cave && be32_at(ops->guest(ops->user, cave)) != PPU_NOP)
         cave = 0;

      switch (p->type)
      {
         case PATCH_ALLOC:
            continue; /* above */
         case PATCH_BYTE:
            size = 1;
            break;
         case PATCH_LE16:
         case PATCH_BE16:
            size = 2;
            break;
         case PATCH_CODE_ALLOC:
         case PATCH_JUMP:
         case PATCH_JUMP_LINK:
         case PATCH_JUMP_FUNC:
         case PATCH_BP_EXEC:
         case PATCH_LE32:
         case PATCH_LEF32:
         case PATCH_BD32:
         case PATCH_BE32:
         case PATCH_BEF32:
            size = 4;
            break;
         case PATCH_LEF64:
         case PATCH_LE64:
         case PATCH_BD64:
         case PATCH_BE64:
         case PATCH_BEF64:
            size = 8;
            break;
         case PATCH_UTF8:
            size = (uint32_t)p->original_value.len;
            break;
         case PATCH_C_UTF8:
            size = p->original_value.len >= 0xffffffffu ? 0xffffffffu : (uint32_t)p->original_value.len + 1;
            break;
         default:
            break;
      }

      if (size && !cave && (filesz < size || offset < min_addr || offset - min_addr > filesz - size))
         continue; /* outside this segment */

      offset -= min_addr;
      ptr = ops->translate(ops->user, offset, size);
      if (!ptr && size && !cave)
         continue;

      in_cave = cave != 0;
      if (cave)
      {
         offset = cave;
         ptr = ops->guest(ops->user, cave);
         cave += 4;
      }

      switch (p->type)
      {
         case PATCH_INVALID:
         case PATCH_LOAD:
         case PATCH_ALLOC:
            continue;
         case PATCH_CODE_ALLOC:
         case PATCH_JUMP:
         case PATCH_JUMP_LINK:
         case PATCH_JUMP_FUNC:
         case PATCH_BP_EXEC:
         case PATCH_MOVE_FILE:
         case PATCH_HIDE_FILE:
            if (!ops->special || !ops->special(ops->user, p, u, offset, in_cave, &cave, &resval))
               continue;
            break;
         case PATCH_BYTE:
            *ptr = (uint8_t)u;
            break;
         case PATCH_LE16:
            put_le(ptr, u, 2);
            break;
         case PATCH_LE32:
            put_le(ptr, u, 4);
            break;
         case PATCH_LEF32:
            put_le(ptr, f32_bits(d), 4);
            break;
         case PATCH_LE64:
            put_le(ptr, u, 8);
            break;
         case PATCH_LEF64:
            put_le(ptr, f64_bits(d), 8);
            break;
         case PATCH_BE16:
            put_be(ptr, u, 2);
            break;
         case PATCH_BD32:
            put_be(ptr, u, 4);
            break;
         case PATCH_BE32:
            put_be(ptr, u, 4);
            if (offset % 4 == 0)
               resval = offset;
            break;
         case PATCH_BEF32:
            put_be(ptr, f32_bits(d), 4);
            break;
         case PATCH_BD64:
            put_be(ptr, u, 8);
            break;
         case PATCH_BE64:
            put_be(ptr, u, 8);
            if (offset % 4)
               break;
            resval = offset;
            patch_u32s_push(applied, (offset + 7) & ~3u); /* two words */
            break;
         case PATCH_BEF64:
            put_be(ptr, f64_bits(d), 8);
            break;
         case PATCH_UTF8:
            if (p->original_value.len)
               memcpy(ptr, p->original_value.s, p->original_value.len);
            break;
         case PATCH_C_UTF8:
            if (p->original_value.len)
               memcpy(ptr, p->original_value.s, p->original_value.len);
            ptr[p->original_value.len] = '\0';
            break;
      }

      /* Maybe an instruction */
      patch_u32s_push(applied, resval);
   }

out:
   if (values != stack_values)
      free(values);
   return applied->n - old;
}

/* Whether @info is on for the game, and how specifically: the target, and
 * the bucket (0 any serial and version, 1 any serial, 2 any version,
 * 3 exactly this game) */
static size_t target_for(const patch_db_t *db, const patch_info_t *info,
      const char *serial, const char *app_version, int *bucket)
{
   const size_t end = info->first_target + info->num_targets;
   size_t run = info->first_target;
   while (run < end)
   {
      const patch_str_t title = db->targets[run].title;
      size_t run_end = run, i;
      patch_str_t found_serial;
      int all_serials = 0, all_versions = 0;
      size_t found = NONE;
      found_serial.s = NULL;
      found_serial.len = 0;
      while (run_end < end && str_same(db->targets[run_end].title, title))
         run_end++;
      for (i = run; i < run_end && !found_serial.s; i++)
         if (patch_db_str_is(db->targets[i].serial, serial))
            found_serial = db->targets[i].serial;
      for (i = run; i < run_end && !found_serial.s; i++)
         if (patch_db_str_is(db->targets[i].serial, "All"))
         {
            found_serial = db->targets[i].serial;
            all_serials = 1;
         }
      if (found_serial.s)
      {
         for (i = run; i < run_end && found == NONE; i++)
            if (str_same(db->targets[i].serial, found_serial) && patch_db_str_is(db->targets[i].app_version, app_version))
               found = i;
         for (i = run; i < run_end && found == NONE; i++)
            if (str_same(db->targets[i].serial, found_serial) && patch_db_str_is(db->targets[i].app_version, "All"))
            {
               found = i;
               all_versions = 1;
            }
         if (found != NONE && db->targets[found].enabled)
         {
            *bucket = all_serials ? (all_versions ? 0 : 1) : (all_versions ? 2 : 3);
            return found;
         }
      }
      run = run_end;
   }
   return NONE;
}

static int cmp_u32(const void *a, const void *b)
{
   const uint32_t x = *(const uint32_t*)a;
   const uint32_t y = *(const uint32_t*)b;
   return x < y ? -1 : x > y;
}

void patch_db_apply(patch_db_t *db, patch_u32s_t *applied, const char *name,
      size_t name_len, const char *serial, const char *app_version,
      const patch_apply_ops_t *ops, uint32_t filesz, uint32_t min_addr)
{
   const size_t ci = patch_db_find(db, name, name_len);
   size_t count, n = 0, i, p;
   size_t *chosen;    /* patch, target and bucket of each patch that is on */
   int b;

   if (ci == NONE || !db->containers[ci].count)
      return;
   count = db->containers[ci].count;
   chosen = (size_t*)malloc(count * 3 * sizeof(*chosen));
   if (!chosen)
      return;

   for (p = db->containers[ci].first; p != NONE; p = db->patches[p].next)
   {
      int bucket = 0;
      const size_t t = target_for(db, &db->patches[p], serial, app_version, &bucket);
      if (t == NONE)
         continue;
      chosen[n * 3] = p;
      chosen[n * 3 + 1] = t;
      chosen[n * 3 + 2] = (size_t)bucket;
      n++;
   }

   /* One patch of a group, the most specific: over all calls */
   for (b = 3; b >= 0; b--)
   {
      for (i = 0; i < n; i++)
      {
         const patch_str_t group = db->patches[chosen[i * 3]].group;
         size_t g;
         if (chosen[i * 3 + 2] != (size_t)b || !group.len)
            continue;
         for (g = 0; g < db->num_groups; g++)
            if (str_same(db->groups[g], group))
               break;
         if (g < db->num_groups)
            chosen[i * 3 + 2] = NONE;
         else if (RESERVE(db, groups))
            db->groups[db->num_groups++] = group;
      }
   }

   for (b = 0; b < 4; b++)
   {
      for (i = 0; i < n; i++)
      {
         const patch_info_t *info;
         size_t added;
         if (chosen[i * 3 + 2] != (size_t)b)
            continue;
         added = apply_one(db, applied, chosen[i * 3], chosen[i * 3 + 1], ops, filesz, min_addr);
         info = &db->patches[chosen[i * 3]];
         if (added)
            db_log(db, PATCH_LOG_SUCCESS, "Applied patch (hash='%S', description='%S', author='%S', patch_version='%S', file_version='%S') (<- %u)",
                  info->hash, info->description, info->author, info->patch_version, info->version, (unsigned long)added);
      }
   }
   free(chosen);

   /* The same order every time */
   if (applied->n > 1)
      qsort(applied->v, applied->n, sizeof(*applied->v), cmp_u32);
}

void patch_db_unload(patch_db_t *db, const char *name, size_t name_len,
      const patch_apply_ops_t *ops)
{
   const size_t ci = patch_db_find(db, name, name_len);
   size_t p, i;
   if (ci == NONE)
      return;
   for (p = db->containers[ci].first; p != NONE; p = db->patches[p].next)
   {
      const patch_info_t *info = &db->patches[p];
      for (i = info->first_data; i < info->first_data + info->num_data; i++)
      {
         const uint32_t addr = db->data[i].alloc_addr;
         if (!addr)
            continue;
         db->data[i].alloc_addr = 0;
         if (ops->dealloc)
            ops->dealloc(ops->user, addr);
      }
   }
}

/* ------------------------------------------------------------------ */
/* patch_config.yml                                                   */
/* ------------------------------------------------------------------ */

/* The shortest %g that reads back as @v */
static void write_f64(ryaml_writer_t *w, double v)
{
   char buf[64];
   int prec;
   for (prec = 1; prec <= 17; prec++)
   {
      sprintf(buf, "%.*g", prec, v);
      if (strtod(buf, NULL) == v)
         break;
   }
   ryaml_writer_scalar(w, buf, strlen(buf));
}

static void wkey(ryaml_writer_t *w, patch_str_t s)
{
   ryaml_writer_key(w, s.s, s.len);
}

static const patch_config_pick_t *pick_named(const patch_config_choice_t *c, patch_str_t name)
{
   size_t i;
   for (i = 0; i < c->num_picks; i++)
      if (str_eq(name, c->picks[i].name, c->picks[i].name_len))
         return &c->picks[i];
   return NULL;
}

static void write_patch(const patch_db_t *db, const patch_info_t *info,
      const patch_config_choice_t *c, ryaml_writer_t *w)
{
   const size_t end = info->first_target + info->num_targets;
   size_t run = info->first_target, i, k;
   int dirty = 0;

   /* The values go in when one differs from its default */
   for (k = 0; k < c->num_picks; k++)
   {
      patch_str_t name;
      size_t cfg;
      name.s = c->picks[k].name;
      name.len = c->picks[k].name_len;
      cfg = config_named(db, info, name.s, name.len);
      if (cfg == NONE || db->configs[cfg].value != c->picks[k].value)
         dirty = 1;
   }
   dirty = dirty && info->num_configs;

   wkey(w, info->description);
   ryaml_writer_begin_map(w);
   while (run < end)
   {
      const patch_str_t title = db->targets[run].title;
      size_t run_end = run;
      int any = 0;
      while (run_end < end && str_same(db->targets[run_end].title, title))
         run_end++;
      for (i = run; i < run_end; i++)
      {
         const patch_target_t *t = &db->targets[i];
         if (!patch_db_str_is(t->serial, c->serial_key))
            continue;
         if (!any)
         {
            wkey(w, title);
            ryaml_writer_begin_map(w);
            ryaml_writer_key(w, c->serial_key, strlen(c->serial_key));
            ryaml_writer_begin_map(w);
            any = 1;
         }
         wkey(w, t->app_version);
         ryaml_writer_begin_map(w);
         ryaml_writer_key(w, "Enabled", 7);
         ryaml_writer_scalar(w, "true", 4);
         if (dirty)
         {
            ryaml_writer_key(w, "Configurable Values", 19);
            ryaml_writer_begin_map(w);
            for (k = info->first_config; k < info->first_config + info->num_configs; k++)
            {
               const patch_config_pick_t *pick = pick_named(c, db->configs[k].name);
               wkey(w, db->configs[k].name);
               write_f64(w, pick ? pick->value : db->configs[k].value);
            }
            for (k = 0; k < c->num_picks; k++)
            {
               if (config_named(db, info, c->picks[k].name, c->picks[k].name_len) != NONE)
                  continue;
               ryaml_writer_key(w, c->picks[k].name, c->picks[k].name_len);
               write_f64(w, c->picks[k].value);
            }
            ryaml_writer_end_map(w);
         }
         ryaml_writer_end_map(w);
      }
      if (any)
      {
         ryaml_writer_end_map(w);
         ryaml_writer_end_map(w);
      }
      run = run_end;
   }
   ryaml_writer_end_map(w);
}

static int has_target(const patch_db_t *db, const patch_info_t *info, const char *serial_key)
{
   size_t i;
   for (i = info->first_target; i < info->first_target + info->num_targets; i++)
      if (patch_db_str_is(db->targets[i].serial, serial_key))
         return 1;
   return 0;
}

int patch_db_write_config(const patch_db_t *db,
      const patch_config_choice_t *choices, size_t num_choices,
      ryaml_writer_t *w)
{
   size_t ci;
   ryaml_writer_begin_map(w);
   for (ci = 0; ci < db->num_containers; ci++)
   {
      const patch_container_t *pc = &db->containers[ci];
      int any = 0;
      size_t p;
      for (p = pc->first; p != NONE; p = db->patches[p].next)
      {
         const patch_info_t *info = &db->patches[p];
         size_t c;
         for (c = 0; c < num_choices; c++)
            if (str_eq(info->description, choices[c].description, choices[c].description_len))
               break;
         if (c == num_choices || !has_target(db, info, choices[c].serial_key))
            continue;
         if (!any)
         {
            wkey(w, pc->hash);
            ryaml_writer_begin_map(w);
            any = 1;
         }
         write_patch(db, info, &choices[c], w);
      }
      if (any)
         ryaml_writer_end_map(w);
   }
   ryaml_writer_end_map(w);
   return !ryaml_writer_error(w);
}
