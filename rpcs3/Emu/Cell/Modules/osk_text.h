/* The text an on-screen keyboard dialog serves to the game
 * (cellOskDialogGetInputText and its kin) - with no lock.
 *
 * Each text set is written whole into a node of its own, taken from a
 * fixed pool, and published by swapping the pointer; the node it replaces
 * is retired (retro_retire) and back in the pool once no reader is in. A
 * reader enters, reads the text published in place, and leaves. Two
 * threads setting it at once each publish a whole text, the later one
 * kept; a reader never sees half of one. A set waits for a node only when
 * readers hold back every one retired, until they leave. Nothing is
 * allocated. C89. */

#ifndef OSK_TEXT_H
#define OSK_TEXT_H

#include <stdint.h>
#include <retro_atomic.h>
#include <queues/retro_retire.h>

#ifdef __cplusplus
extern "C" {
#endif

/* As CELL_OSKDIALOG_STRING_SIZE */
#define OSK_TEXT_CHARS 512
/* The one published, one being set by each of two threads at once, and
 * some retired while a reader is in */
#define OSK_TEXT_NODES 8

struct osk_text;

typedef struct osk_text_node
{
   mpsc_stack_node_t  link;        /* first: retired as this */
   struct osk_text   *owner;
   retro_atomic_int_t used;
   uint16_t           text[OSK_TEXT_CHARS];
} osk_text_node_t;

typedef struct osk_text
{
   retro_atomic_ptr_t current;     /* osk_text_node_t*, never NULL once set up */
   retro_retire_t     retire;
   retro_atomic_int_t freed;       /* bumped as a node is back in the pool */
   osk_text_node_t    nodes[OSK_TEXT_NODES];
} osk_text_t;

/* Empty text published */
void osk_text_init(osk_text_t *t);

/* @src's first @n characters (up to the first 0) published, the rest 0;
 * NULL (or @n 0) publishes the empty text */
void osk_text_set(osk_text_t *t, const uint16_t *src, unsigned n);

/* The text published, OSK_TEXT_CHARS long and 0-ended, valid until the
 * leave */
const uint16_t *osk_text_enter(osk_text_t *t);
void osk_text_leave(osk_text_t *t);

#ifdef __cplusplus
}
#endif

#endif
