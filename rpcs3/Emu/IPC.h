#pragma once

#include <unordered_map>
#include <unordered_set>

#include <retro_atomic.h>
#include <queues/retro_retire.h>

#include <rthreads/retro_waitaddr.h>
#include "util/shared_ptr.hpp"
#include "Emu/Cell/Modules/hle_service.h"

// IPC manager for objects of type T and IPC keys of type K: the guest's
// process-shared objects by key.
//
// No lock. The table is read in place: a lookup enters, finds and leaves,
// and a table is never changed once published. The HLE thread alone
// changes it (hle_call), copying the table, changing the copy and
// publishing it; the one replaced is retired (retro_retire) and freed once
// no lookup is in it. An object being made for a key is marked so in the
// table: it is made on its caller's thread, and another caller asking for
// the same key meanwhile waits, untimed, for it to be made.
template <typename T, typename K>
class ipc_manager final
{
	struct table
	{
		mpsc_stack_node_t link{}; // first: retired as this
		std::unordered_map<K, shared_ptr<T>> map;
		std::unordered_set<K> making;
	};

	retro_atomic_ptr_t m_table{}; // table*, never null
	retro_retire_t m_retire{};
	retro_atomic_int_t m_made{0}; // bumped, and its waiters woken, as an object made is published

	static void free_table(mpsc_stack_node_t* node)
	{
		delete static_cast<table*>(static_cast<void*>(node));
	}

	table* current() const
	{
		return static_cast<table*>(retro_atomic_load_acquire_ptr(const_cast<retro_atomic_ptr_t*>(&m_table)));
	}

	// The HLE thread's: @f applied to a copy, published
	template <typename F>
	void change(F&& f)
	{
		table* const cur = current();
		table* const next = new table;
		next->map = cur->map;
		next->making = cur->making;
		f(*next);
		retro_atomic_store_release_ptr(&m_table, next);
		retro_retire(&m_retire, &cur->link, free_table);
	}

	// @f with the table published, read in place
	template <typename F>
	auto read(F&& f) const -> decltype(f(std::declval<const table&>()))
	{
		retro_retire_t* const r = const_cast<retro_retire_t*>(&m_retire);
		retro_retire_enter(r);
		struct leave_t { retro_retire_t* r; ~leave_t() { retro_retire_leave(r, free_table); } } leave{r};
		return f(*current());
	}

public:
	ipc_manager()
	{
		retro_retire_init(&m_retire);
		retro_atomic_ptr_init(&m_table, new table);
	}

	~ipc_manager()
	{
		retro_retire_drain(&m_retire, free_table);
		delete current();
	}

	ipc_manager(const ipc_manager&) = delete;
	ipc_manager& operator=(const ipc_manager&) = delete;

	// Add new object if specified ipc_key is not used
	// .first: added new object?, .second: what's at m_map[key] after this function if (peek_ptr || added new object) is true
	template <typename F>
	std::pair<bool, shared_ptr<T>> add(const K& ipc_key, F&& provider, bool peek_ptr = true)
	{
		enum class found_t { make, there, being_made };

		// There already: seen in place, no call to the HLE thread
		if (shared_ptr<T> there = get(ipc_key))
		{
			return {false, peek_ptr ? std::move(there) : null_ptr};
		}

		for (;;)
		{
			// Read before looking: an object made after it moves it
			const u32 made_before = static_cast<u32>(retro_atomic_load_acquire_int(&m_made));

			shared_ptr<T> existing;
			const found_t found = hle_call([&]() -> found_t
			{
				const table* const t = current();

				if (const auto it = t->map.find(ipc_key); it != t->map.end() && it->second)
				{
					existing = it->second;
					return found_t::there;
				}

				if (t->making.contains(ipc_key))
				{
					return found_t::being_made;
				}

				change([&](table& next) { next.making.insert(ipc_key); });
				return found_t::make;
			});

			if (found == found_t::there)
			{
				return {false, peek_ptr ? std::move(existing) : null_ptr};
			}

			if (found == found_t::being_made)
			{
				// Another caller makes it: waited for as a PPU waits
				cpu_thread* const cpu = cpu_thread::get_current();
				const bool flagged = cpu && !(cpu->state & cpu_flag::wait);

				if (flagged)
					cpu->state += cpu_flag::wait;

				const retro_waitaddr_t made{&m_made, made_before};

				while (static_cast<u32>(retro_atomic_load_acquire_int(&m_made)) == made_before)
				{
					retro_waitaddr_wait(&made, 1, -1);
				}

				if (flagged)
					cpu->check_state();

				continue;
			}

			// Call a function which must return the object (on this thread, as
			// it always was)
			shared_ptr<T> made = provider();

			hle_call([&]()
			{
				change([&](table& next)
				{
					next.making.erase(ipc_key);
					next.map[ipc_key] = made;
				});
			});

			retro_atomic_fetch_add_int(&m_made, 1);
			retro_waitaddr_notify_all(&m_made);

			const bool added = static_cast<bool>(made);
			return {added, added ? std::move(made) : null_ptr};
		}
	}

	// Unregister specified ipc_key, may return true even if the object doesn't exist anymore
	bool remove(const K& ipc_key)
	{
		return hle_call([&]() -> bool
		{
			if (!current()->map.contains(ipc_key))
			{
				return false;
			}

			change([&](table& next) { next.map.erase(ipc_key); });
			return true;
		});
	}

	// Get object with specified ipc_key
	shared_ptr<T> get(const K& ipc_key) const
	{
		return read([&](const table& t) -> shared_ptr<T>
		{
			const auto found = t.map.find(ipc_key);
			return found != t.map.end() ? found->second : shared_ptr<T>{};
		});
	}

	// Check whether the object actually exists
	bool check(const K& ipc_key) const
	{
		return read([&](const table& t) { return t.map.contains(ipc_key); });
	}
};
