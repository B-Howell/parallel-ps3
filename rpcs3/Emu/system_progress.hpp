#pragma once

#include "util/types.hpp"
#include "util/atomic.hpp"
#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>
#include "Utilities/lockless.h"
#include "util/shared_ptr.hpp"

extern lf_array<atomic_ptr<std::string>> g_progr_text_queue;

struct progress_dialog_string_t
{
	// What the dialog shows, unpacked: a count of updates (only compared
	// between two reads), the index in g_progr_text_queue of the text shown
	// (umax: none) and how many dialogs are open
	struct data_t
	{
		u32 update_id = 0;
		u32 text_index = umax;
		u32 text_count = 0;
	};

	// All three in one word: update_id in the top 24 bits, text_index in the
	// next 20 (all ones: none), text_count in the low 20
	static constexpr u32 c_field_mask = 0xfffff;

	static constexpr u64 pack(const data_t& d)
	{
		const u64 index = d.text_index == umax ? c_field_mask : (d.text_index & c_field_mask);
		return (static_cast<u64>(d.update_id & 0xffffff) << 40) | (index << 20) | (d.text_count & c_field_mask);
	}

	static constexpr data_t unpack(u64 word)
	{
		data_t d;
		const u32 index = static_cast<u32>(word >> 20) & c_field_mask;
		d.update_id = static_cast<u32>(word >> 40);
		d.text_index = index == c_field_mask ? umax : index;
		d.text_count = static_cast<u32>(word) & c_field_mask;
		return d;
	}

	mutable retro_atomic_64_t data{static_cast<s64>(pack(data_t{}))};

	data_t load() const noexcept { return unpack(static_cast<u64>(retro_atomic_load_acquire_64(&data))); }
	bool cas(const data_t& from, const data_t& to) noexcept { return retro_atomic_cas_64(&data, static_cast<s64>(pack(from)), static_cast<s64>(pack(to))) != 0; }
	void reset() noexcept { retro_atomic_store_release_64(&data, static_cast<s64>(pack(data_t{}))); }

	shared_ptr<std::string> get_string_ptr() const noexcept;

	operator std::string() const noexcept
	{
		if (shared_ptr<std::string> ptr = get_string_ptr())
		{
			return *ptr;
		}

		return {};
	}

	explicit operator bool() const noexcept
	{
		return load().text_count != 0;
	}
};

enum system_progress_stop_state : u32
{
	stop_state_disabled = 0,
	stop_state_stopping,
	stop_state_continuous_savestate
};

extern progress_dialog_string_t g_progr_text;
// The progress counters: files and modules to do and done, and the files'
// sizes. Added to from the compiling threads, read by the dialog's server
extern retro_atomic_int_t g_progr_ftotal;
extern retro_atomic_int_t g_progr_fdone;
extern retro_atomic_64_t g_progr_ftotal_bits;
extern retro_atomic_64_t g_progr_fknown_bits;
extern retro_atomic_int_t g_progr_ptotal; // notified as it falls to none (its waiters: retro_waitaddr)
extern retro_atomic_int_t g_progr_pdone;
extern retro_atomic_int_t g_system_progress_canceled; // 0 or 1
extern retro_atomic_int_t g_system_progress_stopping; // a system_progress_stop_state

// A counter's value, and what is added to or taken from it
inline u32 progr_load(retro_atomic_int_t& v) { return static_cast<u32>(retro_atomic_load_acquire_int(&v)); }
inline u64 progr_load(retro_atomic_64_t& v) { return static_cast<u64>(retro_atomic_load_acquire_64(&v)); }
inline void progr_add(retro_atomic_int_t& v, u32 n) { retro_atomic_fetch_add_int(&v, static_cast<int>(n)); }
inline void progr_add(retro_atomic_64_t& v, u64 n) { retro_atomic_fetch_add_64(&v, static_cast<s64>(n)); }
inline void progr_sub(retro_atomic_int_t& v, u32 n) { retro_atomic_fetch_sub_int(&v, static_cast<int>(n)); }
inline void progr_sub(retro_atomic_64_t& v, u64 n) { retro_atomic_fetch_sub_64(&v, static_cast<s64>(n)); }

inline system_progress_stop_state progr_stopping()
{
	return static_cast<system_progress_stop_state>(retro_atomic_load_acquire_int(&g_system_progress_stopping));
}

// Initialize progress dialog (can be recursive)
class scoped_progress_dialog final
{
private:
	u32 m_text_index = 0;

public:
	scoped_progress_dialog(std::string text) noexcept;

	scoped_progress_dialog(const scoped_progress_dialog&) = delete;

	scoped_progress_dialog& operator=(const scoped_progress_dialog&) = delete;

	scoped_progress_dialog& operator=(std::string text) noexcept;

	~scoped_progress_dialog() noexcept;
};

struct progress_dialog_server
{
	void operator()();
	~progress_dialog_server();

	static constexpr auto thread_name = "Progress Dialog Server"sv;
};

struct progress_dialog_workaround
{
	// We don't want to show the native dialog during gameplay.
	retro_atomic_int_t show_overlay_message_only{0}; // 0 or 1
};
