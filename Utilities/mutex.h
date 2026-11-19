#pragma once

#include <mutex>
#include "util/types.hpp"
#include <retro_atomic.h>
#include "rw_word.h"

// Shared mutex with small size (u32): the word and its paths are C89
// (rw_word.h/.c); a broken state (an underflow or overflow) is fatal
class shared_mutex final
{
	mutable retro_atomic_int_t m_value{0};

public:
	constexpr shared_mutex() = default;

	bool try_lock_shared()
	{
		return rw_try_lock_shared(&m_value);
	}

	void lock_shared()
	{
		ensure(rw_lock_shared(&m_value)); // "shared_mutex underflow/overflow"
	}

	void unlock_shared()
	{
		ensure(rw_unlock_shared(&m_value)); // "shared_mutex underflow"
	}

	bool try_lock()
	{
		return rw_try_lock(&m_value);
	}

	void lock()
	{
		ensure(rw_lock(&m_value)); // "shared_mutex underflow/overflow"
	}

	void unlock()
	{
		ensure(rw_unlock(&m_value)); // "shared_mutex underflow"
	}

	bool try_lock_upgrade()
	{
		return rw_try_lock_upgrade(&m_value);
	}

	void lock_upgrade()
	{
		ensure(rw_lock_upgrade(&m_value)); // "shared_mutex overflow"
	}

	void lock_downgrade()
	{
		rw_lock_downgrade(&m_value);
	}

	// Wait for lockability without locking
	void lock_unlock()
	{
		rw_lock_unlock(&m_value);
	}

	// Check whether can immediately obtain an exclusive (writer) lock
	bool is_free() const
	{
		return rw_value(&m_value) == 0;
	}

	// Check whether can immediately obtain a shared (reader) lock
	bool is_lockable() const
	{
		return rw_value(&m_value) < RW_ONE - 1;
	}

	bool has_waiters() const
	{
		return rw_value(&m_value) > RW_ONE;
	}
};

// Simplified shared (reader) lock implementation.
class reader_lock final
{
	shared_mutex& m_mutex;
	bool m_upgraded = false;

public:
	reader_lock(const reader_lock&) = delete;

	reader_lock& operator=(const reader_lock&) = delete;

	explicit reader_lock(shared_mutex& mutex)
		: m_mutex(mutex)
	{
		m_mutex.lock_shared();
	}

	// One-way lock upgrade; note that the observed state could have been changed
	void upgrade()
	{
		if (!m_upgraded)
		{
			m_mutex.lock_upgrade();
			m_upgraded = true;
		}
	}

	// Try to upgrade; if it succeeds, the observed state has NOT been changed
	bool try_upgrade()
	{
		return m_upgraded || (m_upgraded = m_mutex.try_lock_upgrade());
	}

	~reader_lock()
	{
		m_upgraded ? m_mutex.unlock() : m_mutex.unlock_shared();
	}
};
