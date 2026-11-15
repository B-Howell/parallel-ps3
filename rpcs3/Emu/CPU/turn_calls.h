#pragma once

/* Calls on one object taken in turn, with no lock and no thread of the
 * object's own: whoever finds the object free runs its call at once, then
 * the calls other threads handed it meanwhile, in the order they came, and
 * gives the object up. A thread that finds it taken hands its call over
 * and waits for it to have been run.
 *
 * Free, as an object mostly is, a call costs one compare-and-swap to take
 * the turn and one exchange to give it up, as a lock would, with nothing
 * spun on: a waiter sleeps on its own record's word (retro_waitaddr) and
 * the thread whose turn it is runs its call instead of handing the object
 * from one thread to the next. The records live on the callers' stacks
 * and go on an mpsc_stack: nothing is allocated.
 *
 * A call runs on whichever thread has the turn, so it must not depend on
 * the thread it runs on, and must not make a call on the same object (it
 * would wait for itself). */

#include <retro_atomic.h>
#include <queues/mpsc_stack.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turn_calls
{
   mpsc_stack_t calls;      /* handed over, waiting for the turn */
   retro_atomic_int_t busy; /* 1 while a thread has the turn */
} turn_calls_t;

void turn_calls_init(turn_calls_t *t);

/* @run(@ctx) in turn, on this thread or the one that has the turn; back
 * once it has run */
void turn_calls_call(turn_calls_t *t, void (*run)(void *ctx), void *ctx);

/* Whether a thread has the turn now: for a look that is only a hint */
int turn_calls_busy(turn_calls_t *t);

#ifdef __cplusplus
}

#include <optional>
#include <type_traits>
#include <utility>

/* C++: @f run in turn and its result given back */
template <typename F>
auto turn_call(turn_calls_t* t, F&& f) -> decltype(f())
{
   using result_t = decltype(f());

   struct call_t
   {
      std::remove_reference_t<F>* f;
      std::conditional_t<std::is_void_v<result_t>, char, std::optional<result_t>> result;
   } call{&f, {}};

   turn_calls_call(t, [](void* p)
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

/* C++: a turn_calls_t that is set up when made */
struct turn_calls_cpp : turn_calls_t
{
   turn_calls_cpp() noexcept { turn_calls_init(this); }
   turn_calls_cpp(const turn_calls_cpp&) = delete;
   turn_calls_cpp& operator=(const turn_calls_cpp&) = delete;
};
#endif
