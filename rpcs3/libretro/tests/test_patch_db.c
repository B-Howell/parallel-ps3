/* The patch engine (Utilities/patch_db.c): patch files read into the
 * database, with their mistakes reported and the patches around them kept;
 * patch_config.yml turning patches on and setting their values; patches
 * applied to an executable's memory in RPCS3's order (any serial, any
 * version, then exactly the game), one of a group at most, within the
 * segment, with code caves and allocations that all happen or none; and
 * patch_config.yml written back and read again. C89. */

#include "test_common.h"

#include "../../../Utilities/patch_db.h"

#include <formats/ryaml_writer.h>

static unsigned s_user_messages;
static unsigned s_errors;
static char s_last[1024];

static void log_fn(void *user, int level, const char *msg, size_t len)
{
   (void)user;
   if (level & PATCH_LOG_USER)
      s_user_messages++;
   if ((level & PATCH_LOG_LEVEL_MASK) == PATCH_LOG_ERROR)
      s_errors++;
   if (len >= sizeof(s_last))
      len = sizeof(s_last) - 1;
   memcpy(s_last, msg, len);
   s_last[len] = '\0';
}

static char *dup_text(const char *s)
{
   const size_t len = strlen(s);
   char *copy = (char*)malloc(len ? len : 1);
   memcpy(copy, s, len);
   return copy;
}

static int load(patch_db_t *db, const char *text, const char *config)
{
   return patch_db_load(db, "test.yml", dup_text(text), strlen(text),
         config, config ? strlen(config) : 0);
}

/* The executable's memory, and guest memory for code caves */
static uint8_t s_mem[0x400];
static uint8_t s_guest[0x2000];
static unsigned s_allocs, s_deallocs;
static uint32_t s_dealloc_addr;
static uint32_t s_next_alloc;

static uint8_t *translate(void *user, uint32_t offset, uint32_t size)
{
   (void)user;
   return offset <= sizeof(s_mem) && size <= sizeof(s_mem) - offset ? s_mem + offset : NULL;
}

static uint8_t *guest(void *user, uint32_t addr)
{
   (void)user;
   return addr < sizeof(s_guest) - 4 ? s_guest + addr : NULL;
}

static uint32_t alloc_fn(void *user, const patch_data_t *p)
{
   (void)user;
   (void)p;
   s_allocs++;
   if (!s_next_alloc)
      return 0;
   s_next_alloc += 0x10000;
   return s_next_alloc - 0x10000;
}

static void dealloc_fn(void *user, uint32_t addr)
{
   (void)user;
   s_deallocs++;
   s_dealloc_addr = addr;
}

static uint64_t s_jump_value;

/* A code cave at 0x1000 of guest memory, two NOPs long */
static int special(void *user, patch_data_t *p, uint64_t value, uint32_t offset,
      int in_cave, uint32_t *cave, uint32_t *resval)
{
   (void)user;
   (void)in_cave;
   if (p->type == PATCH_CODE_ALLOC)
   {
      memset(s_guest + 0x1000, 0, 16);
      s_guest[0x1000] = 0x60;
      s_guest[0x1004] = 0x60;
      p->alloc_addr = 0x1000;
      *cave = 0x1000;
      *resval = offset & ~3u;
      return 1;
   }
   if (p->type == PATCH_JUMP)
   {
      s_jump_value = value;
      *resval = offset & ~3u;
      return 1;
   }
   return 0;
}

static const patch_apply_ops_t s_ops = {
   NULL, translate, guest, alloc_fn, dealloc_fn, special
};

static uint32_t be32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static const patch_info_t *find_patch(const patch_db_t *db, const char *hash, const char *desc)
{
   const size_t c = patch_db_find(db, hash, strlen(hash));
   const size_t p = patch_db_find_patch(db, c, desc, strlen(desc));
   return p == PATCH_DB_NONE ? NULL : &db->patches[p];
}

static const char s_file[] =
   "Version: 1.2\n"
   "\n"
   "Anchors:\n"
   "  anchor_a: &anchor_a\n"
   "    - [ be32, 0x10, 0x11111111 ]\n"
   "\n"
   "PPU-abc:\n"
   "  \"Big\":\n"
   "    Games:\n"
   "      \"Game A\":\n"
   "        BLUS00001: [ 01.00, 01.01 ]\n"
   "    Author: me\n"
   "    Patch Version: 1.0\n"
   "    Notes:\n"
   "      - \"one \"\n"
   "      - \"two\"\n"
   "    Configurable Values:\n"
   "      \"Mode\":\n"
   "        Type: long_enum\n"
   "        Value: 2\n"
   "        Allowed Values:\n"
   "          \"Low\": 1\n"
   "          \"High\": 2\n"
   "      \"Scale\":\n"
   "        Type: double_range\n"
   "        Value: 0.5\n"
   "        Min: 0\n"
   "        Max: 4\n"
   "    Patch:\n"
   "      - [ be32, 0x100, 0x60000000 ]\n"
   "      - [ be16, 0x104, Mode ]\n"
   "      - [ byte, 0x106, -1 ]\n"
   "      - [ le32, 0x108, 0x01020304 ]\n"
   "      - [ bef32, 0x10c, Scale ]\n"
   "      - [ bef64, 0x110, 0.1 ]\n"
   "      - [ utf8, 0x118, ab ]\n"
   "      - [ cutf8, 0x11c, xy ]\n"
   "      - [ be64, 0x120, 0x0102030405060708 ]\n"
   "      - [ load, *anchor_a, 0x200 ]\n"
   "      - [ jump, 0x130, 0x10200 ]\n";

static void test_read_and_apply(void)
{
   patch_db_t db;
   const patch_info_t *info;
   patch_u32s_t applied;
   size_t i;
   static const char config[] =
      "PPU-abc:\n"
      "  Big:\n"
      "    Game A:\n"
      "      BLUS00001:\n"
      "        \"01.00\":\n"
      "          Enabled: true\n"
      "          Configurable Values:\n"
      "            Mode: 1\n"
      "            Scale: 9\n";

   patch_db_init(&db, log_fn, NULL);
   s_user_messages = 0;
   TEST_CHECK(load(&db, s_file, config));
   TEST_CHECK(s_user_messages == 0);
   if (s_user_messages)
      fprintf(stderr, "last message: %s\n", s_last);
   info = find_patch(&db, "PPU-abc", "Big");
   TEST_CHECK(info != NULL);
   TEST_CHECK(patch_db_find(&db, "Anchors", 7) == PATCH_DB_NONE);
   if (!info)
   {
      patch_db_free(&db);
      return;
   }
   TEST_CHECK(info->num_data == 11);
   TEST_CHECK(info->num_targets == 2);
   TEST_CHECK(info->num_configs == 2);
   TEST_CHECK(patch_db_str_is(info->notes, "one two"));
   TEST_CHECK(patch_db_str_is(info->author, "me"));
   TEST_CHECK(patch_db_str_is(info->version, "1.2"));
   TEST_CHECK(db.targets[info->first_target].enabled);
   TEST_CHECK(!db.targets[info->first_target + 1].enabled);
   TEST_CHECK(db.targets[info->first_target].num_overrides == 2);
   TEST_CHECK(db.data[info->first_data + 1].value.u == 2);
   TEST_CHECK(db.data[info->first_data + 2].value.u == ~(uint64_t)0);
   TEST_CHECK(db.data[info->first_data + 9].offset == 0x210);
   TEST_CHECK(db.data[info->first_data + 9].type == PATCH_BE32);

   memset(s_mem, 0, sizeof(s_mem));
   memset(&applied, 0, sizeof(applied));
   s_errors = 0;
   patch_db_apply(&db, &applied, "PPU-abc", 7, "BLUS00001", "01.00", &s_ops, sizeof(s_mem), 0);
   TEST_CHECK(be32(s_mem + 0x100) == 0x60000000u);
   /* Mode from patch_config.yml; Scale 9 is out of its range: the default */
   TEST_CHECK(s_mem[0x104] == 0 && s_mem[0x105] == 1);
   TEST_CHECK(s_errors == 1);
   TEST_CHECK(s_mem[0x106] == 0xff && s_mem[0x107] == 0);
   TEST_CHECK(s_mem[0x108] == 4 && s_mem[0x109] == 3 && s_mem[0x10a] == 2 && s_mem[0x10b] == 1);
   TEST_CHECK(be32(s_mem + 0x10c) == 0x3f000000u);
   TEST_CHECK(be32(s_mem + 0x110) == 0x3fb99999u && be32(s_mem + 0x114) == 0x9999999au);
   TEST_CHECK(memcmp(s_mem + 0x118, "ab\0\0", 4) == 0);
   TEST_CHECK(memcmp(s_mem + 0x11c, "xy\0", 3) == 0);
   TEST_CHECK(be32(s_mem + 0x120) == 0x01020304u && be32(s_mem + 0x124) == 0x05060708u);
   TEST_CHECK(be32(s_mem + 0x210) == 0x11111111u);
   TEST_CHECK(s_jump_value == 0x10200);
   /* What may be code, in order: be32 twice, be64 two words, the jump */
   TEST_CHECK(applied.n == 12);
   if (applied.n == 12)
   {
      TEST_CHECK(applied.v[0] == 0x100 && applied.v[1] == 0x120 && applied.v[2] == 0x124
            && applied.v[3] == 0x130 && applied.v[4] == 0x210);
      for (i = 5; i < 12; i++)
         TEST_CHECK(applied.v[i] == 0xffffffffu);
   }

   /* Another version of the game: not on */
   memset(s_mem, 0, sizeof(s_mem));
   applied.n = 0;
   patch_db_apply(&db, &applied, "PPU-abc", 7, "BLUS00001", "01.01", &s_ops, sizeof(s_mem), 0);
   TEST_CHECK(applied.n == 0 && s_mem[0x100] == 0);
   free(applied.v);
   patch_db_free(&db);
}

static const char s_priority_file[] =
   "Version: 1.2\n"
   "PPU-pri:\n"
   "  Generic:\n"
   "    Games:\n"
   "      All:\n"
   "        All: [ All ]\n"
   "    Patch:\n"
   "      - [ byte, 0x1, 0x01 ]\n"
   "  Generic grouped:\n"
   "    Group: g\n"
   "    Games:\n"
   "      All:\n"
   "        All: [ All ]\n"
   "    Patch:\n"
   "      - [ byte, 0x3, 0x09 ]\n"
   "  Specific:\n"
   "    Games:\n"
   "      Game A:\n"
   "        BLUS00001: [ All ]\n"
   "    Patch:\n"
   "      - [ byte, 0x1, 0x02 ]\n"
   "  Exact:\n"
   "    Group: g\n"
   "    Games:\n"
   "      Other title:\n"
   "        BLES00001: [ 01.00 ]\n"
   "      Game A:\n"
   "        BLUS00001: [ 01.00 ]\n"
   "    Patch:\n"
   "      - [ byte, 0x2, 0x03 ]\n"
   "  Off:\n"
   "    Games:\n"
   "      Game A:\n"
   "        BLUS00001: [ 01.00 ]\n"
   "    Patch:\n"
   "      - [ byte, 0x4, 0x04 ]\n"
   "PPU-range:\n"
   "  Range:\n"
   "    Games:\n"
   "      Game A:\n"
   "        BLUS00001: [ All ]\n"
   "    Patch:\n"
   "      - [ byte, 0x50, 0x05 ]\n"
   "      - [ byte, 0x70, 0x06 ]\n"
   "      - [ be32, 0x5e, 0x07070707 ]\n"
   "      - [ byte, 0x3f, 0x08 ]\n";

static const char s_priority_config[] =
   "PPU-pri:\n"
   "  Generic:\n"
   "    All:\n"
   "      All:\n"
   "        All: true\n"
   "  Generic grouped:\n"
   "    All:\n"
   "      All:\n"
   "        All:\n"
   "          Enabled: true\n"
   "  Specific:\n"
   "    Game A:\n"
   "      BLUS00001:\n"
   "        All:\n"
   "          Enabled: true\n"
   "  Exact:\n"
   "    Game A:\n"
   "      BLUS00001:\n"
   "        \"01.00\":\n"
   "          Enabled: true\n"
   "  Off:\n"
   "    Game A:\n"
   "      BLUS00001:\n"
   "        \"01.00\":\n"
   "          Enabled: false\n"
   "PPU-range:\n"
   "  Range:\n"
   "    Game A:\n"
   "      BLUS00001:\n"
   "        All: true\n";

static void test_priority_groups_ranges(void)
{
   patch_db_t db;
   patch_u32s_t applied;

   patch_db_init(&db, log_fn, NULL);
   s_user_messages = 0;
   TEST_CHECK(load(&db, s_priority_file, s_priority_config));
   TEST_CHECK(s_user_messages == 0);

   memset(s_mem, 0, sizeof(s_mem));
   memset(&applied, 0, sizeof(applied));
   patch_db_apply(&db, &applied, "PPU-pri", 7, "BLUS00001", "01.00", &s_ops, sizeof(s_mem), 0);
   /* Any serial first, so the game's own wins */
   TEST_CHECK(s_mem[1] == 0x02);
   /* The group's most specific patch, and no other */
   TEST_CHECK(s_mem[2] == 0x03);
   TEST_CHECK(s_mem[3] == 0);
   TEST_CHECK(s_mem[4] == 0);
   TEST_CHECK(applied.n == 3);

   /* The group was applied: not again */
   memset(s_mem, 0, sizeof(s_mem));
   applied.n = 0;
   patch_db_apply(&db, &applied, "PPU-pri", 7, "BLUS00001", "01.00", &s_ops, sizeof(s_mem), 0);
   TEST_CHECK(s_mem[1] == 0x02 && s_mem[2] == 0 && s_mem[3] == 0);

   /* Another game: the patches for all */
   memset(s_mem, 0, sizeof(s_mem));
   applied.n = 0;
   patch_db_apply(&db, &applied, "PPU-pri", 7, "BLES00002", "01.00", &s_ops, sizeof(s_mem), 0);
   TEST_CHECK(s_mem[1] == 0x01 && s_mem[2] == 0);

   /* A segment of 0x20 bytes from 0x40: what lies outside is left out */
   memset(s_mem, 0, sizeof(s_mem));
   applied.n = 0;
   patch_db_apply(&db, &applied, "PPU-range", 9, "BLUS00001", "01.00", &s_ops, 0x20, 0x40);
   TEST_CHECK(s_mem[0x10] == 0x05);
   TEST_CHECK(s_mem[0x30] == 0 && s_mem[0x1e] == 0 && s_mem[0x3f] == 0 && s_mem[0xff] == 0);
   TEST_CHECK(applied.n == 1);

   free(applied.v);
   patch_db_free(&db);
}

static const char s_memory_file[] =
   "Version: 1.2\n"
   "PPU-mem:\n"
   "  Allocs:\n"
   "    Games:\n"
   "      Game A:\n"
   "        BLUS00001: [ All ]\n"
   "    Patch:\n"
   "      - [ alloc, 0x20000003, 0x100 ]\n"
   "      - [ byte, 0x0, 0x05 ]\n"
   "      - [ alloc, 0x30000003, 0x100 ]\n"
   "  Cave:\n"
   "    Games:\n"
   "      Game A:\n"
   "        BLUS00001: [ All ]\n"
   "    Patch:\n"
   "      - [ calloc, 0x2fc, 2 ]\n"
   "      - [ be32, 0x300, 0xAAAAAAAA ]\n"
   "      - [ be32, 0x304, 0xBBBBBBBB ]\n"
   "      - [ be32, 0x308, 0xCCCCCCCC ]\n";

static const char s_memory_config[] =
   "PPU-mem:\n"
   "  Allocs:\n"
   "    Game A:\n"
   "      BLUS00001:\n"
   "        All: true\n"
   "  Cave:\n"
   "    Game A:\n"
   "      BLUS00001:\n"
   "        All: true\n";

static void test_memory(void)
{
   patch_db_t db;
   patch_u32s_t applied;
   const patch_info_t *info;

   patch_db_init(&db, log_fn, NULL);
   TEST_CHECK(load(&db, s_memory_file, s_memory_config));
   info = find_patch(&db, "PPU-mem", "Allocs");
   TEST_CHECK(info != NULL);
   if (!info)
   {
      patch_db_free(&db);
      return;
   }

   memset(s_mem, 0, sizeof(s_mem));
   memset(s_guest, 0, sizeof(s_guest));
   memset(&applied, 0, sizeof(applied));
   patch_u32s_push(&applied, 7);
   s_allocs = s_deallocs = 0;
   s_next_alloc = 0x20000000;
   patch_db_apply(&db, &applied, "PPU-mem", 7, "BLUS00001", "01.00", &s_ops, sizeof(s_mem), 0);
   /* Both allocated, before the rest: the byte written */
   TEST_CHECK(s_allocs == 2);
   TEST_CHECK(db.data[info->first_data].alloc_addr == 0x20000000);
   TEST_CHECK(db.data[info->first_data + 2].alloc_addr == 0x20010000);
   TEST_CHECK(s_mem[0] == 0x05);

   /* The code cave: two of the three words go into it, the third where it
    * is, as the cave has no NOP left */
   TEST_CHECK(be32(s_guest + 0x1000) == 0xAAAAAAAAu);
   TEST_CHECK(be32(s_guest + 0x1004) == 0xBBBBBBBBu);
   TEST_CHECK(be32(s_mem + 0x300) == 0 && be32(s_mem + 0x304) == 0);
   TEST_CHECK(be32(s_mem + 0x308) == 0xCCCCCCCCu);
   /* 7, calloc, the third word, the cave's words; the allocations and the
    * byte, not code */
   TEST_CHECK(applied.n == 8);
   if (applied.n == 8)
   {
      TEST_CHECK(applied.v[0] == 7 && applied.v[1] == 0x2fc && applied.v[2] == 0x308
            && applied.v[3] == 0x1000 && applied.v[4] == 0x1004);
   }

   /* Unloaded: everything allocated given back */
   s_deallocs = 0;
   patch_db_unload(&db, "PPU-mem", 7, &s_ops);
   TEST_CHECK(s_deallocs == 3);
   TEST_CHECK(db.data[info->first_data].alloc_addr == 0);
   free(applied.v);
   patch_db_free(&db);
}

/* An allocator with one block to give */
static unsigned s_blocks_left;

static uint32_t alloc_once(void *user, const patch_data_t *p)
{
   (void)user;
   (void)p;
   s_allocs++;
   if (!s_blocks_left)
      return 0;
   s_blocks_left--;
   return 0x20000000;
}

static void test_alloc_failure(void)
{
   static const patch_apply_ops_t ops = {
      NULL, translate, guest, alloc_once, dealloc_fn, special
   };
   patch_db_t db;
   patch_u32s_t applied;
   const patch_info_t *info;

   patch_db_init(&db, log_fn, NULL);
   TEST_CHECK(load(&db, s_memory_file, s_memory_config));
   info = find_patch(&db, "PPU-mem", "Allocs");
   TEST_CHECK(info != NULL);
   if (!info)
   {
      patch_db_free(&db);
      return;
   }

   memset(s_mem, 0, sizeof(s_mem));
   memset(&applied, 0, sizeof(applied));
   patch_u32s_push(&applied, 7);
   s_blocks_left = 1;
   s_allocs = s_deallocs = 0;
   s_dealloc_addr = 0;
   patch_db_apply(&db, &applied, "PPU-mem", 7, "BLUS00001", "01.00", &ops, sizeof(s_mem), 0);
   TEST_CHECK(s_allocs == 2);
   TEST_CHECK(s_deallocs == 1 && s_dealloc_addr == 0x20000000);
   TEST_CHECK(db.data[info->first_data].alloc_addr == 0);
   /* Nothing of Allocs: the 7 that was there and the cave's patch */
   TEST_CHECK(s_mem[0] == 0);
   TEST_CHECK(applied.n == 5);
   if (applied.n)
      TEST_CHECK(applied.v[0] == 7);
   free(applied.v);
   patch_db_free(&db);
}

static const char s_errors_file[] =
   "Version: 1.2\n"
   "PPU-err:\n"
   "  Bad serial:\n"
   "    Games:\n"
   "      G:\n"
   "        BLUS1: [ 01.00 ]\n"
   "    Patch:\n"
   "      - [ be32, 0x10, 1 ]\n"
   "  Bad version:\n"
   "    Games:\n"
   "      G:\n"
   "        BLUS00001: [ 1.0, 01.00, 01.00 ]\n"
   "    Patch:\n"
   "      - [ be32, 0x10, 1 ]\n"
   "  Bad entries:\n"
   "    Games:\n"
   "      G:\n"
   "        BLUS00001: [ 01.00 ]\n"
   "    Patch:\n"
   "      - [ nope, 0x10, 1 ]\n"
   "      - [ be32, 16, 1 ]\n"
   "      - [ byte, 0x10, 300 ]\n"
   "      - [ byte, 0x11, -128 ]\n"
   "      - [ byte, 0x12, -129 ]\n"
   "      - [ 7, 0x13, 1 ]\n"
   "      - [ be32, 0x100000000, 1 ]\n"
   "  Bad values:\n"
   "    Configurable Values:\n"
   "      \"Mode\":\n"
   "        Type: long_enum\n"
   "        Value: 3\n"
   "        Allowed Values:\n"
   "          \"Low\": 1\n"
   "          \"Low\": 2\n"
   "    Patch:\n"
   "      - [ byte, 0x0, Mode ]\n";

static void test_errors(void)
{
   patch_db_t db;
   const patch_info_t *info;

   patch_db_init(&db, log_fn, NULL);

   /* Another engine version: nothing read */
   s_user_messages = 0;
   TEST_CHECK(!load(&db, "Version: 1.1\nPPU-x:\n  P:\n    Patch:\n      - [ byte, 0x0, 1 ]\n", NULL));
   TEST_CHECK(s_user_messages == 1 && strstr(s_last, "does not match") != NULL);
   TEST_CHECK(patch_db_find(&db, "PPU-x", 5) == PATCH_DB_NONE);
   s_user_messages = 0;
   TEST_CHECK(!load(&db, "PPU-x:\n  P:\n    Author: a\n", NULL));
   TEST_CHECK(s_user_messages == 1);
   s_user_messages = 0;
   TEST_CHECK(!load(&db, "Version: 1.2\nPPU-x: [ 1, 2\n", NULL));
   TEST_CHECK(s_user_messages == 1 && strstr(s_last, "cannot read line 2") != NULL);

   /* Mistakes reported, the rest read */
   s_user_messages = 0;
   TEST_CHECK(!load(&db, s_errors_file, NULL));
   /* the serial; two versions; type, offset, 300, -129, 0x100000000 and
    * the patch node; the label twice, too few allowed values, the value
    * not among them */
   TEST_CHECK(s_user_messages == 12);
   info = find_patch(&db, "PPU-err", "Bad serial");
   TEST_CHECK(info && info->num_targets == 0 && info->num_data == 1);
   info = find_patch(&db, "PPU-err", "Bad version");
   TEST_CHECK(info && info->num_targets == 1);
   info = find_patch(&db, "PPU-err", "Bad entries");
   TEST_CHECK(info && info->num_data == 2);
   if (info && info->num_data == 2)
   {
      TEST_CHECK(db.data[info->first_data].value.u == (uint64_t)0 - 128);
      TEST_CHECK(db.data[info->first_data + 1].type == PATCH_BYTE);
   }
   info = find_patch(&db, "PPU-err", "Bad values");
   TEST_CHECK(info && info->num_configs == 1);
   if (info && info->num_configs == 1)
      TEST_CHECK(db.configs[info->first_config].num_allowed == 1);
   patch_db_free(&db);
}

static void test_versions(void)
{
   patch_db_t db;
   const patch_info_t *info;
   static const char v1[] = "Version: 1.2\nPPU-v:\n  P:\n    Patch Version: 1.0\n    Patch:\n      - [ byte, 0x0, 1 ]\n";
   static const char v2[] = "Version: 1.2\nPPU-v:\n  P:\n    Patch Version: 2.0\n    Patch:\n      - [ byte, 0x0, 2 ]\n";
   static const char v15[] = "Version: 1.2\nPPU-v:\n  P:\n    Patch Version: 1.5\n    Patch:\n      - [ byte, 0x0, 3 ]\n";
   patch_str_t a, b;
   int ok;

   patch_db_init(&db, log_fn, NULL);
   TEST_CHECK(load(&db, v1, NULL));
   TEST_CHECK(load(&db, v2, NULL));
   s_user_messages = 0;
   TEST_CHECK(load(&db, v15, NULL));
   TEST_CHECK(s_user_messages == 1 && strstr(s_last, "higher or equal") != NULL);
   info = find_patch(&db, "PPU-v", "P");
   TEST_CHECK(info && patch_db_str_is(info->patch_version, "2.0"));
   if (info)
      TEST_CHECK(db.data[info->first_data].value.u == 2);
   TEST_CHECK(db.containers[patch_db_find(&db, "PPU-v", 5)].count == 1);
   patch_db_free(&db);

   a.s = "1.10";
   a.len = 4;
   b.s = "1.9";
   b.len = 3;
   TEST_CHECK(patch_db_compare_versions(a, b, &ok) == 1 && ok);
   TEST_CHECK(patch_db_compare_versions(b, a, &ok) == -1 && ok);
   b.s = "1.10.0";
   b.len = 6;
   TEST_CHECK(patch_db_compare_versions(a, b, &ok) == 0 && ok);
   b.s = "1.x";
   b.len = 3;
   TEST_CHECK(patch_db_compare_versions(a, b, &ok) == -1 && !ok);

   TEST_CHECK(patch_db_type_from_name("calloc", 6) == PATCH_CODE_ALLOC);
   TEST_CHECK(patch_db_type_from_name("7", 1) == PATCH_BYTE);
   TEST_CHECK(patch_db_type_from_name("0x18", 4) == PATCH_HIDE_FILE);
   TEST_CHECK(patch_db_type_from_name("25", 2) == PATCH_INVALID);
   TEST_CHECK(patch_db_type_from_name("0x", 2) == PATCH_INVALID);
   TEST_CHECK(patch_db_type_from_name("", 0) == PATCH_INVALID);
   TEST_CHECK(patch_db_type_from_name("be3", 3) == PATCH_INVALID);
}

/* patch_config.yml written for a choice, then read with the patches */
static void test_write_config(void)
{
   patch_db_t db, again;
   ryaml_writer_t *w;
   const char *text;
   size_t len = 0;
   char *copy;
   const patch_info_t *info;
   patch_config_pick_t pick;
   patch_config_choice_t choice;

   patch_db_init(&db, log_fn, NULL);
   TEST_CHECK(load(&db, s_file, NULL));

   pick.name = "Mode";
   pick.name_len = 4;
   pick.value = 1;
   choice.description = "Big";
   choice.description_len = 3;
   choice.serial_key = "BLUS00001";
   choice.picks = &pick;
   choice.num_picks = 1;

   w = ryaml_writer_new(NULL, NULL);
   TEST_CHECK(patch_db_write_config(&db, &choice, 1, w));
   text = ryaml_writer_memory(w, &len);
   TEST_CHECK(text && strstr(text, "Configurable Values") && strstr(text, "Scale: 0.5"));
   copy = (char*)malloc(len + 1);
   memcpy(copy, text, len);
   copy[len] = '\0';
   ryaml_writer_free(w);

   patch_db_init(&again, log_fn, NULL);
   TEST_CHECK(load(&again, s_file, copy));
   info = find_patch(&again, "PPU-abc", "Big");
   TEST_CHECK(info != NULL);
   if (info)
   {
      const patch_target_t *t = &again.targets[info->first_target];
      TEST_CHECK(t[0].enabled && t[1].enabled);
      TEST_CHECK(t[0].num_overrides == 2);
      TEST_CHECK(again.overrides[t[0].first_override].value == 1);
      TEST_CHECK(again.overrides[t[0].first_override + 1].value == 0.5);
   }
   patch_db_free(&again);
   free(copy);

   /* The defaults: no values written; another serial: nothing */
   pick.value = 2;
   w = ryaml_writer_new(NULL, NULL);
   TEST_CHECK(patch_db_write_config(&db, &choice, 1, w));
   text = ryaml_writer_memory(w, &len);
   TEST_CHECK(text && strstr(text, "Enabled") && !strstr(text, "Configurable Values"));
   ryaml_writer_free(w);
   choice.serial_key = "BLES00001";
   w = ryaml_writer_new(NULL, NULL);
   TEST_CHECK(patch_db_write_config(&db, &choice, 1, w));
   text = ryaml_writer_memory(w, &len);
   TEST_CHECK(!text || !strstr(text, "PPU-abc"));
   ryaml_writer_free(w);
   patch_db_free(&db);
}

int main(void)
{
   test_read_and_apply();
   test_priority_groups_ranges();
   test_memory();
   test_alloc_failure();
   test_errors();
   test_versions();
   test_write_config();
   if (test_failures)
   {
      fprintf(stderr, "test_patch_db: %d failures\n", test_failures);
      return 1;
   }
   printf("test_patch_db: ok\n");
   return 0;
}
