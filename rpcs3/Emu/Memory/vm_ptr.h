#pragma once

#include "util/types.hpp"
#include "util/to_endian.hpp"
#include "Utilities/StrFmt.h"
#include "vm.h"
#include "guest_atomic.h"

#include <bit>
#include <cstring>
#include <functional>

class ppu_thread;
struct ppu_func_opd_t;

namespace vm
{
	// A word of the game's memory, read and changed whole from the host: a
	// structure of 1, 2, 4, 8 or 16 bytes laid out as the game has it
	// (big-endian fields), copied whole through guest_atomic.h, and the
	// 16-byte one through retro_atomic's 128-bit word. A 1- or 2-byte one
	// goes through the aligned 4-byte word it is in, the rest of it kept.
	namespace guest_detail
	{
		template <typename T>
		u8* half_word(const T& word, usz& offset) noexcept
		{
			const uptr addr = reinterpret_cast<uptr>(&word);
			offset = addr & 3;
			return reinterpret_cast<u8*>(addr - offset);
		}
	}

	template <typename T>
	T guest_load(const T& word) noexcept
	{
		static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8 || sizeof(T) == 16);
		void* const p = const_cast<T*>(&word);

		if constexpr (sizeof(T) == 16)
		{
			const retro_u128_t v = retro_atomic_load_acquire_128(static_cast<retro_atomic_128_t*>(p));
			return std::bit_cast<T>(v);
		}
		else if constexpr (sizeof(T) == 8)
		{
			return std::bit_cast<T>(guest_atomic_load_raw64(p));
		}
		else if constexpr (sizeof(T) == 4)
		{
			return std::bit_cast<T>(guest_atomic_load_raw32(p));
		}
		else
		{
			usz offset = 0;
			const u32 raw = guest_atomic_load_raw32(guest_detail::half_word(word, offset));
			T v;
			std::memcpy(&v, reinterpret_cast<const u8*>(&raw) + offset, sizeof(T));
			return v;
		}
	}

	// Whether it was @expected and is @desired now
	template <typename T>
	bool guest_cas(T& word, const std::type_identity_t<T>& expected, const std::type_identity_t<T>& desired) noexcept
	{
		static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8 || sizeof(T) == 16);

		if constexpr (sizeof(T) == 16)
		{
			void* const p = &word;
			return retro_atomic_cas_128(static_cast<retro_atomic_128_t*>(p), std::bit_cast<retro_u128_t>(expected), std::bit_cast<retro_u128_t>(desired)) != 0;
		}
		else if constexpr (sizeof(T) == 8)
		{
			return guest_atomic_cas_raw64(&word, std::bit_cast<u64>(expected), std::bit_cast<u64>(desired)) != 0;
		}
		else if constexpr (sizeof(T) == 4)
		{
			return guest_atomic_cas_raw32(&word, std::bit_cast<u32>(expected), std::bit_cast<u32>(desired)) != 0;
		}
		else
		{
			usz offset = 0;
			u8* const w = guest_detail::half_word(word, offset);

			while (true)
			{
				const u32 raw = guest_atomic_load_raw32(w);

				if (std::memcmp(reinterpret_cast<const u8*>(&raw) + offset, &expected, sizeof(T)) != 0)
				{
					return false;
				}

				u32 next = raw;
				std::memcpy(reinterpret_cast<u8*>(&next) + offset, &desired, sizeof(T));

				// The other half may have changed meanwhile: then again
				if (guest_atomic_cas_raw32(w, raw, next))
				{
					return true;
				}
			}
		}
	}

	// Changes it as @op (a function of T&, or a member of T) says, until it
	// sticks, and returns what op returned; no write where nothing changed
	template <typename T, typename F>
	auto guest_op(T& word, F&& op) noexcept
	{
		T old = guest_load(word);

		while (true)
		{
			T next = old;

			if constexpr (std::is_void_v<std::invoke_result_t<F&, T&>>)
			{
				std::invoke(op, next);

				if (std::memcmp(&next, &old, sizeof(T)) == 0 || guest_cas(word, old, next))
				{
					return;
				}
			}
			else
			{
				auto result = std::invoke(op, next);

				if (std::memcmp(&next, &old, sizeof(T)) == 0 || guest_cas(word, old, next))
				{
					return result;
				}
			}

			old = guest_load(word);
		}
	}

	// As atomic_t's fetch_op: @op on a copy; one returning bool stores only
	// where it says true and returns {old value, its answer}, one returning
	// nothing always stores and returns the old value
	template <typename T, typename F>
	auto guest_fetch_op(T& word, F&& op) noexcept
	{
		T old = guest_load(word);

		while (true)
		{
			T next = old;

			if constexpr (std::is_void_v<std::invoke_result_t<F&, T&>>)
			{
				std::invoke(op, next);

				if (std::memcmp(&next, &old, sizeof(T)) == 0 || guest_cas(word, old, next))
				{
					return old;
				}
			}
			else
			{
				auto result = std::invoke(op, next);

				if (!result || std::memcmp(&next, &old, sizeof(T)) == 0 || guest_cas(word, old, next))
				{
					return std::make_pair(old, result);
				}
			}

			old = guest_load(word);
		}
	}

	template <typename T>
	void guest_store(T& word, const std::type_identity_t<T>& value) noexcept
	{
		if constexpr (sizeof(T) == 16)
		{
			void* const p = &word;
			retro_atomic_store_release_128(static_cast<retro_atomic_128_t*>(p), std::bit_cast<retro_u128_t>(value));
		}
		else if constexpr (sizeof(T) == 8)
		{
			guest_atomic_store_raw64(&word, std::bit_cast<u64>(value));
		}
		else if constexpr (sizeof(T) == 4)
		{
			guest_atomic_store_raw32(&word, std::bit_cast<u32>(value));
		}
		else
		{
			guest_op(word, [&](T& v) { v = value; });
		}
	}

	// The value before
	template <typename T>
	T guest_exchange(T& word, const std::type_identity_t<T>& value) noexcept
	{
		if constexpr (sizeof(T) == 8)
		{
			return std::bit_cast<T>(guest_atomic_exchange_raw64(&word, std::bit_cast<u64>(value)));
		}
		else if constexpr (sizeof(T) == 4)
		{
			return std::bit_cast<T>(guest_atomic_exchange_raw32(&word, std::bit_cast<u32>(value)));
		}
		else
		{
			return guest_op(word, [&](T& v) { const T old = v; v = value; return old; });
		}
	}

	// Enables comparison between comparable types of pointers
	template<typename T1, typename T2>
	concept PtrComparable = requires (T1* t1, T2* t2) { t1 == t2; };

	template <typename T, typename AT>
	class _ptr_base
	{
		AT m_addr;

		static_assert(!std::is_pointer_v<T>, "vm::_ptr_base<> error: invalid type (pointer)");
		static_assert(!std::is_reference_v<T>, "vm::_ptr_base<> error: invalid type (reference)");

	public:
		using type = T;
		using addr_type = std::remove_cv_t<AT>;

		ENABLE_BITWISE_SERIALIZATION;

		_ptr_base() = default;

		_ptr_base(vm::addr_t addr) noexcept
			: m_addr(addr)
		{
		}

		addr_type addr() const
		{
			return m_addr;
		}

		void set(addr_type addr)
		{
			this->m_addr = addr;
		}

		static _ptr_base make(addr_type addr)
		{
			_ptr_base result;
			result.m_addr = addr;
			return result;
		}

		// Enable only the conversions which are originally possible between pointer types
		template <typename T2, typename AT2> requires (std::is_convertible_v<T*, T2*>)
		operator _ptr_base<T2, AT2>() const noexcept
		{
			return vm::cast(m_addr);
		}

		explicit operator bool() const noexcept
		{
			return m_addr != 0u;
		}

		// Get vm pointer to a struct member
		template <typename MT, typename T2> requires PtrComparable<T, T2>
		_ptr_base<MT, u32> ptr(MT T2::*const mptr) const
		{
			return vm::cast(vm::cast(m_addr) + offset32(mptr));
		}

		// Get vm pointer to a struct member with array subscription
		template <typename MT, typename T2, typename ET = std::remove_extent_t<MT>> requires PtrComparable<T, T2>
		_ptr_base<ET, u32> ptr(MT T2::*const mptr, u32 index) const
		{
			return vm::cast(vm::cast(m_addr) + offset32(mptr) + u32{sizeof(ET)} * index);
		}

		template <bool Strict = false>
		T* get_ptr() const
		{
			if constexpr (Strict)
			{
				AUDIT(m_addr);
			}

			return static_cast<T*>(vm::base(vm::cast(m_addr)));
		}

		T* operator ->() const requires (!std::is_void_v<T>)
		{
			return get_ptr<true>();
		}

		std::add_lvalue_reference_t<T> operator *() const requires (!std::is_void_v<T>)
		{
			return *get_ptr<true>();
		}

		std::add_lvalue_reference_t<T> operator [](u32 index) const requires (!std::is_void_v<T>)
		{
			AUDIT(m_addr);

			return *static_cast<T*>(vm::base(vm::cast(m_addr) + u32{sizeof(T)} * index));
		}

		// Test address for arbitrary alignment: (addr & (align - 1)) == 0
		bool aligned(u32 align = alignof(T)) const
		{
			return (m_addr & (align - 1)) == 0u;
		}

		// Get type size
		static constexpr u32 size() noexcept requires (!std::is_void_v<T>)
		{
			return sizeof(T);
		}

		// Get type alignment
		static constexpr u32 align() noexcept requires (!std::is_void_v<T>)
		{
			return alignof(T);
		}

		_ptr_base<T, u32> operator +() const
		{
			return vm::cast(m_addr);
		}

		_ptr_base<T, u32> operator +(u32 count) const requires (!std::is_void_v<T>)
		{
			return vm::cast(vm::cast(m_addr) + count * size());
		}

		_ptr_base<T, u32> operator -(u32 count) const requires (!std::is_void_v<T>)
		{
			return vm::cast(vm::cast(m_addr) - count * size());
		}

		friend _ptr_base<T, u32> operator +(u32 count, const _ptr_base& ptr) requires (!std::is_void_v<T>)
		{
			return vm::cast(vm::cast(ptr.m_addr) + count * size());
		}

		// Pointer difference operator
		template <typename T2, typename AT2> requires (std::is_object_v<T2> && std::is_same_v<std::decay_t<T>, std::decay_t<T2>>)
		s32 operator -(const _ptr_base<T2, AT2>& right) const
		{
			return static_cast<s32>(vm::cast(m_addr) - vm::cast(right.m_addr)) / size();
		}

		_ptr_base operator ++(int) requires (!std::is_void_v<T>)
		{
			_ptr_base result = *this;
			m_addr = vm::cast(m_addr) + size();
			return result;
		}

		_ptr_base& operator ++() requires (!std::is_void_v<T>)
		{
			m_addr = vm::cast(m_addr) + size();
			return *this;
		}

		_ptr_base operator --(int) requires (!std::is_void_v<T>)
		{
			_ptr_base result = *this;
			m_addr = vm::cast(m_addr) - size();
			return result;
		}

		_ptr_base& operator --() requires (!std::is_void_v<T>)
		{
			m_addr = vm::cast(m_addr) - size();
			return *this;
		}

		_ptr_base& operator +=(s32 count) requires (!std::is_void_v<T>)
		{
			m_addr = vm::cast(m_addr) + count * size();
			return *this;
		}

		_ptr_base& operator -=(s32 count) requires (!std::is_void_v<T>)
		{
			m_addr = vm::cast(m_addr) - count * size();
			return *this;
		}

		std::pair<bool, std::conditional_t<std::is_void_v<T>, char, std::remove_const_t<T>>> try_read() const requires (std::is_copy_constructible_v<T>)
		{
			alignas(sizeof(T) >= 16 ? 16 : 8) char buf[sizeof(T)]{};
			const bool ok = vm::try_access(vm::cast(m_addr), buf, sizeof(T), false);
			return { ok, std::bit_cast<decltype(try_read().second)>(buf) };
		}

		bool try_read(std::conditional_t<std::is_void_v<T>, char, std::remove_const_t<T>>& out) const requires (!std::is_void_v<T>)
		{
			return vm::try_access(vm::cast(m_addr), std::addressof(out), sizeof(T), false);
		}

		bool try_write(const std::conditional_t<std::is_void_v<T>, char, T>& _in) const requires (!std::is_void_v<T>)
		{
			return vm::try_access(vm::cast(m_addr), const_cast<T*>(std::addressof(_in)), sizeof(T), true);
		}

		// Don't use
		auto& raw()
		{
			return m_addr;
		}
	};

	template<typename AT, typename RT, typename... T>
	class _ptr_base<RT(T...), AT>
	{
		AT m_addr;

	public:
		using addr_type = std::remove_cv_t<AT>;
		ENABLE_BITWISE_SERIALIZATION;

		_ptr_base() = default;

		_ptr_base(vm::addr_t addr)
			: m_addr(addr)
		{
		}

		addr_type addr() const
		{
			return m_addr;
		}

		void set(addr_type addr)
		{
			m_addr = addr;
		}

		static _ptr_base make(addr_type addr)
		{
			_ptr_base result;
			result.m_addr = addr;
			return result;
		}

		// Conversion to another function pointer
		template<typename AT2>
		operator _ptr_base<RT(T...), AT2>() const
		{
			return vm::cast(m_addr);
		}

		explicit operator bool() const
		{
			return m_addr != 0u;
		}

		_ptr_base<RT(T...), u32> operator +() const
		{
			return vm::cast(m_addr);
		}

		// Don't use
		auto& raw()
		{
			return m_addr;
		}

		// Callback; defined in PPUCallback.h, passing context is mandatory
		RT operator()(ppu_thread& ppu, T... args) const;
		const ppu_func_opd_t& opd() const;
	};

	template<typename AT, typename RT, typename... T>
	class _ptr_base<RT(*)(T...), AT>
	{
		static_assert(!sizeof(AT), "vm::_ptr_base<> error: use RT(T...) format for functions instead of RT(*)(T...)");
	};

	// Native endianness pointer to LE data
	template<typename T, typename AT = u32> using ptrl = _ptr_base<to_le_t<T>, AT>;
	template<typename T, typename AT = u32> using cptrl = ptrl<const T, AT>;

	// Native endianness pointer to BE data
	template<typename T, typename AT = u32> using ptrb = _ptr_base<to_be_t<T>, AT>;
	template<typename T, typename AT = u32> using cptrb = ptrb<const T, AT>;

	// BE pointer to LE data
	template<typename T, typename AT = u32> using bptrl = _ptr_base<to_le_t<T>, to_be_t<AT>>;

	// BE pointer to BE data
	template<typename T, typename AT = u32> using bptrb = _ptr_base<to_be_t<T>, to_be_t<AT>>;

	// LE pointer to LE data
	template<typename T, typename AT = u32> using lptrl = _ptr_base<to_le_t<T>, to_le_t<AT>>;

	// LE pointer to BE data
	template<typename T, typename AT = u32> using lptrb = _ptr_base<to_be_t<T>, to_le_t<AT>>;

	inline namespace ps3_
	{
		// Default pointer type for PS3 HLE functions (Native endianness pointer to BE data)
		template<typename T, typename AT = u32> using ptr = ptrb<T, AT>;
		template<typename T, typename AT = u32> using cptr = ptr<const T, AT>;

		// Default pointer to pointer type for PS3 HLE functions (Native endianness pointer to BE pointer to BE data)
		template<typename T, typename AT = u32, typename AT2 = u32> using pptr = ptr<ptr<T, AT2>, AT>;
		template<typename T, typename AT = u32, typename AT2 = u32> using cpptr = pptr<const T, AT, AT2>;

		// Default pointer type for PS3 HLE structures (BE pointer to BE data)
		template<typename T, typename AT = u32> using bptr = bptrb<T, AT>;
		template<typename T, typename AT = u32> using bcptr = bptr<const T, AT>;

		// Default pointer to pointer type for PS3 HLE structures (BE pointer to BE pointer to BE data)
		template<typename T, typename AT = u32, typename AT2 = u32> using bpptr = bptr<ptr<T, AT2>, AT>;
		template<typename T, typename AT = u32, typename AT2 = u32> using bcpptr = bpptr<const T, AT, AT2>;

		// Perform static_cast (for example, vm::ptr<void> to vm::ptr<char>)
		template <typename CT, typename T, typename AT>
			requires requires(T* t) { static_cast<to_be_t<CT>*>(t); }
		inline _ptr_base<to_be_t<CT>, u32> static_ptr_cast(const _ptr_base<T, AT>& other)
		{
			return vm::cast(other.addr());
		}

		// Perform const_cast (for example, vm::cptr<char> to vm::ptr<char>)
		template <typename CT, typename T, typename AT>
			requires requires(T* t) { const_cast<to_be_t<CT>*>(t); }
		inline _ptr_base<to_be_t<CT>, u32> const_ptr_cast(const _ptr_base<T, AT>& other)
		{
			return vm::cast(other.addr());
		}

		// Perform reinterpret cast
		template <typename CT, typename T, typename AT>
			requires std::is_object_v<T> && std::is_object_v<to_be_t<CT>>
		inline _ptr_base<to_be_t<CT>, u32> unsafe_ptr_cast(const _ptr_base<T, AT>& other)
		{
			return vm::cast(other.addr());
		}
	}

	struct null_t
	{
		template<typename T, typename AT>
		operator _ptr_base<T, AT>() const
		{
			return _ptr_base<T, AT>{};
		}

		template<typename T, typename AT>
		constexpr bool operator ==(const _ptr_base<T, AT>& ptr) const
		{
			return !ptr;
		}

		template<typename T, typename AT>
		constexpr bool operator <(const _ptr_base<T, AT>& ptr) const
		{
			return 0 < ptr.addr();
		}
	};

	// Null pointer convertible to any vm::ptr* type
	constexpr null_t null{};
}

template<typename T1, typename AT1, typename T2, typename AT2> requires vm::PtrComparable<T1, T2>
inline bool operator ==(const vm::_ptr_base<T1, AT1>& left, const vm::_ptr_base<T2, AT2>& right)
{
	return left.addr() == right.addr();
}

template<typename T1, typename AT1, typename T2, typename AT2> requires vm::PtrComparable<T1, T2>
inline bool operator <(const vm::_ptr_base<T1, AT1>& left, const vm::_ptr_base<T2, AT2>& right)
{
	return left.addr() < right.addr();
}

template<typename T1, typename AT1, typename T2, typename AT2> requires vm::PtrComparable<T1, T2>
inline bool operator <=(const vm::_ptr_base<T1, AT1>& left, const vm::_ptr_base<T2, AT2>& right)
{
	return left.addr() <= right.addr();
}

template<typename T1, typename AT1, typename T2, typename AT2> requires vm::PtrComparable<T1, T2>
inline bool operator >(const vm::_ptr_base<T1, AT1>& left, const vm::_ptr_base<T2, AT2>& right)
{
	return left.addr() > right.addr();
}

template<typename T1, typename AT1, typename T2, typename AT2> requires vm::PtrComparable<T1, T2>
inline bool operator >=(const vm::_ptr_base<T1, AT1>& left, const vm::_ptr_base<T2, AT2>& right)
{
	return left.addr() >= right.addr();
}

// Change AT endianness to BE/LE
template<typename T, typename AT, bool Se>
struct to_se<vm::_ptr_base<T, AT>, Se>
{
	using type = vm::_ptr_base<T, typename to_se<AT, Se>::type>;
};

// Format pointer
template <typename T, typename AT>
struct fmt_unveil<vm::_ptr_base<T, AT>>
{
	using type = vm::_ptr_base<T, u32>; // Use only T, ignoring AT

	static inline auto get(const vm::_ptr_base<T, AT>& arg)
	{
		return fmt_unveil<AT>::get(arg.addr());
	}
};

template <>
struct fmt_class_string<vm::_ptr_base<const void, u32>>
{
	static void format(std::string& out, u64 arg);
};

template <typename T>
struct fmt_class_string<vm::_ptr_base<T, u32>> : fmt_class_string<vm::_ptr_base<const void, u32>>
{
	// Classify all pointers as const void*
};

template <>
struct fmt_class_string<vm::_ptr_base<const char, u32>>
{
	static void format(std::string& out, u64 arg);
};

template <>
struct fmt_class_string<vm::_ptr_base<char, u32>> : fmt_class_string<vm::_ptr_base<const char, u32>>
{
	// Classify char* as const char*
};

template <usz Size>
struct fmt_class_string<vm::_ptr_base<const char[Size], u32>> : fmt_class_string<vm::_ptr_base<const char, u32>>
{
	// Classify const char[] as const char*
};

template <usz Size>
struct fmt_class_string<vm::_ptr_base<char[Size], u32>> : fmt_class_string<vm::_ptr_base<const char, u32>>
{
	// Classify char[] as const char*
};
