#pragma once

#include "util/ptr_table.h"
#include "util/types.hpp"

#include <memory>

// Objects owned by a 64-bit key in an open-addressed table (ptr_table.c), deleted with it: the overlay passes'
// programs, one a render pass, and the fonts and images an overlay draws (VKOverlays.h, GLOverlays.h)
template <typename T>
class owned_ptr_table
{
	ptr_table_t m_table{};

public:
	owned_ptr_table() = default;
	owned_ptr_table(const owned_ptr_table&) = delete;
	owned_ptr_table& operator=(const owned_ptr_table&) = delete;
	~owned_ptr_table() { clear(); }

	T* find(u64 key) const { return static_cast<T*>(ptr_table_find(&m_table, key)); }

	// The key not looked for: the caller has found it absent
	T* add(u64 key, std::unique_ptr<T>&& obj)
	{
		ensure(ptr_table_add(&m_table, key, obj.get()));
		return obj.release();
	}

	std::unique_ptr<T> take(u64 key) { return std::unique_ptr<T>(static_cast<T*>(ptr_table_remove(&m_table, key))); }

	// Gone through by slot: one taken out, the same slot is looked at again, as the entries after shift back
	u32 slots() const { return m_table.cap; }
	T* slot(u32 index) const { return static_cast<T*>(ptr_table_slot(&m_table, index)); }
	std::unique_ptr<T> take_slot(u32 index) { return std::unique_ptr<T>(static_cast<T*>(ptr_table_remove_at(&m_table, index))); }

	void clear()
	{
		for (u32 i = 0; i < m_table.cap; i++)
		{
			delete static_cast<T*>(m_table.vals[i]);
		}
		ptr_table_free(&m_table);
	}
};
