#pragma once

/* Calls run on one owner thread: the data a thread owns is touched by that
 * thread alone, so it takes no lock, and another thread that needs it run
 * hands the call over and waits for the owner to have run it.
 *
 * A caller's record lives on its own stack: it is pushed on a lock-free
 * mpsc_stack (nothing allocated), the owner is woken through
 * retro_waitaddr, and the caller waits on the record's own word. The owner
 * takes the calls up in the order they were made (owner_calls_run), at
 * the top of each round of its loop, and waits on the wake word between
 * rounds (owner_calls_wake_word) with whatever else it waits on.
 *
 * When the owner stops it closes the calls (owner_calls_close): those made
 * are run, and one made after runs on the caller's own thread, since
 * nothing else runs the owner's part any more. The count of callers and the
 * closed bit are one word, so a caller and the close cannot miss each
 * other. */

#include <retro_atomic.h>
#include <queues/mpsc_stack.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OWNER_CALLS_CLOSED (1 << 30)

typedef struct owner_call
{
   mpsc_stack_node_t node; /* first: the stack's link */
   void (*run)(void *ctx);
   void *ctx;
   retro_atomic_int_t done;
} owner_call_t;

typedef struct owner_calls
{
   mpsc_stack_t calls;
   retro_atomic_int_t wake;    /* bumped and notified: a call made, or a caller gone */
   retro_atomic_int_t callers; /* callers in a call; | OWNER_CALLS_CLOSED once closed */
   void (*waker)(void *ctx);   /* NULL, or also called as the owner is woken */
   void *waker_ctx;
} owner_calls_t;

void owner_calls_init(owner_calls_t *calls);

/* For an owner that waits on something other than the wake word (a
 * poll(), say): @waker(@ctx) is called with every wake, after the word is
 * bumped. Set before any call is made */
void owner_calls_set_waker(owner_calls_t *calls, void (*waker)(void *ctx), void *ctx);

/* Any thread but the owner: @run(@ctx) on the owner, waited for; on this
 * thread when the calls are closed */
void owner_calls_call(owner_calls_t *calls, void (*run)(void *ctx), void *ctx);

/* The owner: the calls made so far, in order */
void owner_calls_run(owner_calls_t *calls);

/* Wakes the owner: for another hand-over of its own, or its stop */
void owner_calls_wake(owner_calls_t *calls);

/* The word the owner waits on between rounds, and its value to wait on:
 * read before owner_calls_run, so a call made after wakes it */
retro_atomic_int_t *owner_calls_wake_word(owner_calls_t *calls);

/* The owner, stopping: no more calls; those made are run, until no caller
 * is left in one */
void owner_calls_close(owner_calls_t *calls);

#ifdef __cplusplus
}

#include <optional>
#include <type_traits>
#include <utility>

/* C++: @f run on the owner and its result given back; on the owner itself
 * (@on_owner) at once */
template <typename F>
auto owner_calls_on(owner_calls_t *calls, bool on_owner, F&& f) -> decltype(f())
{
   using result_t = decltype(f());

   if (on_owner)
      return f();

   struct call_t
   {
      std::remove_reference_t<F>* f;
      std::conditional_t<std::is_void_v<result_t>, char, std::optional<result_t>> result;
   } call{&f, {}};

   owner_calls_call(calls, [](void* p)
   {
      call_t* const c = static_cast<call_t*>(p);

      if constexpr (std::is_void_v<result_t>)
         (*c->f)();
      else
         c->result.emplace((*c->f)());
   }, &call);

   if constexpr (!std::is_void_v<result_t>)
      return std::move(*call.result);
}
#endif
