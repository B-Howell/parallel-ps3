#pragma once

#include "vm.h"
#include "vm_locking.h"
#include "vm_ptr.h"
#include "util/atomic.hpp"
#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>
#include "util/tsc.hpp"
#include <functional>
#include <bit>

#ifdef _MSC_VER
extern "C"
{
	u32 _xbegin();
	void _xend();
}
#endif

namespace vm
{
	enum : u64
	{
		rsrv_lock_mask = 127,
		rsrv_unique_lock = 64,
		rsrv_putunc_flag = 32,
	};

	// A reservation's word: the time of its last update (a multiple of
	// 128) and its lock bits (rsrv_lock_mask); 64 bytes of reservation
	// memory per 128-byte cache line, the word first
	using reservation_t = retro_atomic_64_t;

	inline reservation_t* reservation_word(u32 addr)
	{
		return reinterpret_cast<reservation_t*>(g_reservations + (addr & 0xff80) / 2);
	}

	inline u64 reservation_load(reservation_t* res)
	{
		return static_cast<u64>(retro_atomic_load_acquire_64(res));
	}

	// The reservation's word as it is now: its time and lock bits
	inline u64 reservation_time(u32 addr)
	{
		return reservation_load(reservation_word(addr));
	}

	inline bool reservation_cas(reservation_t* res, u64 old, u64 value)
	{
		return retro_atomic_cas_64(res, static_cast<s64>(old), static_cast<s64>(value)) != 0;
	}

	// Adds @v (or, negative, takes away): the old word
	inline u64 reservation_add(reservation_t* res, u64 v)
	{
		return static_cast<u64>(retro_atomic_fetch_add_64(res, static_cast<s64>(v)));
	}

	inline void reservation_release(reservation_t* res, u64 value)
	{
		retro_atomic_store_release_64(res, static_cast<s64>(value));
	}

	// @f changes a copy of the word and says whether to store it: the old
	// word and whether it was stored (as atomic_t::fetch_op did)
	template <typename F>
	inline std::pair<u64, bool> reservation_fetch_op(reservation_t* res, F f)
	{
		while (true)
		{
			const u64 old = reservation_load(res);
			u64 value = old;

			if (!f(value))
			{
				return {old, false};
			}

			if (reservation_cas(res, old, value))
			{
				return {old, true};
			}
		}
	}

	// The first 16 bytes of a reservation's memory: the word (low half)
	// and, while a full-line store is in progress, the shared memory base
	// (high half)
	inline retro_atomic_128_t* reservation_wide(u32 addr)
	{
		return reinterpret_cast<retro_atomic_128_t*>(g_reservations + (addr & 0xff80) / 2);
	}

	// The next 16: a bit per byte of the cache line being written
	inline retro_atomic_128_t* reservation_byte_bits(u32 addr)
	{
		return reinterpret_cast<retro_atomic_128_t*>(g_reservations + (addr & 0xff80) / 2 + 16);
	}

	// Update reservation status
	void reservation_update(u32 addr);
	std::pair<bool, u64> try_reservation_update(u32 addr);

	// A slot reservation waiters share (by address and time): the wait
	// flag in the low half of its word (odd while someone is to be
	// notified; waited on) and the count of waiters in the high half
	struct alignas(128) reservation_waiter_t
	{
		retro_atomic_64_t word{0};
	};

	static_assert(std::endian::native == std::endian::little, "the wait flag is the word's low half");

	inline u64 reservation_waiter_load(reservation_waiter_t* waiter)
	{
		return static_cast<u64>(retro_atomic_load_acquire_64(&waiter->word));
	}

	// The wait flag's 32 bits, as waited on and notified
	inline const void* reservation_waiter_flag(reservation_waiter_t* waiter)
	{
		return &waiter->word;
	}

	// @f changes the flag and the count (f(flag, count)) and says whether
	// to store them: whether they were (a compare-and-swap loop)
	template <typename F>
	inline bool reservation_waiter_op(reservation_waiter_t* waiter, F f)
	{
		while (true)
		{
			const u64 old = reservation_waiter_load(waiter);
			u32 flag = static_cast<u32>(old);
			u32 count = static_cast<u32>(old >> 32);

			if (!f(flag, count))
			{
				return false;
			}

			if (retro_atomic_cas_64(&waiter->word, static_cast<s64>(old), static_cast<s64>(u64{count} << 32 | flag)))
			{
				return true;
			}
		}
	}

	static inline reservation_waiter_t* reservation_notifier(u32 raddr, u64 rtime)
	{
		constexpr u32 wait_vars_for_each = 32;
		constexpr u32 unique_address_bit_mask = 0b1111;
		constexpr u32 unique_rtime_bit_mask = 0b1;

		extern std::array<reservation_waiter_t, wait_vars_for_each * (unique_address_bit_mask + 1) * (unique_rtime_bit_mask + 1)> g_resrv_waiters_count;

		// Storage efficient method to distinguish different nearby addresses (which are likely)
		const usz index = std::min<usz>(std::popcount(raddr & -2048), 31) * (1 << 5) + ((rtime / 128) & unique_rtime_bit_mask) * (1 << 4) + ((raddr / 128) & unique_address_bit_mask);
		return &g_resrv_waiters_count[index];
	}

	// Returns waiter count
	static inline u32 reservation_notifier_count(u32 raddr, u64 rtime)
	{
		const u64 v = reservation_waiter_load(reservation_notifier(raddr, rtime));
		return static_cast<u32>(v) % 2 == 1 ? static_cast<u32>(v >> 32) : 0;
	}

	static inline void reservation_notifier_end_wait(reservation_waiter_t& waiter)
	{
		reservation_waiter_op(&waiter, [](u32& wait_flag, u32& waiters_count)
		{
			if (waiters_count == 1 && wait_flag % 2 == 1)
			{
				// Make wait_flag even (disabling notification on last waiter)
				wait_flag++;
			}

			waiters_count--;
			return true;
		});
	}

	static inline std::pair<reservation_waiter_t*, u32> reservation_notifier_begin_wait(u32 raddr, u64 rtime)
	{
		reservation_waiter_t& waiter = *reservation_notifier(raddr, rtime);

		u32 seen_flag = 0;

		reservation_waiter_op(&waiter, [&](u32& wait_flag, u32& waiters_count)
		{
			if (wait_flag % 2 == 0)
			{
				// Make wait_flag odd (for notification deduplication detection)
				wait_flag++;
			}

			seen_flag = wait_flag;
			waiters_count++;
			return true;
		});

		if ((reservation_time(raddr) & -128) != rtime)
		{
			reservation_notifier_end_wait(waiter);
			return {};
		}

		return { &waiter, seen_flag };
	}

	// Waits while the waiter's flag is still @flag_val, up to @timeout_ns,
	// through the wait engine (its thread stop checks and one-time callback
	// apply); may return early
	inline void reservation_notifier_wait(reservation_waiter_t* waiter, u32 flag_val, u64 timeout_ns)
	{
		atomic_wait::list<1> list{};
		list.set_raw<0>(reservation_waiter_flag(waiter), flag_val);
		list.wait(atomic_wait_timeout{timeout_ns});
	}

	// Wakes the waiters on a flag reservation_notifier_notify() gave back
	inline void reservation_notifier_wake(const void* flag)
	{
		retro_waitaddr_notify_all(flag);
	}

	// Ends a wait on the reservation (flag made even): with @postpone, the
	// flag to wake later (reservation_notifier_wake), if there were waiters
	const void* reservation_notifier_notify(u32 raddr, u64 rtime, bool postpone = false);

	u64 reservation_lock_internal(u32, reservation_t*);

	void reservation_shared_lock_internal(reservation_t*);

	inline bool reservation_try_lock(reservation_t* res, u64 rtime)
	{
		if (reservation_cas(res, rtime, rtime | rsrv_unique_lock)) [[likely]]
		{
			return true;
		}

		return false;
	}

	inline std::pair<reservation_t*, u64> reservation_lock(u32 addr)
	{
		auto res = vm::reservation_word(addr);
		auto rtime = reservation_load(res);

		if (rtime & 127 || !reservation_try_lock(res, rtime)) [[unlikely]]
		{
			static reservation_t no_lock{};

			rtime = reservation_lock_internal(addr, res);

			if (rtime == umax)
			{
				res = &no_lock;
			}
		}

		return {res, rtime};
	}

	// TODO: remove and make it external
	void reservation_op_internal(u32 addr, std::function<bool()> func);

	template <bool Ack = false, typename CPU, typename T, typename AT = u32, typename F>
	inline SAFE_BUFFERS(auto) reservation_op(CPU& /*cpu*/, _ptr_base<T, AT> ptr, F op)
	{
		// Atomic operation will be performed on aligned 128 bytes of data, so the data size and alignment must comply
		static_assert(sizeof(T) <= 128 && alignof(T) == sizeof(T), "vm::reservation_op: unsupported type");
		static_assert(std::is_trivially_copyable_v<T>, "vm::reservation_op: not triv copyable (optimization)");

		// Use "super" pointer to prevent access violation handling during atomic op
		const auto sptr = vm::get_super_ptr<T>(static_cast<u32>(ptr.addr()));

		// Prefetch some data
		//_m_prefetchw(sptr);
		//_m_prefetchw(reinterpret_cast<char*>(sptr) + 64);

		// Use 128-byte aligned addr
		const u32 addr = static_cast<u32>(ptr.addr()) & -128;

		auto res = vm::reservation_word(addr);
		//_m_prefetchw(res);

		// Lock reservation and perform heavyweight lock
		reservation_shared_lock_internal(res);

		u64 old_time = umax;

		if constexpr (std::is_void_v<std::invoke_result_t<F, T&>>)
		{
			{
				vm::writer_lock lock(addr);
				std::invoke(op, *sptr);
				old_time = reservation_add(res, 127);
			}

			if constexpr (Ack)
				reservation_notifier_notify(addr, old_time);
			return;
		}
		else
		{
			auto result = std::invoke_result_t<F, T&>();
			{
				vm::writer_lock lock(addr);

				if ((result = std::invoke(op, *sptr)))
				{
					old_time = reservation_add(res, 127);
				}
				else
				{
					old_time = reservation_add(res, u64{umax});
				}
			}

			if (Ack && result)
				reservation_notifier_notify(addr, old_time);
			return result;
		}
	}

	// For internal usage
	[[noreturn]] void reservation_escape_internal();

	// Read memory value in pseudo-atomic manner
	template <typename CPU, typename T, typename AT = u32, typename F>
	inline SAFE_BUFFERS(auto) peek_op(CPU&& cpu, _ptr_base<T, AT> ptr, F op)
	{
		// Atomic operation will be performed on aligned 128 bytes of data, so the data size and alignment must comply
		static_assert(sizeof(T) <= 128 && alignof(T) == sizeof(T), "vm::peek_op: unsupported type");

		// Use 128-byte aligned addr
		const u32 addr = static_cast<u32>(ptr.addr()) & -128;

		while (true)
		{
			if constexpr (std::is_class_v<std::remove_cvref_t<CPU>>)
			{
				if (cpu.test_stopped())
				{
					reservation_escape_internal();
				}
			}

			const u64 rtime = vm::reservation_time(addr);

			if (rtime & 127)
			{
				continue;
			}

			// Observe data non-atomically and make sure no reservation updates were made
			if constexpr (std::is_void_v<std::invoke_result_t<F, const T&>>)
			{
				std::invoke(op, *ptr);

				if (rtime == vm::reservation_time(addr))
				{
					return;
				}
			}
			else
			{
				auto res = std::invoke(op, *ptr);

				if (rtime == vm::reservation_time(addr))
				{
					return res;
				}
			}
		}
	}

	template <bool Ack = false, typename T, typename F>
	inline SAFE_BUFFERS(auto) light_op(T& data, F op)
	{
		// Optimized real ptr -> vm ptr conversion, simply UB if out of range
		const u32 addr = static_cast<u32>(reinterpret_cast<const u8*>(&data) - g_base_addr);

		// Use "super" pointer to prevent access violation handling during atomic op
		const auto sptr = vm::get_super_ptr<T>(addr);

		// "Lock" reservation
		auto res = vm::reservation_word(addr);

		auto [_old, _ok] = reservation_fetch_op(res, [&](u64& r)
		{
			if (r & vm::rsrv_unique_lock)
			{
				return false;
			}

			r += 1;
			return true;
		});

		if (!_ok) [[unlikely]]
		{
			vm::reservation_shared_lock_internal(res);
		}

		if constexpr (std::is_void_v<std::invoke_result_t<F, T&>>)
		{
			std::invoke(op, *sptr);
			reservation_add(res, 127);

			if constexpr (Ack)
			{
				// The word's low half, where a waiter on it waits
				retro_waitaddr_notify_all(res);
			}
		}
		else
		{
			auto result = std::invoke(op, *sptr);
			reservation_add(res, 127);

			if constexpr (Ack)
			{
				// The word's low half, where a waiter on it waits
				retro_waitaddr_notify_all(res);
			}

			return result;
		}
	}

	template <bool Ack = false, typename T, typename F>
	inline SAFE_BUFFERS(auto) atomic_op(T& data, F op)
	{
		return light_op<Ack, T>(data, [&](T& data)
		{
			return vm::guest_op(data, op);
		});
	}

	template <bool Ack = false, typename T, typename F>
	inline SAFE_BUFFERS(auto) fetch_op(T& data, F op)
	{
		return light_op<Ack, T>(data, [&](T& data)
		{
			return vm::guest_fetch_op(data, op);
		});
	}
} // namespace vm
