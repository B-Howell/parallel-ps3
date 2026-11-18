/* Game text patches: lines of a settings text built into a game's
 * executable, rewritten in the game's memory as the executable loads
 * (game_text_patch.c). The Patch Manager turns them on before the game
 * boots; the PPU loader applies them. The text is found by its bytes, not
 * by an address, so every version of a game that carries it is patched. */

#ifndef GAME_TEXT_PATCH_H
#define GAME_TEXT_PATCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAME_TEXT_PATCH_MAX     8
#define GAME_TEXT_PATCH_TEXT    64

/* No edit on. Only while no game runs. */
void game_text_patch_clear(void);

/* One more edit on: in the zero-terminated text that holds @anchor, the line
 * "@victim = ..." becomes "@key = @value", padded with spaces before the '='
 * to the line's length, so the text keeps its size. The victim is a key the
 * game does not read, whose line has room. Only while no game runs. 0 when
 * there is no room or a text is too long. */
int game_text_patch_add(const char *anchor, const char *victim, const char *key, unsigned long value);

/* How many edits are on */
int game_text_patch_count(void);

/* Every edit applied to the memory @mem, @len bytes: an executable's
 * segment. How many lines changed. */
int game_text_patch_apply(char *mem, size_t len);

/* In @text (@len bytes, a line per '\n'), the first line that starts with
 * "@victim" then blanks or '=' rewritten as "@key", spaces, "= @value",
 * filling the line to its old length. 1 when it changed; 0 when there is no
 * such line or "@key = @value" does not fit in it. */
int game_text_patch_replace_line(char *text, size_t len, const char *victim, const char *key, unsigned long value);

#ifdef __cplusplus
}
#endif

#endif
