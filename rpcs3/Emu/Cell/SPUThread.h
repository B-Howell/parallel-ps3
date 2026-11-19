#pragma once

#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>
#include "Emu/CPU/CPUThread.h"
#include "Emu/CPU/Hypervisor.h"
#include "Emu/Cell/SPUInterpreter.h"
#include "Emu/Memory/vm.h"
#include "MFC.h"

#include "util/v128.hpp"
#include "util/logs.hpp"
#include "util/to_endian.hpp"

#include "Utilities/mutex.h"

#include "Loader/ELF.h"

#include <span>

LOG_CHANNEL(spu_log, "SPU");

struct lv2_event_queue;
struct lv2_spu_group;
struct lv2_int_tag;

namespace utils
{
	class shm;
}

// LUTs for SPU
extern const u32 spu_frest_fraction_lut[32];
extern const u32 spu_frest_exponent_lut[256];
extern const u32 spu_frsqest_fraction_lut[64];
extern const u32 spu_frsqest_exponent_lut[256];

// JIT Block
using spu_function_t = void(*)(spu_thread&, void*, u8*);

// SPU Channels
enum : u32
{
	SPU_RdEventStat     = 0,  // Read event status with mask applied
	SPU_WrEventMask     = 1,  // Write event mask
	SPU_WrEventAck      = 2,  // Write end of event processing
	SPU_RdSigNotify1    = 3,  // Signal notification 1
	SPU_RdSigNotify2    = 4,  // Signal notification 2
	SPU_WrDec           = 7,  // Write decrementer count
	SPU_RdDec           = 8,  // Read decrementer count
	SPU_RdEventMask     = 11, // Read event mask
	SPU_RdMachStat      = 13, // Read SPU run status
	SPU_WrSRR0          = 14, // Write SPU machine state save/restore register 0 (SRR0)
	SPU_RdSRR0          = 15, // Read SPU machine state save/restore register 0 (SRR0)
	SPU_WrOutMbox       = 28, // Write outbound mailbox contents
	SPU_RdInMbox        = 29, // Read inbound mailbox contents
	SPU_WrOutIntrMbox   = 30, // Write outbound interrupt mailbox contents (interrupting PPU)
	SPU_Set_Bkmk_Tag    = 69, // Causes an event that can be logged in the performance monitor logic if enabled in the SPU
	SPU_PM_Start_Ev     = 70, // Starts the performance monitor event if enabled
	SPU_PM_Stop_Ev      = 71, // Stops the performance monitor event if enabled
};

// MFC Channels
enum : u32
{
	MFC_WrMSSyncReq     = 9,  // Write multisource synchronization request
	MFC_RdTagMask       = 12, // Read tag mask
	MFC_LSA             = 16, // Write local memory address command parameter
	MFC_EAH             = 17, // Write high order DMA effective address command parameter
	MFC_EAL             = 18, // Write low order DMA effective address command parameter
	MFC_Size            = 19, // Write DMA transfer size command parameter
	MFC_TagID           = 20, // Write tag identifier command parameter
	MFC_Cmd             = 21, // Write and enqueue DMA command with associated class ID
	MFC_WrTagMask       = 22, // Write tag mask
	MFC_WrTagUpdate     = 23, // Write request for conditional or unconditional tag status update
	MFC_RdTagStat       = 24, // Read tag status with mask applied
	MFC_RdListStallStat = 25, // Read DMA list stall-and-notify status
	MFC_WrListStallAck  = 26, // Write DMA list stall-and-notify acknowledge
	MFC_RdAtomicStat    = 27, // Read completion status of last completed immediate MFC atomic update command
};

// SPU Events
enum : u32
{
	SPU_EVENT_MS = 0x1000, // Multisource Synchronization event
	SPU_EVENT_A  = 0x800,  // Privileged Attention event
	SPU_EVENT_LR = 0x400,  // Lock Line Reservation Lost event
	SPU_EVENT_S1 = 0x200,  // Signal Notification Register 1 available
	SPU_EVENT_S2 = 0x100,  // Signal Notification Register 2 available
	SPU_EVENT_LE = 0x80,   // SPU Outbound Mailbox available
	SPU_EVENT_ME = 0x40,   // SPU Outbound Interrupt Mailbox available
	SPU_EVENT_TM = 0x20,   // SPU Decrementer became negative (?)
	SPU_EVENT_MB = 0x10,   // SPU Inbound mailbox available
	SPU_EVENT_QV = 0x8,    // MFC SPU Command Queue available
	SPU_EVENT_SN = 0x2,    // MFC List Command stall-and-notify event
	SPU_EVENT_TG = 0x1,    // MFC Tag Group status update event

	SPU_EVENT_IMPLEMENTED  = SPU_EVENT_LR | SPU_EVENT_TM | SPU_EVENT_SN | SPU_EVENT_S1 | SPU_EVENT_S2, // Mask of implemented events
	SPU_EVENT_INTR_IMPLEMENTED = SPU_EVENT_SN,
	SPU_EVENT_INTR_BUSY_CHECK = SPU_EVENT_IMPLEMENTED & ~SPU_EVENT_INTR_IMPLEMENTED,
};

// SPU Class 0 Interrupts
enum : u64
{
	SPU_INT0_STAT_DMA_ALIGNMENT_INT   = (1ull << 0),
	SPU_INT0_STAT_INVALID_DMA_CMD_INT = (1ull << 1),
	SPU_INT0_STAT_SPU_ERROR_INT       = (1ull << 2),
};

// SPU Class 2 Interrupts
enum : u64
{
	SPU_INT2_STAT_MAILBOX_INT                  = (1ull << 0),
	SPU_INT2_STAT_SPU_STOP_AND_SIGNAL_INT      = (1ull << 1),
	SPU_INT2_STAT_SPU_HALT_OR_STEP_INT         = (1ull << 2),
	SPU_INT2_STAT_DMA_TAG_GROUP_COMPLETION_INT = (1ull << 3),
	SPU_INT2_STAT_SPU_MAILBOX_THRESHOLD_INT    = (1ull << 4),
};

enum : u32
{
	SPU_RUNCNTL_STOP_REQUEST = 0,
	SPU_RUNCNTL_RUN_REQUEST  = 1,
};

// SPU Status Register bits (not accurate)
enum : u32
{
	SPU_STATUS_STOPPED             = 0x0,
	SPU_STATUS_RUNNING             = 0x1,
	SPU_STATUS_STOPPED_BY_STOP     = 0x2,
	SPU_STATUS_STOPPED_BY_HALT     = 0x4,
	SPU_STATUS_WAITING_FOR_CHANNEL = 0x8,
	SPU_STATUS_SINGLE_STEP         = 0x10,
	SPU_STATUS_IS_ISOLATED         = 0x80,
};

enum : s32
{
	SPU_LS_SIZE = 0x40000,
};

enum : u32
{
	SYS_SPU_THREAD_BASE_LOW  = 0xf0000000,
	SYS_SPU_THREAD_OFFSET    = 0x100000,
	SYS_SPU_THREAD_SNR1      = 0x5400c,
	SYS_SPU_THREAD_SNR2      = 0x5C00c,
};

enum
{
	MFC_LSA_offs = 0x3004,
	MFC_EAH_offs = 0x3008,
	MFC_EAL_offs = 0x300C,
	MFC_Size_Tag_offs = 0x3010,
	MFC_Class_CMD_offs = 0x3014,
	MFC_CMDStatus_offs = 0x3014,
	MFC_QStatus_offs = 0x3104,
	Prxy_QueryType_offs = 0x3204,
	Prxy_QueryMask_offs = 0x321C,
	Prxy_TagStatus_offs = 0x322C,
	SPU_Out_MBox_offs = 0x4004,
	SPU_In_MBox_offs = 0x400C,
	SPU_MBox_Status_offs = 0x4014,
	SPU_RunCntl_offs = 0x401C,
	SPU_Status_offs = 0x4024,
	SPU_NPC_offs = 0x4034,
	SPU_RdSigNotify1_offs = 0x1400C,
	SPU_RdSigNotify2_offs = 0x1C00C,
};

enum : u32
{
	RAW_SPU_BASE_ADDR   = 0xE0000000,
	RAW_SPU_OFFSET      = 0x00100000,
	RAW_SPU_LS_OFFSET   = 0x00000000,
	RAW_SPU_PROB_OFFSET = 0x00040000,

	SPU_FAKE_BASE_ADDR  = 0xE8000000,
};

struct spu_channel_op_state
{
	u8 old_count;
	u8 count;
	bool notify;
	bool op_done;
};

struct alignas(16) spu_channel
{
	// Low 32 bits contain value. The recompilers read and write it at the
	// channel's own address (offset 0), 8 bytes
	retro_atomic_64_t data{0};

	// Pending value to be inserted when it is possible in pop() or pop_wait()
	retro_atomic_64_t jostling_value{0};

	u64 load() const { return static_cast<u64>(retro_atomic_load_acquire_64(const_cast<retro_atomic_64_t*>(&data))); }
	u64 load_jostling() const { return static_cast<u64>(retro_atomic_load_acquire_64(const_cast<retro_atomic_64_t*>(&jostling_value))); }
	void set_jostling(u64 value) { retro_atomic_store_release_64(&jostling_value, static_cast<s64>(value)); }
	u64 take_jostling() { return static_cast<u64>(retro_atomic_exchange_64(&jostling_value, 0)); }

	// The word waited on: data's upper half (the wait, occupy and count bits)
	const void* wait_word() const
	{
		return reinterpret_cast<const char*>(&data) + (std::endian::native == std::endian::little ? 4 : 0);
	}

	u32 wait_word_of(u64 value) const { return static_cast<u32>(value >> 32); }

	// @f changes the value it is given, and may say whether it did (a
	// bool): the change made with a compare-and-swap, f run again if
	// another came first. Gives back the value before and f's result, as
	// atomic_t's fetch_op did
	template <typename F>
	auto fetch_data(F&& f)
	{
		for (;;)
		{
			const u64 old = load();
			u64 value = old;

			if constexpr (std::is_void_v<std::invoke_result_t<F&, u64&>>)
			{
				f(value);

				if (value == old || retro_atomic_cas_64(&data, static_cast<s64>(old), static_cast<s64>(value)))
					return old;
			}
			else
			{
				const bool done = f(value);

				if (!done || value == old || retro_atomic_cas_64(&data, static_cast<s64>(old), static_cast<s64>(value)))
					return std::pair<u64, bool>{old, done};
			}
		}
	}

public:
	static constexpr u32 off_wait  = 32;
	static constexpr u32 off_occupy = 33;
	static constexpr u32 off_count = 63;
	static constexpr u64 bit_wait  = 1ull << off_wait;
	static constexpr u64 bit_occupy = 1ull << off_occupy;
	static constexpr u64 bit_count = 1ull << off_count;
	static constexpr u64 occupy_ored_wait = bit_wait | bit_occupy; 

	// Returns true on success
	bool try_push(u32 value)
	{
		return fetch_data([value](u64& data)
		{
			if (!(data & bit_count)) [[likely]]
			{
				data = bit_count | value;
				return true;
			}

			return false;
		}).second;
	}

	// Push unconditionally, may require notification
	// Performing bitwise OR with previous value if specified, otherwise overwiting it
	// Returns old count and new count
	spu_channel_op_state push(u32 value, bool to_or = false, bool postpone_notify = false)
	{
		while (true)
		{
			const auto [old, pushed_to_data] = fetch_data([&](u64& data)
			{
				if (data & bit_occupy)
				{
					return false;
				}

				if (to_or)
				{
					data = bit_count | (static_cast<u32>(data) | value);
				}
				else
				{
					data = bit_count | value;
				}

				return true;
			});

			if (!pushed_to_data)
			{
				// Insert the pending value in special storage for waiting SPUs, leave no time in which the channel has data
				if (!retro_atomic_cas_64(&jostling_value, static_cast<s64>(bit_occupy), static_cast<s64>(value)))
				{
					// Other thread has inserted a value through jostling_value, retry
					continue;
				}

				// Turn off waiting bit manually (must succeed because waiting bit can only be resetted by the thread pushed to jostling_value)
				if (~static_cast<u64>(retro_atomic_fetch_and_64(&this->data, static_cast<s64>(~occupy_ored_wait))) & bit_wait)
				{
					// Could be fatal or at emulation stopping, to be checked by the caller
					ensure(false);
				}

				// Fallthrough to notification
				ensure(old & bit_wait);
			}

			if (old & bit_wait)
			{
				if (!postpone_notify)
				{
					retro_waitaddr_notify_one(wait_word());
				}
			}

			// Return true if count has changed from 0 to 1, this condition is considered satisfied even if we pushed a value directly to the special storage for waiting SPUs
			return { (old & bit_count) == 0, 1, (old & bit_wait) != 0, true };
		}
	}

	void notify()
	{
		retro_waitaddr_notify_one(wait_word());
	}

	// Returns true on success
	bool try_pop(u32& out)
	{
		return fetch_data([&out](u64& data)
		{
			if (data & bit_count) [[likely]]
			{
				out = static_cast<u32>(data);
				data = 0;
				return true;
			}

			return false;
		}).second;
	}

	// Reading without modification
	bool try_read(u32& out) const
	{
		const u64 old = load();
		out = static_cast<u32>(old);

		if (old & bit_count) [[likely]]
		{
			return true;
		}

		return false;
	}

	// Pop unconditionally (loading last value), may require notification
	// If the SPU tries to insert a value, do it instead the SPU
	u32 pop()
	{
		// Value is not cleared and may be read again
		constexpr u64 mask = bit_count | bit_occupy;

		const u64 old = fetch_data([&](u64& data)
		{
			if ((data & mask) == mask)
			{
				// Insert the pending value, leave no time in which the channel has no data
				data = bit_count | static_cast<u32>(load_jostling());
				return;
			}

			data &= ~(mask | bit_wait);
		});

		if (old & bit_wait)
		{
			retro_waitaddr_notify_one(wait_word());
		}

		return static_cast<u32>(old);
	}

	// Waiting for channel pop state availability, actually popping if specified
	s64 pop_wait(cpu_thread& spu, bool pop = true);

	// Waiting for channel push state availability, actually pushing if specified
	bool push_wait(cpu_thread& spu, u32 value, bool push = true);

	void set_value(u32 value, bool count = true)
	{
		retro_atomic_store_release_64(&data, static_cast<s64>(u64{count} << off_count | value));
	}

	u32 get_value() const
	{
		return static_cast<u32>(load());
	}

	u32 get_count() const
	{
		return (load() & bit_count) ? 1 : 0;
	}
};

static_assert(sizeof(spu_channel) == 16 && offsetof(spu_channel, data) == 0 && sizeof(spu_channel::data) == 8);

struct spu_channel_4_t
{
	struct alignas(16) sync_var_t
	{
		u8 waiting;
		u8 count;
		u16 value3_inval;
		u32 value0;
		u32 value1;
		u32 value2;
	};

	// The recompilers read the first word at the channel's own address,
	// and the whole of it with one 16-byte load
	mutable retro_atomic_128_t values{};
	retro_atomic_64_t jostling_value{0};
	retro_atomic_int_t value3{0}; // u32

	static sync_var_t from_u128(retro_u128_t v) { return std::bit_cast<sync_var_t>(v); }
	static retro_u128_t to_u128(const sync_var_t& v) { return std::bit_cast<retro_u128_t>(v); }

	sync_var_t load_values() const { return from_u128(retro_atomic_load_acquire_128(&values)); }
	void store_values(const sync_var_t& v) { retro_atomic_store_release_128(&values, to_u128(v)); }

	// The waiting byte (the low byte of the first word): read, or taken
	// (set to 0, the one before given back)
	u8 waiting() const { return static_cast<u8>(retro_atomic_load_acquire_128_lo(&values)); }

	u32 get_value3() const { return static_cast<u32>(retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t*>(&value3))); }
	u64 take_jostling() { return static_cast<u64>(retro_atomic_exchange_64(&jostling_value, 0)); }

	// @f changes the values it is given, and gives back what it likes: the
	// change made with a compare-and-swap, f run again if another came
	// first. Gives back f's result, as atomic_op did
	template <typename F>
	auto values_op(F&& f)
	{
		for (;;)
		{
			const retro_u128_t old = retro_atomic_load_acquire_128(&values);
			sync_var_t data = from_u128(old);
			auto result = f(data);
			const retro_u128_t now = to_u128(data);

			if ((now.lo == old.lo && now.hi == old.hi) || retro_atomic_cas_128(&values, old, now))
				return result;
		}
	}

	u8 take_waiting()
	{
		return values_op([](sync_var_t& data)
		{
			return std::exchange(data.waiting, u8{0});
		});
	}

	// As fetch_op did: the values before, and whether f changed them (a
	// bool f returns false to change nothing); with a void f, just the
	// values before
	template <typename F>
	auto values_fetch_op(F&& f)
	{
		for (;;)
		{
			const retro_u128_t old = retro_atomic_load_acquire_128(&values);
			sync_var_t data = from_u128(old);

			if constexpr (std::is_void_v<std::invoke_result_t<F&, sync_var_t&>>)
			{
				f(data);
				const retro_u128_t now = to_u128(data);

				if ((now.lo == old.lo && now.hi == old.hi) || retro_atomic_cas_128(&values, old, now))
					return from_u128(old);
			}
			else
			{
				const bool done = f(data);
				const retro_u128_t now = to_u128(data);

				if (!done || (now.lo == old.lo && now.hi == old.hi) || retro_atomic_cas_128(&values, old, now))
					return std::pair<sync_var_t, bool>{from_u128(old), done};
			}
		}
	}

	static constexpr u32 off_wait  = 0;
	static constexpr u32 off_occupy = 7;
	static constexpr u64 bit_wait  = 1ull << off_wait;
	static constexpr u64 bit_occupy = 1ull << off_occupy;
	static constexpr u64 jostling_flag = 1ull << 63;

	void clear()
	{
		store_values({});
		retro_atomic_store_release_64(&jostling_value, 0);
		retro_atomic_store_release_int(&value3, 0);
	}

	// push unconditionally (overwriting latest value), returns true if needs signaling
	// returning if could be aborted (operation failed unexpectedly)
	spu_channel_op_state push(u32 value, bool postpone_notify = false);

	void notify()
	{
		retro_waitaddr_notify_one(&values);
	}

	// returns non-zero value on success: queue size before removal
	uint try_pop(u32& out)
	{
		return values_op([&](sync_var_t& data)
		{
			const uint result = data.count;

			if (result != 0)
			{
				data.waiting = 0;
				data.count--;
				out = data.value0;

				data.value0 = data.value1;
				data.value1 = data.value2;
				data.value2 = this->get_value3();
			}

			return result;
		});
	}

	// Returns [previous count, value] (if aborted 0 count is returned)
	std::pair<u32, u32> pop_wait(cpu_thread& spu, bool pop_value = true);

	// returns current queue size without modification
	uint try_read(u32 (&out)[4]) const
	{
		const sync_var_t data = load_values();
		const uint result = data.count;

		if (result != 0)
		{
			out[0] = data.value0;
			out[1] = data.value1;
			out[2] = data.value2;
			out[3] = get_value3();
		}

		return result;
	}

	u32 get_count() const
	{
		return static_cast<u8>(retro_atomic_load_acquire_128_lo(&values) >> 8);
	}

	// One value put in when the mailbox is empty (a reply the SPU reserves
	// before its syscall's answer): false when it is not
	bool try_set_if_empty(u32 value0)
	{
		return values_op([&](sync_var_t& data)
		{
			if (data.count)
			{
				return false;
			}

			data = {0, 1, data.value3_inval, value0, 0, 0};
			return true;
		});
	}

	void set_values(u32 count, u32 value0, u32 value1 = 0, u32 value2 = 0, u32 value3 = 0)
	{
		store_values({ 0, static_cast<u8>(count), {}, value0, value1, value2 });
		retro_atomic_store_release_int(&this->value3, static_cast<int>(value3));
	}
};

static_assert(sizeof(spu_channel_4_t::sync_var_t) == 16 && offsetof(spu_channel_4_t, values) == 0 && alignof(spu_channel_4_t) == 16);

struct spu_int_ctrl_t
{
	mutable retro_atomic_64_t mask{0}; // u64
	mutable retro_atomic_64_t stat{0}; // u64

	u64 get_mask() const { return static_cast<u64>(retro_atomic_load_acquire_64(&mask)); }
	u64 get_stat() const { return static_cast<u64>(retro_atomic_load_acquire_64(&stat)); }

	shared_ptr<struct lv2_int_tag> tag;

	// What set() goes by, with no lock: the tag's ID, and the tag itself
	// to tell it from another object that got the ID later. Written with
	// tag, by set_tag().
	retro_atomic_int_t tag_id{};
	retro_atomic_ptr_t tag_obj{};

	void set_tag(shared_ptr<struct lv2_int_tag> new_tag);

	void set(u64 ints);

	void clear(u64 ints)
	{
		retro_atomic_fetch_and_64(&stat, static_cast<s64>(~ints));
	}

	void clear()
	{
		retro_atomic_store_release_64(&mask, 0);
		retro_atomic_store_release_64(&stat, 0);
		set_tag({});
	}
};

struct spu_imm_table_t
{
	v128 sldq_pshufb[32]; // table for SHLQBYBI, SHLQBY, SHLQBYI instructions
	v128 srdq_pshufb[32]; // table for ROTQMBYBI, ROTQMBY, ROTQMBYI instructions
	v128 rldq_pshufb[16]; // table for ROTQBYBI, ROTQBY, ROTQBYI instructions

	class scale_table_t
	{
		std::array<v128, 155 + 174> m_data;

	public:
		scale_table_t();

		FORCE_INLINE const v128& operator [](s32 scale) const
		{
			return m_data[scale + 155];
		}
	}
	const scale;

	spu_imm_table_t();
};

extern const spu_imm_table_t g_spu_imm;

enum FPSCR_EX
{
	//Single-precision exceptions
	FPSCR_SOVF = 1 << 2,    //Overflow
	FPSCR_SUNF = 1 << 1,    //Underflow
	FPSCR_SDIFF = 1 << 0,   //Different (could be IEEE non-compliant)
	//Double-precision exceptions
	FPSCR_DOVF = 1 << 13,   //Overflow
	FPSCR_DUNF = 1 << 12,   //Underflow
	FPSCR_DINX = 1 << 11,   //Inexact
	FPSCR_DINV = 1 << 10,   //Invalid operation
	FPSCR_DNAN = 1 << 9,    //NaN
	FPSCR_DDENORM = 1 << 8, //Denormal
};

//Is 128 bits, but bits 0-19, 24-28, 32-49, 56-60, 64-81, 88-92, 96-115, 120-124 are unused
class SPU_FPSCR
{
public:
	u32 _u32[4]{};

	SPU_FPSCR() {}

	std::string ToString() const
	{
		return fmt::format("%08x%08x%08x%08x", _u32[3], _u32[2], _u32[1], _u32[0]);
	}

	void Reset()
	{
		memset(this, 0, sizeof(*this));
	}

	//slice -> 0 - 1 (double-precision slice index)
	//NOTE: slices follow v128 indexing, i.e. slice 0 is RIGHT end of register!
	//roundTo -> FPSCR_RN_*
	void setSliceRounding(u8 slice, u8 roundTo)
	{
		int shift = 8 + 2*slice;
		//rounding is located in the left end of the FPSCR
		this->_u32[3] = (this->_u32[3] & ~(3 << shift)) | (roundTo << shift);
	}

	//Slice 0 or 1
	u8 checkSliceRounding(u8 slice) const
	{
		switch(slice)
		{
		case 0:
			return this->_u32[3] >> 8 & 0x3;

		case 1:
			return this->_u32[3] >> 10 & 0x3;

		default:
			fmt::throw_exception("Unexpected slice value (%d)", slice);
		}
	}

	//Single-precision exception flags (all 4 slices)
	//slice -> slice number (0-3)
	//exception: FPSCR_S* bitmask
	void setSinglePrecisionExceptionFlags(u8 slice, u32 exceptions)
	{
		_u32[slice] |= exceptions;
	}

	//Single-precision divide-by-zero flags (all 4 slices)
	//slice -> slice number (0-3)
	void setDivideByZeroFlag(u8 slice)
	{
		_u32[0] |= 1 << (8 + slice);
	}

	//Double-precision exception flags
	//slice -> slice number (0-1)
	//exception: FPSCR_D* bitmask
	void setDoublePrecisionExceptionFlags(u8 slice, u32 exceptions)
	{
		_u32[1 + slice] |= exceptions;
	}

	// Write the FPSCR
	void Write(const v128& r)
	{
		_u32[3] = r._u32[3] & 0x00000F07;
		_u32[2] = r._u32[2] & 0x00003F07;
		_u32[1] = r._u32[1] & 0x00003F07;
		_u32[0] = r._u32[0] & 0x00000F07;
	}

	// Read the FPSCR
	void Read(v128& r) const
	{
		r._u32[3] = _u32[3];
		r._u32[2] = _u32[2];
		r._u32[1] = _u32[1];
		r._u32[0] = _u32[0];
	}
};

enum class spu_type : u32
{
	threaded,
	raw,
	isolated,
};

struct spu_memory_segment_dump_data
{
	u32 ls_addr;
	const u8* src_addr;
	u32 segment_size;
	u32 flags = umax;
};

enum class spu_debugger_mode : u32
{
	_default,
	is_float,
	is_decimal,

	max_mode,
};

enum class spu_block_hash : u64 {};

class spu_thread : public cpu_thread
{
public:
	virtual void dump_regs(std::string&, std::any& custom_data) const override;
	virtual std::string dump_callstack() const override;
	virtual std::vector<std::pair<u32, u32>> dump_callstack_list() const override;
	virtual void dump_misc(std::string& ret, std::any& custom_data) const override;
	virtual void cpu_task() override final;
	virtual void cpu_on_stop() override;
	virtual void cpu_return() override;
	virtual void cpu_work() override;
	virtual ~spu_thread() override;
	void cleanup();
	void cpu_init();
	void init_spu_decoder();

	static const u32 id_base = 0x02000000; // TODO (used to determine thread type)
	static const u32 id_step = 1;
	static const u32 id_count = (0xFFFC0000 - SPU_FAKE_BASE_ADDR) / SPU_LS_SIZE;

	spu_thread(lv2_spu_group* group, u32 index, std::string_view name, u32 lv2_id, bool is_isolated = false, u32 option = 0);

	spu_thread(const spu_thread&) = delete;
	spu_thread& operator=(const spu_thread&) = delete;

	using cpu_thread::operator=;

	SAVESTATE_INIT_POS(5);

	spu_thread(utils::serial& ar, lv2_spu_group* group = nullptr);
	void serialize_common(utils::serial& ar);
	void save(utils::serial& ar);
	bool savable() const { return get_type() != spu_type::threaded; } // Threaded SPUs are saved as part of the SPU group

	u32 pc = 0;
	u32 dbg_step_pc = 0;

	// May be used internally by recompilers.
	u32 base_pc = 0;

	// May be used by recompilers.
	u8* memory_base_addr = vm::g_base_addr;
	u8* memory_sudo_addr = vm::g_sudo_addr;
	u8* reserv_base_addr = vm::g_reservations;

	// General-Purpose Registers
	std::array<v128, 128> gpr{};
	SPU_FPSCR fpscr{};

	// MFC command data
	spu_mfc_cmd ch_mfc_cmd{};

	// MFC command queue
	spu_mfc_cmd mfc_queue[16]{};
	u32 mfc_size = 0;
	u32 mfc_barrier = -1;
	u32 mfc_fence = -1;

	// Timestamp of the first postponed command (transfers shuffling related)
	u64 mfc_last_timestamp = 0;

	// MFC proxy command data, written a register at a time by the PPU, and
	// which of its registers were written since the last command: one
	// 128-bit word, each register's write and the command's start a
	// compare-and-swap of it. The command as LV2 lays it out (spu_mfc_cmd,
	// 16 bytes); the local store address never needs its top 14 bits, so
	// the five written bits live there
	retro_atomic_128_t mfc_prxy_word{};
	retro_atomic_int_t mfc_prxy_mask{0}; // u32

	static constexpr u32 mfc_prxy_written_shift = 27;
	static constexpr u8 mfc_prxy_written_all = 0x1f;

	enum : u8
	{
		mfc_prxy_written_lsa = 1 << 0,
		mfc_prxy_written_eal = 1 << 1,
		mfc_prxy_written_eah = 1 << 2,
		mfc_prxy_written_tag_size = 1 << 3,
		mfc_prxy_written_cmd = 1 << 4,
	};

	static spu_mfc_cmd mfc_prxy_unpack(retro_u128_t word, u8* written)
	{
		spu_mfc_cmd cmd = std::bit_cast<spu_mfc_cmd>(word);
		*written = static_cast<u8>(cmd.lsa >> mfc_prxy_written_shift);
		cmd.lsa &= (1u << mfc_prxy_written_shift) - 1;
		return cmd;
	}

	static retro_u128_t mfc_prxy_pack(spu_mfc_cmd cmd, u8 written)
	{
		cmd.lsa = (cmd.lsa & ((1u << mfc_prxy_written_shift) - 1)) | (u32{written} << mfc_prxy_written_shift);
		return std::bit_cast<retro_u128_t>(cmd);
	}

	// One register written: f changes the command and gives the bit it wrote
	template <typename F>
	static void mfc_prxy_write(retro_atomic_128_t& word, F&& f)
	{
		for (;;)
		{
			const retro_u128_t seen = retro_atomic_load_acquire_128(&word);
			u8 written = 0;
			spu_mfc_cmd cmd = mfc_prxy_unpack(seen, &written);
			written |= f(cmd);

			if (retro_atomic_cas_128(&word, seen, mfc_prxy_pack(cmd, written)))
			{
				return;
			}
		}
	}

	// The command, when all its registers were written, taken and the
	// written bits cleared in one change
	static bool mfc_prxy_take(retro_atomic_128_t& word, spu_mfc_cmd* out)
	{
		for (;;)
		{
			const retro_u128_t seen = retro_atomic_load_acquire_128(&word);
			u8 written = 0;
			const spu_mfc_cmd cmd = mfc_prxy_unpack(seen, &written);

			if (written != mfc_prxy_written_all)
			{
				return false;
			}

			if (retro_atomic_cas_128(&word, seen, mfc_prxy_pack(cmd, 0)))
			{
				*out = cmd;
				return true;
			}
		}
	}

	// Reservation Data
	u64 rtime = 0;
	alignas(64) std::byte rdata[128]{};
	u32 raddr = 0;
	const decltype(rdata)* resrv_mem{};

	// Range Lock pointer
	atomic_t<u64, 128>* range_lock{};

	u32 srr0 = 0;
	u32 ch_tag_upd = 0;
	u32 ch_tag_mask = 0;
	spu_channel ch_tag_stat;
	u32 ch_stall_mask = 0;
	spu_channel ch_stall_stat;
	spu_channel ch_atomic_stat;

	spu_channel_4_t ch_in_mbox{};

	spu_channel ch_out_mbox;
	spu_channel ch_out_intr_mbox;

	u64 snr_config = 0; // SPU SNR Config Register

	spu_channel ch_snr1{}; // SPU Signal Notification Register 1
	spu_channel ch_snr2{}; // SPU Signal Notification Register 2

	union ch_events_t
	{
		u64 all;
		bf_t<u64, 0, 16> events;
		bf_t<u64, 16, 8> locks;
		bf_t<u64, 30, 1> waiting;
		bf_t<u64, 31, 1> count;
		bf_t<u64, 32, 32> mask;
	};

	static_assert(sizeof(ch_events_t) == 8);

	// One word; the recompilers read it at its own offset, 8 bytes
	mutable retro_atomic_64_t ch_events{0}; // a ch_events_t

	ch_events_t load_events() const { return std::bit_cast<ch_events_t>(static_cast<u64>(retro_atomic_load_acquire_64(&ch_events))); }

	// @f changes the events it is given, and gives back what it likes: the
	// change made with a compare-and-swap, f run again if another came
	// first. Gives back f's result, as atomic_op did
	template <typename F>
	auto events_op(F&& f)
	{
		for (;;)
		{
			const s64 old = retro_atomic_load_acquire_64(&ch_events);
			ch_events_t ev = std::bit_cast<ch_events_t>(static_cast<u64>(old));

			if constexpr (std::is_void_v<std::invoke_result_t<F&, ch_events_t&>>)
			{
				f(ev);

				if (static_cast<s64>(ev.all) == old || retro_atomic_cas_64(&ch_events, old, static_cast<s64>(ev.all)))
					return;
			}
			else
			{
				auto result = f(ev);

				if (static_cast<s64>(ev.all) == old || retro_atomic_cas_64(&ch_events, old, static_cast<s64>(ev.all)))
					return result;
			}
		}
	}

	// As fetch_op did: the events before, and f's bool (false: nothing changed)
	template <typename F>
	std::pair<ch_events_t, bool> events_fetch_op(F&& f)
	{
		for (;;)
		{
			const s64 old = retro_atomic_load_acquire_64(&ch_events);
			ch_events_t ev = std::bit_cast<ch_events_t>(static_cast<u64>(old));
			const bool done = f(ev);

			if (!done || static_cast<s64>(ev.all) == old || retro_atomic_cas_64(&ch_events, old, static_cast<s64>(ev.all)))
				return {std::bit_cast<ch_events_t>(static_cast<u64>(old)), done};
		}
	}
	bool interrupts_enabled = false;

	u64 ch_dec_start_timestamp = 0; // timestamp of writing decrementer value
	u32 ch_dec_value = 0; // written decrementer value
	bool is_dec_frozen = false;
	std::pair<u32, u32> read_dec() const; // Read decrementer

	retro_atomic_int_t run_ctrl{0}; // SPU Run Control register, a u32 (only provided to get latest data written)
	// Counted up each time a raw SPU stops running, with a wake: what a stop
	// request waits on, so a run request after the stop cannot keep it
	// waiting (run requests take no lock)
	retro_atomic_int_t stop_count{0}; // u32

	void count_stop()
	{
		retro_atomic_fetch_add_int(&stop_count, 1);
		retro_waitaddr_notify_all(&stop_count);
	}

	struct alignas(8) status_npc_sync_var
	{
		u32 status; // SPU Status register
		u32 npc; // SPU Next Program Counter register
	};

	static_assert(sizeof(status_npc_sync_var) == 8);

	mutable retro_atomic_64_t status_npc{0}; // a status_npc_sync_var

	status_npc_sync_var load_status_npc() const { return std::bit_cast<status_npc_sync_var>(static_cast<u64>(retro_atomic_load_acquire_64(&status_npc))); }
	void store_status_npc(status_npc_sync_var v) { retro_atomic_store_release_64(&status_npc, std::bit_cast<s64>(v)); }
	void notify_status_npc() { retro_waitaddr_notify_one(&status_npc); }

	// @f changes the status and NPC it is given: the change made with a
	// compare-and-swap. A void f: as atomic_op did. A bool f: the value
	// before and whether f changed it, as fetch_op did
	template <typename F>
	auto status_npc_op(F&& f)
	{
		for (;;)
		{
			const s64 old = retro_atomic_load_acquire_64(&status_npc);
			status_npc_sync_var v = std::bit_cast<status_npc_sync_var>(old);

			if constexpr (std::is_void_v<std::invoke_result_t<F&, status_npc_sync_var&>>)
			{
				f(v);

				if (std::bit_cast<s64>(v) == old || retro_atomic_cas_64(&status_npc, old, std::bit_cast<s64>(v)))
					return;
			}
			else
			{
				const bool done = f(v);

				if (!done || std::bit_cast<s64>(v) == old || retro_atomic_cas_64(&status_npc, old, std::bit_cast<s64>(v)))
					return std::pair<status_npc_sync_var, bool>{std::bit_cast<status_npc_sync_var>(old), done};
			}
		}
	}
	std::array<spu_int_ctrl_t, 3> int_ctrl{}; // SPU Class 0, 1, 2 Interrupt Management

	// Event Queue Keys for SPU Thread: an entry is claimed (binding or
	// unbinding: 1) and published (bound: 2) with compare-and-swap, its key
	// and queue written between, so the SPU reads a bound entry whole with
	// no lock and no turn
	struct spuq_entry
	{
		retro_atomic_int_t state{};
		retro_atomic_int_t key{};
		atomic_ptr<lv2_event_queue> queue;
	};

	enum : int
	{
		spuq_free = 0,
		spuq_changing = 1,
		spuq_bound = 2,
	};

	std::array<spuq_entry, 32> spuq{};

	// The queue bound to @key, if it still exists
	shared_ptr<lv2_event_queue> get_spuq(u32 key);

	// SPU Ports: connected and disconnected with compare-and-swap
	atomic_ptr<lv2_event_queue> spup[64];
	spu_channel exit_status{}; // Threaded SPU exit status (not a channel, but the interface fits)
	retro_atomic_int_t last_exit_status{0}; // u32: value to be written in exit_status after checking group termination
	lv2_spu_group* const group; // SPU Thread Group (access by the spu threads in the group only! From other threads obtain a shared pointer to group using group ID)
	const u32 index; // SPU index
	const spu_type thread_type;
	std::shared_ptr<utils::shm> shm; // SPU memory
	const std::add_pointer_t<u8> ls; // SPU LS pointer
	const u32 option; // sys_spu_thread_initialize option
	u32 lv2_id; // The actual id that is used by syscalls
	u32 spurs_addr = 0;
	bool spurs_waited = false;
	bool spurs_entered_wait = false;
	u64 spurs_wait_duration_last = 0;
	u64 spurs_average_task_duration = 0;
	u64 spurs_last_task_timestamp = 0;
	static constexpr u64 spurs_task_count_to_calculate = 10;

	spu_thread* next_cpu{}; // LV2 thread queues' node link

	// Thread name
	atomic_ptr<std::string> spu_tname;

	std::unique_ptr<class spu_recompiler_base> jit; // Recompiler instance

	u64 block_counter = 0;
	u64 block_recover = 0;
	u64 block_failure = 0;

	rpcs3::hypervisor_context_t hv_ctx; // NOTE: The offset within the class must be within the first 1MiB

	u64 ftx = 0; // Failed transactions
	u64 stx = 0; // Succeeded transactions (pure counters)

	u64 last_ftsc = 0;
	u64 last_ftime = 0;
	u32 last_faddr = 0;
	u64 last_fail = 0;
	u64 last_succ = 0;
	u64 last_gtsc = 0;
	u32 last_getllar = umax; // LS address of last GETLLAR (if matches current GETLLAR we can let the thread rest)
	u32 last_getllar_gpr1 = umax;
	u32 last_getllar_addr = umax;
	u32 last_getllar_lsa = umax;
	u32 getllar_spin_count = 0;
	u32 getllar_busy_waiting_switch = umax; // umax means the test needs evaluation, otherwise it's a boolean
	u64 getllar_evaluate_time = 0;

	u32 eventstat_raddr = 0;
	u32 eventstat_getllar = 0;
	u64 eventstat_block_counter = 0;
	u64 eventstat_spu_group_restart = 0;
	u64 eventstat_spin_count = 0;
	u64 eventstat_evaluate_time = 0;
	u32 eventstat_busy_waiting_switch = 0;

	std::vector<mfc_cmd_dump> mfc_history;
	u64 mfc_dump_idx = 0;
	static constexpr u32 max_mfc_dump_idx = 4096;

	struct alignas(16) raw_spu_log_stats_t
	{
		u32 mmio_offset = umax;
		u32 mmio_value = umax;
		u64 mmio_time = umax;
	};

	atomic_t<raw_spu_log_stats_t> mmio_stats{};

	bool in_cpu_work = false;
	bool allow_interrupts_in_cpu_work = false;
	u8 cpu_work_iteration_count = 0;

	std::array<v128, 0x4000> stack_mirror; // Return address information
	std::array<u32, 8> raddr_busy_wait_addr{}; // Return address information

	const char* current_func{}; // Current STOP or RDCH blocking function
	u64 start_time{}; // Starting time of STOP or RDCH bloking function
	retro_atomic_int_t lv2_first_sleep{}; // Still to call its first lv2 sleep (lv2_obj::set_future_sleep)
	bool unsavable = false; // Flag indicating whether saving the spu thread state is currently unsafe

	retro_atomic_int_t debugger_mode{0}; // a spu_debugger_mode

	// PC-based breakpoint list
	// Nothing in the core sets a breakpoint (the debugger is not in it):
	// the list stays empty and the flag 0
	std::array<u8, SPU_LS_SIZE / 4 / 8> local_breakpoints{};
	retro_atomic_int_t has_active_local_bps{0}; // 0 or 1
	u32 current_bp_pc = umax;
	bool stop_flag_removal_protection = false;

	std::array<std::array<u8, 4>, SPU_LS_SIZE / 128> getllar_wait_time{};
	std::array<std::array<u8, 16>, SPU_LS_SIZE / 128> eventstat_wait_time{};
 
	void push_snr(u32 number, u32 value);
	static void do_dma_transfer(spu_thread* _this, const spu_mfc_cmd& args, u8* ls);
	bool do_dma_check(const spu_mfc_cmd& args);
	bool do_list_transfer(spu_mfc_cmd& args);
	void do_putlluc(const spu_mfc_cmd& args);
	bool do_putllc(const spu_mfc_cmd& args);
	bool do_mfc(bool can_escape = true, bool must_finish = true);
	u32 get_mfc_completed() const;

	bool process_mfc_cmd();
	ch_events_t get_events(u64 mask_hint = umax, bool waiting = false, bool reading = false);
	void set_events(u32 bits);
	void set_interrupt_status(bool enable);
	bool check_mfc_interrupts(u32 next_pc);
	static bool is_exec_code(u32 addr, std::span<const u8> ls_ptr, u32 base_addr = 0, bool avoid_dead_code = false, bool is_range_limited = false); // A hint, do not rely on it for true execution compatibility
	static std::vector<u32> discover_functions(u32 base_addr, std::span<const u8> ls, bool is_known_addr, u32 /*entry*/);
	u32 get_ch_count(u32 ch);
	s64 get_ch_value(u32 ch);
	bool set_ch_value(u32 ch, u32 value);
	bool stop_and_signal(u32 code);
	void halt();

	void fast_call(u32 ls_addr);

	std::array<std::shared_ptr<utils::serial>, 32> rewind_captures; // shared_ptr to avoid header inclusion
	u8 current_rewind_capture_idx = 0;

	static spu_exec_object capture_memory_as_elf(std::span<spu_memory_segment_dump_data> segs, u32 pc_hint = umax);
	bool capture_state();
	bool try_load_debug_capture();
	void wakeup_delay(u32 div = 1) const;

	// Convert specified SPU LS address to a pointer of specified (possibly converted to BE) type
	template<typename T>
	to_be_t<T>* _ptr(u32 lsa) const
	{
		return reinterpret_cast<to_be_t<T>*>(ls + (lsa % SPU_LS_SIZE));
	}

	// Convert specified SPU LS address to a reference of specified (possibly converted to BE) type
	template<typename T>
	to_be_t<T>& _ref(u32 lsa) const
	{
		return *_ptr<T>(lsa);
	}

	spu_type get_type() const
	{
		return thread_type;
	}

	u32 vm_offset() const
	{
		return group ? SPU_FAKE_BASE_ADDR + SPU_LS_SIZE * (id & 0xffffff) : RAW_SPU_BASE_ADDR + RAW_SPU_OFFSET * index;
	}

	static u8* map_ls(utils::shm& shm, void* ptr = nullptr);

	// Returns true if reservation existed but was just discovered to be lost
	// It is safe to use on any address, even if not directly accessed by SPU (so it's slower)
	// Optionally pass a known allocated address for internal optimization (the current Effective-Address of the MFC command)
	bool reservation_check(u32 addr, const decltype(rdata)& data, u32 current_eal = 0) const;
	static bool reservation_check(u32 addr, u32 hash, atomic_t<u64, 128>* range_lock);
	usz register_cache_line_waiter(u32 addr);
	void deregister_cache_line_waiter(usz index);

	bool read_reg(const u32 addr, u32& value);
	bool write_reg(const u32 addr, const u32 value);
	static bool test_is_problem_state_register_offset(u32 offset, bool for_read, bool for_write) noexcept;

	static retro_atomic_int_t g_raw_spu_ctr; // u32
	static retro_atomic_int_t g_raw_spu_id[5]; // u32 ids, 1 while being taken, 0 when free
	static retro_atomic_int_t g_spu_work_count; // u32

	// Raised by one if below @limit, as atomic_t's try_inc did
	static bool try_inc_below(retro_atomic_int_t* word, u32 limit)
	{
		for (u32 value = static_cast<u32>(retro_atomic_load_acquire_int(word)); value < limit; value = static_cast<u32>(retro_atomic_load_acquire_int(word)))
		{
			if (retro_atomic_cas_int(word, static_cast<int>(value), static_cast<int>(value + 1)))
				return true;
		}

		return false;
	}

	static atomic_t<u64, 128> g_spu_waiters_by_value[6];

	static u32 find_raw_spu(u32 id)
	{
		if (id < std::size(g_raw_spu_id)) [[likely]]
		{
			return static_cast<u32>(retro_atomic_load_acquire_int(&g_raw_spu_id[id]));
		}

		return -1;
	}

	// For named_thread ctor
	const struct thread_name_t
	{
		const spu_thread* _this;

		operator std::string() const;
	} thread_name{ this };

	union spu_prio_t
	{
		u64 all;
		bf_t<s64, 0, 9> prio; // Thread priority (0..3071) (firs 9-bits)
		bf_t<s64, 9, 55> order; // Thread enqueue order (TODO, last 52-bits)
	};

	// For lv2_obj::schedule<spu_thread>: the group's priority
	spu_prio_t get_prio() const;

	template <typename Func>
	void prio_op(Func&& func)
	{
		static_cast<std::conditional_t<std::is_void_v<Func>, Func, decltype(group)>>(group)->prio_op(std::forward<Func>(func));
	}
};

class spu_function_logger
{
	spu_thread& spu;

public:
	spu_function_logger(spu_thread& spu, const char* func) noexcept;

	~spu_function_logger() noexcept
	{
		if (!spu.is_stopped())
		{
			spu.start_time = 0;
		}
	}
};
