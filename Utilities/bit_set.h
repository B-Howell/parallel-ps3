#pragma once

/*
This header implements bs_t<> class for scoped enum types (enum class).
To enable bs_t<>, enum scope must contain `__bitset_enum_max` entry.

enum class flagzz : u32
{
	flag1, // Bit indices start from zero
	flag2,

	__bitset_enum_max // It must be the last value
};

This also enables helper operators for this enum type.

Examples:
`+flagzz::flag1` - unary `+` operator convert flagzz value to bs_t<flagzz>
`flagzz::flag1 + flagzz::flag2` - bitset union
`flagzz::flag1 - flagzz::flag2` - bitset difference
Intersection (&) and symmetric difference (^) is also available.
*/

#include "util/types.hpp"
#include "util/atomic.hpp"
#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>
#include <utility>

template <typename T>
concept BitSetEnum = std::is_enum_v<T> && requires(T x)
{
	T::__bitset_enum_max;
};

template <BitSetEnum T>
class atomic_bs_t;

// Bitset type for enum class with available bits [0, T::__bitset_enum_max)
template <BitSetEnum T>
class bs_t final
{
public:
	// Underlying type
	using under = std::underlying_type_t<T>;

	ENABLE_BITWISE_SERIALIZATION;

private:
	// Underlying value
	under m_data;

	friend class atomic_bs_t<T>;

	// Value constructor
	constexpr explicit bs_t(int, under data) noexcept
		: m_data(data)
	{
	}

public:
	static constexpr usz bitmax = sizeof(T) * 8;
	static constexpr usz bitsize = static_cast<under>(T::__bitset_enum_max);

	static_assert(std::is_enum_v<T>, "bs_t<> error: invalid type (must be enum)");
	static_assert(bitsize <= bitmax, "bs_t<> error: invalid __bitset_enum_max");
	static_assert(bitsize != bitmax || std::is_unsigned_v<under>, "bs_t<> error: invalid __bitset_enum_max (sign bit)");

	// Helper function
	static constexpr under shift(T value)
	{
		return static_cast<under>(1) << static_cast<under>(value);
	}

	bs_t() = default;

	// Construct from a single bit
	constexpr bs_t(T bit) noexcept
		: m_data(shift(bit))
	{
	}

	// Test for empty bitset
	constexpr explicit operator bool() const noexcept
	{
		return m_data != 0;
	}

	// Extract underlying data
	constexpr explicit operator under() const noexcept
	{
		return m_data;
	}

	// Copy
	constexpr bs_t operator +() const
	{
		return *this;
	}

	constexpr bs_t& operator +=(bs_t rhs)
	{
		m_data |= static_cast<under>(rhs);
		return *this;
	}

	constexpr bs_t& operator -=(bs_t rhs)
	{
		m_data &= ~static_cast<under>(rhs);
		return *this;
	}

	constexpr bs_t& operator &=(bs_t rhs)
	{
		m_data &= static_cast<under>(rhs);
		return *this;
	}

	constexpr bs_t& operator ^=(bs_t rhs)
	{
		m_data ^= static_cast<under>(rhs);
		return *this;
	}

	friend constexpr bs_t operator +(bs_t lhs, bs_t rhs)
	{
		return bs_t(0, lhs.m_data | rhs.m_data);
	}

	friend constexpr bs_t operator -(bs_t lhs, bs_t rhs)
	{
		return bs_t(0, lhs.m_data & ~rhs.m_data);
	}

	friend constexpr bs_t operator &(bs_t lhs, bs_t rhs)
	{
		return bs_t(0, lhs.m_data & rhs.m_data);
	}

	friend constexpr bs_t operator ^(bs_t lhs, bs_t rhs)
	{
		return bs_t(0, lhs.m_data ^ rhs.m_data);
	}

	constexpr bool operator ==(bs_t rhs) const noexcept
	{
		return m_data == rhs.m_data;
	}

	constexpr bool test_and_set(T bit)
	{
		const under s = shift(bit);
		const bool r = (m_data & s) != 0;
		m_data |= s;
		return r;
	}

	constexpr bool test_and_set(T bit, bool value)
	{
		const under s = shift(bit);
		const bool r = (m_data & s) != 0;
		m_data = (value ? (m_data | s) : (m_data & ~s));
		return r;
	}

	constexpr bool test_and_reset(T bit)
	{
		const under s = shift(bit);
		const bool r = (m_data & s) != 0;
		m_data &= ~s;
		return r;
	}

	constexpr bool test_and_complement(T bit)
	{
		const under s = shift(bit);
		const bool r = (m_data & s) != 0;
		m_data ^= s;
		return r;
	}

	constexpr bool test(bs_t arg) const
	{
		return (m_data & arg.m_data) != 0;
	}

	constexpr bool all_of(bs_t arg) const
	{
		return (m_data & arg.m_data) == arg.m_data;
	}

	constexpr bool none_of(bs_t arg) const
	{
		return (m_data & arg.m_data) == 0;
	}
};

// Unary '+' operator: promote plain enum value to bitset value
template <BitSetEnum T>
constexpr bs_t<T> operator +(T bit)
{
	return bs_t<T>(bit);
}

// Binary '+' operator: bitset union
template <BitSetEnum T, typename U> requires (std::is_constructible_v<bs_t<T>, U>)
constexpr bs_t<T> operator +(T lhs, const U& rhs)
{
	return bs_t<T>(lhs) + bs_t<T>(rhs);
}

// Binary '+' operator: bitset union
template <typename U, BitSetEnum T> requires (std::is_constructible_v<bs_t<T>, U> && !std::is_enum_v<U>)
constexpr bs_t<T> operator +(const U& lhs, T rhs)
{
	return bs_t<T>(lhs) + bs_t<T>(rhs);
}

// Binary '-' operator: bitset difference
template <BitSetEnum T, typename U> requires (std::is_constructible_v<bs_t<T>, U>)
constexpr bs_t<T> operator -(T lhs, const U& rhs)
{
	return bs_t<T>(lhs) - bs_t<T>(rhs);
}

// Binary '-' operator: bitset difference
template <typename U, BitSetEnum T> requires (std::is_constructible_v<bs_t<T>, U> && !std::is_enum_v<U>)
constexpr bs_t<T> operator -(const U& lhs, T rhs)
{
	return bs_t<T>(lhs) - bs_t<T>(rhs);
}

// Binary '&' operator: bitset intersection
template <BitSetEnum T, typename U> requires (std::is_constructible_v<bs_t<T>, U>)
constexpr bs_t<T> operator &(T lhs, const U& rhs)
{
	return bs_t<T>(lhs) & bs_t<T>(rhs);
}

// Binary '&' operator: bitset intersection
template <typename U, BitSetEnum T> requires (std::is_constructible_v<bs_t<T>, U> && !std::is_enum_v<U>)
constexpr bs_t<T> operator &(const U& lhs, T rhs)
{
	return bs_t<T>(lhs) & bs_t<T>(rhs);
}

// Binary '^' operator: bitset symmetric difference
template <BitSetEnum T, typename U> requires (std::is_constructible_v<bs_t<T>, U>)
constexpr bs_t<T> operator ^(T lhs, const U& rhs)
{
	return bs_t<T>(lhs) ^ bs_t<T>(rhs);
}

// Binary '^' operator: bitset symmetric difference
template <typename U, BitSetEnum T> requires (std::is_constructible_v<bs_t<T>, U> && !std::is_enum_v<U>)
constexpr bs_t<T> operator ^(const U& lhs, T rhs)
{
	return bs_t<T>(lhs) ^ bs_t<T>(rhs);
}

// Atomic bitset: one retro_atomic_int_t word (the flags of a 32-bit
// enum), every operation on it a retro_atomic call. store() is an
// exchange, a full barrier as the atomic_t store it replaces was;
// release() is a release store. Its word is first, so a waiter list
// waits on the bitset's own address
template <BitSetEnum T>
class atomic_bs_t
{
	// Corresponding bitset type
	using bs_t = ::bs_t<T>;

public:
	// Underlying type
	using under = typename bs_t::under;

private:
	static_assert(sizeof(under) == 4, "atomic_bs_t: a 32-bit enum");

	mutable retro_atomic_int_t m_word{0};

	static bs_t from(int v)
	{
		return bs_t(0, static_cast<under>(static_cast<u32>(v)));
	}

	static int to(bs_t v)
	{
		return static_cast<int>(static_cast<u32>(static_cast<under>(v)));
	}

	static int to_bit(T rhs)
	{
		return static_cast<int>(1u << static_cast<uint>(static_cast<under>(rhs)));
	}

public:
	atomic_bs_t() = default;

	atomic_bs_t(const atomic_bs_t&) = delete;

	atomic_bs_t& operator =(const atomic_bs_t&) = delete;

	explicit atomic_bs_t(bs_t value)
		: m_word(to(value))
	{
	}

	explicit atomic_bs_t(T bit)
		: m_word(to(bs_t(bit)))
	{
	}

	bs_t load() const
	{
		return from(retro_atomic_load_acquire_int(&m_word));
	}

	operator bs_t() const
	{
		return load();
	}

	explicit operator bool() const
	{
		return static_cast<bool>(load());
	}

	explicit operator under() const
	{
		return static_cast<under>(load());
	}

	bs_t operator +() const
	{
		return load();
	}

	// A full barrier, as the atomic_t store was
	void store(bs_t value)
	{
		retro_atomic_exchange_int(&m_word, to(value));
	}

	void release(bs_t value)
	{
		retro_atomic_store_release_int(&m_word, to(value));
	}

	bs_t exchange(bs_t value)
	{
		return from(retro_atomic_exchange_int(&m_word, to(value)));
	}

	// The old flags, whether replaced or not
	bs_t compare_and_swap(bs_t cmp, bs_t exch)
	{
		while (true)
		{
			const int old = retro_atomic_load_acquire_int(&m_word);

			if (old != to(cmp) || retro_atomic_cas_int(&m_word, old, to(exch)))
			{
				return from(old);
			}
		}
	}

	bool compare_and_swap_test(bs_t cmp, bs_t exch)
	{
		return retro_atomic_cas_int(&m_word, to(cmp), to(exch)) != 0;
	}

	// As atomic_t::fetch_op: the old flags, or the old flags and func's
	// result (nothing stored when it is false)
	template <typename F, typename RT = std::invoke_result_t<F, bs_t&>>
	std::conditional_t<std::is_void_v<RT>, bs_t, std::pair<bs_t, RT>> fetch_op(F func)
	{
		while (true)
		{
			const int old = retro_atomic_load_acquire_int(&m_word);
			bs_t value = from(old);

			if constexpr (std::is_void_v<RT>)
			{
				std::invoke(func, value);

				if (retro_atomic_cas_int(&m_word, old, to(value))) [[likely]]
				{
					return from(old);
				}
			}
			else
			{
				RT ret = std::invoke(func, value);

				if (!ret || retro_atomic_cas_int(&m_word, old, to(value))) [[likely]]
				{
					return {from(old), std::move(ret)};
				}
			}
		}
	}

	// As atomic_t::atomic_op: func's result, always stored
	template <typename F, typename RT = std::invoke_result_t<F, bs_t&>>
	RT atomic_op(F func)
	{
		while (true)
		{
			const int old = retro_atomic_load_acquire_int(&m_word);
			bs_t value = from(old);

			if constexpr (std::is_void_v<RT>)
			{
				std::invoke(func, value);

				if (retro_atomic_cas_int(&m_word, old, to(value))) [[likely]]
				{
					return;
				}
			}
			else
			{
				RT result = std::invoke(func, value);

				if (retro_atomic_cas_int(&m_word, old, to(value))) [[likely]]
				{
					return result;
				}
			}
		}
	}

	bs_t fetch_add(const bs_t& rhs)
	{
		return from(retro_atomic_fetch_or_int(&m_word, to(rhs)));
	}

	bs_t add_fetch(const bs_t& rhs)
	{
		return fetch_add(rhs) + rhs;
	}

	bs_t operator +=(const bs_t& rhs)
	{
		return add_fetch(rhs);
	}

	bs_t fetch_sub(const bs_t& rhs)
	{
		return from(retro_atomic_fetch_and_int(&m_word, ~to(rhs)));
	}

	bs_t sub_fetch(const bs_t& rhs)
	{
		return fetch_sub(rhs) - rhs;
	}

	bs_t operator -=(const bs_t& rhs)
	{
		return sub_fetch(rhs);
	}

	bs_t fetch_and(const bs_t& rhs)
	{
		return from(retro_atomic_fetch_and_int(&m_word, to(rhs)));
	}

	bs_t and_fetch(const bs_t& rhs)
	{
		return fetch_and(rhs) & rhs;
	}

	bs_t operator &=(const bs_t& rhs)
	{
		return and_fetch(rhs);
	}

	bs_t fetch_xor(const bs_t& rhs)
	{
		return fetch_op([&](bs_t& v) { v ^= rhs; });
	}

	bs_t xor_fetch(const bs_t& rhs)
	{
		return fetch_xor(rhs) ^ rhs;
	}

	bs_t operator ^=(const bs_t& rhs)
	{
		return xor_fetch(rhs);
	}

	// Whether the flag was set before
	bool test_and_set(T rhs)
	{
		return (retro_atomic_fetch_or_int(&m_word, to_bit(rhs)) & to_bit(rhs)) != 0;
	}

	bool test_and_reset(T rhs)
	{
		return (retro_atomic_fetch_and_int(&m_word, ~to_bit(rhs)) & to_bit(rhs)) != 0;
	}

	bool test_and_invert(T rhs)
	{
		return (to(fetch_xor(bs_t(rhs))) & to_bit(rhs)) != 0;
	}

	bool all_of(bs_t arg) const
	{
		return load().all_of(arg);
	}

	bool none_of(bs_t arg) const
	{
		return load().none_of(arg);
	}

	// Waits while the flags are @old
	void wait(bs_t old) const
	{
		const retro_waitaddr_t w{&m_word, static_cast<u32>(to(old))};

		while (retro_atomic_load_acquire_int(&m_word) == to(old))
		{
			retro_waitaddr_wait(&w, 1, -1);
		}
	}

	void notify_one()
	{
		retro_waitaddr_notify_one(&m_word);
	}

	void notify_all()
	{
		retro_waitaddr_notify_all(&m_word);
	}
};

template <typename T>
struct fmt_unveil;

template <typename T>
struct fmt_unveil<bs_t<T>>
{
	// Format as is
	using type = bs_t<T>;

	static inline u64 get(const bs_t<T>& bitset)
	{
		return static_cast<std::underlying_type_t<T>>(bitset);
	}
};
