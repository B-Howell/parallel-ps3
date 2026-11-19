#pragma once

#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>
#include <type_traits>
#include "util/bit_set.hpp"
#include <util/types.hpp>

namespace rsx
{
	template <int N>
	void unpack_bitset(const bit_set<N>& block, u64* values)
	{
		for (int bit = 0, n = -1, shift = 0; bit < N; ++bit, ++shift)
		{
			if ((bit % 64) == 0)
			{
				values[++n] = 0;
				shift = 0;
			}

			if (block[bit])
			{
				values[n] |= (1ull << shift);
			}
		}
	}

	template <int N>
	void pack_bitset(bit_set<N>& block, u64* values)
	{
		for (int n = 0, shift = 0; shift < N; ++n, shift += 64)
		{
			bit_set<N> tmp = values[n];
			tmp <<= shift;
			block |= tmp;
		}
	}

	// An enum's bits in one retro_atomic word: a retro_atomic_int_t for
	// masks of 32 bits or fewer, a retro_atomic_64_t for 64
	template <typename T, typename bitmask_type = u32>
		requires std::is_integral_v<bitmask_type>&& std::is_enum_v<T>
	class atomic_bitmask_t
	{
	private:
		static constexpr bool wide = sizeof(bitmask_type) == 8;
		using word_t = std::conditional_t<wide, retro_atomic_64_t, retro_atomic_int_t>;
		using raw_t = std::conditional_t<wide, s64, int>;

		mutable word_t m_data{0};

		static raw_t raw(T value) { return static_cast<raw_t>(static_cast<bitmask_type>(value)); }

		bitmask_type get() const
		{
			if constexpr (wide)
				return static_cast<bitmask_type>(retro_atomic_load_acquire_64(&m_data));
			else
				return static_cast<bitmask_type>(retro_atomic_load_acquire_int(&m_data));
		}

		bitmask_type fetch_or(raw_t v)
		{
			if constexpr (wide)
				return static_cast<bitmask_type>(retro_atomic_fetch_or_64(&m_data, v));
			else
				return static_cast<bitmask_type>(retro_atomic_fetch_or_int(&m_data, v));
		}

		bitmask_type fetch_and(raw_t v)
		{
			if constexpr (wide)
				return static_cast<bitmask_type>(retro_atomic_fetch_and_64(&m_data, v));
			else
				return static_cast<bitmask_type>(retro_atomic_fetch_and_int(&m_data, v));
		}

		void set(raw_t v)
		{
			if constexpr (wide)
				retro_atomic_store_release_64(&m_data, v);
			else
				retro_atomic_store_release_int(&m_data, v);
		}

	public:
		atomic_bitmask_t() = default;

		T load() const
		{
			return static_cast<T>(get());
		}

		void store(T value)
		{
			set(raw(value));
		}

		bool operator & (T mask) const
		{
			return ((get() & static_cast<bitmask_type>(mask)) != 0);
		}

		T operator | (T mask) const
		{
			return static_cast<T>(get() | static_cast<bitmask_type>(mask));
		}

		void operator &= (T mask)
		{
			fetch_and(raw(mask));
		}

		void operator |= (T mask)
		{
			fetch_or(raw(mask));
		}

		bool test_and_set(T mask)
		{
			const bitmask_type old = fetch_or(raw(mask));
			return (old & static_cast<bitmask_type>(mask)) != 0;
		}

		auto clear(T mask)
		{
			const bitmask_type clear_mask = ~(static_cast<bitmask_type>(mask));
			return static_cast<bitmask_type>(fetch_and(static_cast<raw_t>(clear_mask)) & clear_mask);
		}

		void clear()
		{
			set(0);
		}

		operator bool () const
		{
			return get() != 0;
		}

		// Until the mask is no longer @old, woken by notify_all: no spin
		void wait(T old) const requires (!wide)
		{
			const retro_waitaddr_t w{&m_data, static_cast<u32>(static_cast<bitmask_type>(old))};
			if (get() == static_cast<bitmask_type>(old))
				retro_waitaddr_wait(&w, 1, -1);
		}

		void notify_all() requires (!wide)
		{
			retro_waitaddr_notify_all(&m_data);
		}
	};

	template <typename T, typename bitmask_type = u32>
		requires std::is_integral_v<bitmask_type> && std::is_enum_v<T>
	class bitmask_t
	{
	private:
		bitmask_type m_data = 0;

	public:
		bitmask_t() = default;

		bitmask_type load() const
		{
			return m_data;
		}

		bool operator & (bitmask_type mask) const
		{
			return !!(m_data & mask);
		}

		bitmask_type operator | (bitmask_type mask) const
		{
			return m_data | mask;
		}

		void operator &= (bitmask_type mask)
		{
			m_data &= mask;
		}

		void operator |= (bitmask_type mask)
		{
			m_data |= mask;
		}

		bool test(T mask)
		{
			return !!(m_data & static_cast<bitmask_type>(mask));
		}

		void set(T mask)
		{
			m_data |= static_cast<bitmask_type>(mask);
		}

		bool test_and_set(T mask)
		{
			const auto old = m_data;
			m_data |= static_cast<bitmask_type>(mask);
			return !!(old & mask);
		}

		template <typename U>
			requires std::is_same_v<T, U> || std::is_same_v<bitmask_type, U>
		void clear(U mask)
		{
			const bitmask_type clear_mask = ~(static_cast<bitmask_type>(mask));
			m_data &= clear_mask;
		}

		void clear()
		{
			m_data = 0;
		}

		operator bool() const
		{
			return !!m_data;
		}
	};
}
