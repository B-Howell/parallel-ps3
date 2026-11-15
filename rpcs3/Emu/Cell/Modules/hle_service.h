#pragma once

/* The HLE modules' own thread. A module whose state only the game's calls
 * touch - several PPU threads, an overlay, the frontend - keeps it on this
 * thread: every call that touches it runs here (owner_calls), one at a
 * time, so it takes no lock. Started at its first call; stopped when the
 * game is unloaded, after which a call runs on its caller's thread until
 * the next start. */

#include "../../CPU/owner_calls.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The calls of the thread, started if it was not */
owner_calls_t *hle_service_calls(void);

/* True on the thread itself, where a call runs at once */
int hle_service_on_thread(void);

/* Stops the thread (the calls made are run) and readies a new one for the
 * next call. Not while the game runs */
void hle_service_stop(void);

#ifdef __cplusplus
}

#include "Emu/CPU/CPUThread.h"

/* C++: @f run on the owner of @calls (at once when @on_owner) and its
 * result given back. A PPU thread waiting there counts as waiting
 * (cpu_flag::wait), so a suspend_all made meanwhile does not wait for it;
 * it looks at its state again after */
template <typename F>
auto cpu_owner_call(owner_calls_t *calls, bool on_owner, F&& f) -> decltype(f())
{
   if (on_owner)
      return f();

   cpu_thread* const cpu = cpu_thread::get_current();
   const bool flagged = cpu && !(cpu->state & cpu_flag::wait);

   if (flagged)
      cpu->state += cpu_flag::wait;

   struct after_t
   {
      cpu_thread* cpu;
      ~after_t() { if (cpu) cpu->check_state(); }
   } after{flagged ? cpu : nullptr};

   return owner_calls_on(calls, false, std::forward<F>(f));
}

/* C++: @f run on the HLE thread and its result given back */
template <typename F>
auto hle_call(F&& f) -> decltype(f())
{
   const bool here = hle_service_on_thread();
   return cpu_owner_call(here ? nullptr : hle_service_calls(), here, std::forward<F>(f));
}
#endif
