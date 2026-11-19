#ifndef PATCH_DB_H__
#define PATCH_DB_H__

/* RPCS3's game patches (patch.yml and the files like it), read with
 * libretro-common's ryaml into flat arrays, and applied to an executable's
 * memory. C89.
 *
 * A file is read where it lies: the database keeps its text, and the
 * strings of the patches point into it; only what YAML spells another way
 * (escaped quotes, block scalars, notes in pieces) is copied, into the
 * database's own blocks. Everything is found by index, as the arrays grow
 * while files are read; nothing points into them from outside until the
 * reading is done.
 *
 * What only the emulator can do (allocating guest memory, writing branches,
 * breakpoints, file mounts) is the caller's, through patch_apply_ops; the
 * values written into memory, the ranges and the order are this file's. */

#include <stddef.h>
#include <stdint.h>

#include <formats/ryaml_writer.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The patch engine version files are written for (the "Version" key) */
#define PATCH_DB_ENGINE_VERSION "1.2"

#ifdef __cplusplus
#define PATCH_DB_NONE (static_cast<size_t>(-1))
#else
#define PATCH_DB_NONE ((size_t)-1)
#endif

/* As RPCS3 numbers them: a file may name a type by its number too */
enum patch_db_type
{
   PATCH_INVALID = 0,
   PATCH_LOAD,
   PATCH_ALLOC,      /* allocate memory at the address (zeroed, executable) */
   PATCH_CODE_ALLOC, /* a code cave somewhere, branched to from the address */
   PATCH_JUMP,       /* a 32-bit branch instruction (PPU) */
   PATCH_JUMP_LINK,  /* the same, setting the link register */
   PATCH_JUMP_FUNC,  /* a branch to an exported function (PPU) */
   PATCH_BYTE,
   PATCH_LE16,
   PATCH_LE32,
   PATCH_LE64,
   PATCH_LEF32,
   PATCH_LEF64,
   PATCH_BE16,
   PATCH_BE32,
   PATCH_BD32,       /* be32 that is data, not code */
   PATCH_BE64,
   PATCH_BD64,       /* be64 that is data, not code */
   PATCH_BEF32,
   PATCH_BEF64,
   PATCH_BP_EXEC,    /* an execution breakpoint */
   PATCH_UTF8,       /* text, not terminated */
   PATCH_C_UTF8,     /* text, terminated */
   PATCH_MOVE_FILE,
   PATCH_HIDE_FILE,
   PATCH_TYPE_COUNT
};

enum patch_db_config_type
{
   PATCH_CONFIG_DOUBLE_RANGE = 0,
   PATCH_CONFIG_DOUBLE_ENUM,
   PATCH_CONFIG_LONG_RANGE,
   PATCH_CONFIG_LONG_ENUM
};

/* The protection of an alloc or code_alloc: the address's low bits */
enum patch_db_protection
{
   PATCH_PROT_WX = 0,
   PATCH_PROT_RO = 1,
   PATCH_PROT_RX = 2,
   PATCH_PROT_RW = 3,
   PATCH_PROT_MASK = 3
};

/* Levels of the messages; PATCH_LOG_USER marks one for the person too
 * (what RPCS3 listed in its patch manager), beside the log */
enum
{
   PATCH_LOG_FATAL = 0,
   PATCH_LOG_ERROR,
   PATCH_LOG_WARNING,
   PATCH_LOG_NOTICE,
   PATCH_LOG_SUCCESS,
   PATCH_LOG_QUIET,             /* for the person only, not the log */
   PATCH_LOG_LEVEL_MASK = 7,
   PATCH_LOG_USER = 8
};

typedef void (*patch_db_log_t)(void *user, int level, const char *msg, size_t len);

typedef struct patch_str
{
   const char *s;
   size_t len;
} patch_str_t;

typedef struct patch_data
{
   int type;
   uint32_t offset;
   patch_str_t original_offset; /* the address as written; a path for files */
   patch_str_t original_value;  /* the value as written */
   union
   {
      uint64_t u;
      double d;
   } value;
   size_t config;               /* the patch's configurable value named as
                                 * the value (an index of configs), or NONE */
   uint32_t alloc_addr;         /* what alloc/code_alloc allocated, or 0 */
} patch_data_t;

typedef struct patch_allowed_value
{
   patch_str_t label;
   double value;
} patch_allowed_value_t;

typedef struct patch_config_value
{
   patch_str_t name;
   double value;
   double min;
   double max;
   int type;
   size_t first_allowed;        /* in allowed */
   size_t num_allowed;
} patch_config_value_t;

/* A value patch_config.yml sets, for the configurable value of the patch
 * with the same name */
typedef struct patch_override
{
   size_t config;               /* an index of configs */
   double value;
} patch_override_t;

/* One app version of one serial of one title the patch is for; a patch's
 * targets lie by title, then serial, as the file has them */
typedef struct patch_target
{
   patch_str_t title;
   patch_str_t serial;
   patch_str_t app_version;
   int enabled;                 /* patch_config.yml turned it on */
   size_t first_override;       /* in overrides */
   size_t num_overrides;
} patch_target_t;

typedef struct patch_info
{
   patch_str_t description;
   patch_str_t hash;
   patch_str_t version;         /* the file's */
   patch_str_t patch_version;
   patch_str_t group;
   patch_str_t author;
   patch_str_t notes;
   patch_str_t source_path;
   size_t first_data, num_data;       /* in data */
   size_t first_target, num_targets;  /* in targets */
   size_t first_config, num_configs;  /* in configs, the defaults */
   size_t next;                 /* the container's next patch, or NONE */
} patch_info_t;

/* The patches of one executable (its hash), in the order read */
typedef struct patch_container
{
   patch_str_t hash;
   patch_str_t version;
   size_t first;                /* in patches, or NONE */
   size_t last;
   size_t count;
} patch_container_t;

typedef struct patch_db
{
   patch_container_t *containers;
   size_t num_containers, cap_containers;
   patch_info_t *patches;
   size_t num_patches, cap_patches;
   patch_data_t *data;
   size_t num_data, cap_data;
   patch_target_t *targets;
   size_t num_targets, cap_targets;
   patch_config_value_t *configs;
   size_t num_configs, cap_configs;
   patch_allowed_value_t *allowed;
   size_t num_allowed, cap_allowed;
   patch_override_t *overrides;
   size_t num_overrides, cap_overrides;

   /* containers by hash: index + 1, 0 for none; a power of two */
   size_t *table;
   size_t table_size;

   /* the texts read, and the blocks strings were copied to */
   char **blocks;
   size_t num_blocks, cap_blocks;
   char *block_pos;
   size_t block_left;

   /* the groups of which a patch was applied: one of each at most */
   patch_str_t *groups;
   size_t num_groups, cap_groups;

   patch_db_log_t log;
   void *log_user;
} patch_db_t;

/* Empty; messages go to @log (may be NULL) */
void patch_db_init(patch_db_t *db, patch_db_log_t log, void *log_user);
void patch_db_free(patch_db_t *db);

/* Reads a patch file and adds its patches: a patch already there (the same
 * executable and description) is replaced only by a higher patch version.
 * @text is malloc'd and the database's from now on, read or not (NULL
 * with @len 0 for an empty file). @config is patch_config.yml's text,
 * borrowed for the call, NULL for none (importing): which patches are on,
 * and their values. Returns 0 when anything was wrong (the rest is read
 * all the same), or when the file cannot be read at all. */
int patch_db_load(patch_db_t *db, const char *path, char *text, size_t len,
      const char *config, size_t config_len);

/* The container of an executable, or NONE */
size_t patch_db_find(const patch_db_t *db, const char *hash, size_t len);

/* The patch of a container with this description, or NONE */
size_t patch_db_find_patch(const patch_db_t *db, size_t container,
      const char *description, size_t len);

/* A type by its name ("be32", "calloc") or number, or PATCH_INVALID */
int patch_db_type_from_name(const char *s, size_t len);
const char *patch_db_type_name(int type);

/* Whether "All" or a serial/app version of a target is @s */
int patch_db_str_is(patch_str_t str, const char *s);

/* Version strings of digits and dots: 1, 0 or -1, and *ok (0 when either
 * is something else, then -1) */
int patch_db_compare_versions(patch_str_t a, patch_str_t b, int *ok);

/* Grown as patches apply: the addresses of what was written that may be
 * code (UINT32_MAX for what is not), for the PPU analyser */
typedef struct patch_u32s
{
   uint32_t *v;
   size_t n, cap;
} patch_u32s_t;

int patch_u32s_push(patch_u32s_t *a, uint32_t value);

struct patch_apply_ops
{
   void *user;
   /* The executable's memory at @offset (from the segment's start), or
    * NULL; @size may be 0 */
   uint8_t *(*translate)(void *user, uint32_t offset, uint32_t size);
   /* Guest memory at @addr as written (a code cave's) */
   uint8_t *(*guest)(void *user, uint32_t addr);
   /* An alloc entry: map and allocate it; its address, or 0 */
   uint32_t (*alloc)(void *user, const patch_data_t *p);
   /* Gives back what alloc or code_alloc allocated */
   void (*dealloc)(void *user, uint32_t addr);
   /* code_alloc, jump, jump_link, jump_func, bp_exec, move_file and
    * hide_file: 1 with *resval set (UINT32_MAX for no code), or 0 to skip
    * the entry. @value is the entry's (or its configurable value's);
    * @offset is where it is (in a code cave when @in_cave); code_alloc sets
    * p->alloc_addr and *cave, where the next entries go */
   int (*special)(void *user, patch_data_t *p, uint64_t value, uint32_t offset,
         int in_cave, uint32_t *cave, uint32_t *resval);
};

typedef struct patch_apply_ops patch_apply_ops_t;

/* Applies the patches on for executable @name to the game @serial at
 * @app_version: patches for "All" serials first, then for all versions,
 * then for exactly this one, so the specific ones win; one patch of a group
 * at most, the most specific, over every call. Entries outside
 * [@min_addr, @min_addr + @filesz) are left out. Appends to @applied. */
void patch_db_apply(patch_db_t *db, patch_u32s_t *applied, const char *name,
      size_t name_len, const char *serial, const char *app_version,
      const patch_apply_ops_t *ops, uint32_t filesz, uint32_t min_addr);

/* Gives back the memory the patches of @name allocated */
void patch_db_unload(patch_db_t *db, const char *name, size_t name_len,
      const patch_apply_ops_t *ops);

/* A patch to turn on in patch_config.yml: every executable's patch with
 * this description, for @serial_key ("All", or the game's serial), with
 * the values picked by name (the rest their defaults) */
typedef struct patch_config_pick
{
   const char *name;
   size_t name_len;
   double value;
} patch_config_pick_t;

typedef struct patch_config_choice
{
   const char *description;
   size_t description_len;
   const char *serial_key;
   const patch_config_pick_t *picks;
   size_t num_picks;
} patch_config_choice_t;

/* patch_config.yml for these choices, into @w. Returns 0 when the writer
 * failed. */
int patch_db_write_config(const patch_db_t *db,
      const patch_config_choice_t *choices, size_t num_choices,
      ryaml_writer_t *w);

#ifdef __cplusplus
}
#endif

#endif
