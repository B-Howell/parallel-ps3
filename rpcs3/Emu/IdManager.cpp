#include "stdafx.h"
#include "IdManager.h"

namespace id_manager
{
	thread_local u32 g_id = 0;

	thread_local int g_hazard_record = -2;

	// Gives the thread's record back when the thread ends
	struct hazard_record_owner
	{
		int record = -1;

		~hazard_record_owner()
		{
			if (record >= 0)
			{
				lf_hazard_release(record);
			}
		}
	};

	int claim_hazard_record()
	{
		thread_local hazard_record_owner owner;

		owner.record = lf_hazard_claim();
		g_hazard_record = owner.record;
		return owner.record;
	}
}

template <>
bool serialize<std::shared_ptr<utils::serial>>(utils::serial& ar, std::shared_ptr<utils::serial>& o)
{
	if (!o)
	{
		o = std::make_shared<utils::serial>();
	}

	if (!ar.is_writing())
	{
		o->set_reading_state();
	}

	ar(o->data);
	return true;
}

std::vector<std::pair<u128, id_manager::typeinfo>>& id_manager::get_typeinfo_map()
{
	// Magic static
	static std::vector<std::pair<u128, id_manager::typeinfo>> s_map;
	return s_map;
}

id_manager::id_key_slot* idm::allocate_id(std::span<id_manager::id_key_slot> keys, retro_atomic_int_t& highest_index, u32 type_id, u32 dst_id, u32 base, u32 step, u32 count, bool uses_lowest_id, std::pair<u32, u32> invl_range)
{
	// A key is claimed with a compare-and-swap from free (type umax) to
	// the new ID: two threads making IDs at once never get the same one
	if (dst_id != (base ? 0 : u32{umax}))
	{
		// Fixed position construction
		const u32 index = id_manager::get_index(dst_id, base, step, count, invl_range);
		ensure(index < count);

		while (true)
		{
			const id_manager::id_key seen = keys[index].load();

			if (seen.type() != umax)
			{
				id_manager::raise_highest(highest_index, index + 1);
				return nullptr;
			}

			if (keys[index].claim(seen, id_manager::id_key(dst_id, type_id)))
			{
				break;
			}
		}

		id_manager::raise_highest(highest_index, index + 1);
		id_manager::g_id = dst_id;
		return &keys[index];
	}

	if (uses_lowest_id)
	{
		// Disable the optimization below (hurts accuracy for known cases)
		id_manager::raise_highest(highest_index, count);
	}
	else
	{
		// Try to emplace back: the index past the highest claimed first, then
		// its key (taken by a fixed construction meanwhile: the search below)
		for (u32 h = static_cast<u32>(retro_atomic_load_acquire_int(&highest_index)); h < count; h = static_cast<u32>(retro_atomic_load_acquire_int(&highest_index)))
		{
			if (!retro_atomic_cas_int(&highest_index, static_cast<int>(h), static_cast<int>(h + 1)))
			{
				continue;
			}

			const id_manager::id_key seen = keys[h].load();
			const u32 _next = base + step * h;

			if (seen.type() == umax && keys[h].claim(seen, id_manager::id_key(_next, type_id)))
			{
				id_manager::g_id = _next;
				return &keys[h];
			}

			break;
		}
	}

	// Check all IDs starting from "next id" (TODO)
	for (u32 i = 0, next = base; i < count; i++, next += step)
	{
		const auto ptr = &keys[i];

		// Look for free ID
		while (true)
		{
			const id_manager::id_key seen = ptr->load();

			if (seen.type() != umax)
			{
				break;
			}

			// Incremenet ID invalidation counter
			const u32 id = next | ((seen.value() + (1u << invl_range.first)) & (invl_range.second ? (((1u << invl_range.second) - 1) << invl_range.first) : 0));

			if (ptr->claim(seen, id_manager::id_key(id, type_id)))
			{
				id_manager::raise_highest(highest_index, i + 1);
				id_manager::g_id = id;
				return ptr;
			}
		}
	}

	// Out of IDs
	return nullptr;
}
