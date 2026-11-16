#include "stdafx.h"
#include "util/asm.hpp"
#include "np_gui_cache.h"

#include <vector>

LOG_CHANNEL(np_gui_cache);

namespace np
{
	// One change to the rooms, as posted
	struct gui_cache_manager::change : mpsc_stack_node_t
	{
		enum class kind
		{
			add_room,
			del_room,
			add_member,
			del_member,
		};

		kind what;
		SceNpRoomId room_id;
		gui_room_member member;
		bool new_member;
	};

	// The rooms as readers see them: made by the applier after each batch,
	// never changed after
	struct gui_cache_manager::snapshot : mpsc_stack_node_t
	{
		std::vector<std::pair<SceNpRoomId, std::vector<gui_room_member>>> rooms;
	};

	gui_cache_manager::gui_cache_manager()
	{
		lf_post_init(&m_changes);
		retro_atomic_ptr_init(&m_published, nullptr);
		retro_retire_init(&m_readers);
	}

	// Nothing else uses the cache by now
	gui_cache_manager::~gui_cache_manager()
	{
		delete static_cast<snapshot*>(retro_atomic_load_acquire_ptr(&m_published));
		retro_retire_drain(&m_readers, free_snapshot);

		for (mpsc_stack_node_t* n = mpsc_stack_drain(&m_changes.posted); n;)
		{
			mpsc_stack_node_t* next = n->next;
			delete static_cast<change*>(n);
			n = next;
		}
	}

	void gui_cache_manager::free_snapshot(mpsc_stack_node_t* node)
	{
		delete static_cast<snapshot*>(node);
	}

	void gui_cache_manager::post(change* c)
	{
		lf_post(&m_changes, c, apply, this);
	}

	void gui_cache_manager::apply(void* ctx, mpsc_stack_node_t* oldest)
	{
		auto& self = *static_cast<gui_cache_manager*>(ctx);

		while (oldest)
		{
			mpsc_stack_node_t* next = oldest->next;
			const change* c = static_cast<change*>(oldest);

			switch (c->what)
			{
			case change::kind::add_room:
			{
				const auto [room, inserted] = self.rooms.insert_or_assign(c->room_id, gui_room_cache{});
				if (!inserted)
					np_gui_cache.error("Cache mismatch: tried to insert room but it already existed");
				break;
			}
			case change::kind::del_room:
			{
				if (self.rooms.erase(c->room_id) != 1)
					np_gui_cache.error("Cache mismatch: tried to delete a room that wasn't in the cache");
				break;
			}
			case change::kind::add_member:
			{
				const auto room = self.rooms.find(c->room_id);
				if (room == self.rooms.end())
				{
					np_gui_cache.error("Cache mismatch: tried to add a member to a non-existing room");
					break;
				}

				const auto [_, inserted] = room->second.members.insert_or_assign(c->member.info.userId, c->member);

				if (c->new_member)
				{
					if (!inserted)
						np_gui_cache.error("Cache mismatch: tried to add a member but it was already in the room");
				}
				else if (inserted)
				{
					np_gui_cache.error("Cache mismatch: tried to update a member but it wasn't already in the room");
				}
				break;
			}
			case change::kind::del_member:
			{
				const auto room = self.rooms.find(c->room_id);
				if (room == self.rooms.end())
				{
					np_gui_cache.error("Cache mismatch: tried to remove a member from a non-existing room");
					break;
				}

				if (room->second.members.erase(c->member.info.userId) != 1)
					np_gui_cache.error("Cache mismatch: tried to remove a member but it wasn't in the room");
				break;
			}
			}

			delete c;
			oldest = next;
		}

		// What the readers see from now on
		auto* made = new snapshot();
		made->rooms.reserve(self.rooms.size());
		for (const auto& [id, room] : self.rooms)
		{
			auto& members = made->rooms.emplace_back(id, std::vector<gui_room_member>{}).second;
			members.reserve(room.members.size());
			for (const auto& [_, member] : room.members)
			{
				members.push_back(member);
			}
		}

		auto* old = static_cast<snapshot*>(retro_atomic_exchange_ptr(&self.m_published, made));
		retro_retire(&self.m_readers, old, free_snapshot);
	}

	void gui_cache_manager::add_room(const SceNpRoomId& room_id)
	{
		auto* c = new change{};
		c->what = change::kind::add_room;
		c->room_id = room_id;
		post(c);
	}

	void gui_cache_manager::del_room(const SceNpRoomId& room_id)
	{
		auto* c = new change{};
		c->what = change::kind::del_room;
		c->room_id = room_id;
		post(c);
	}

	void gui_cache_manager::add_member(const SceNpRoomId& room_id, const SceNpMatchingRoomMember* user_info, bool new_member)
	{
		auto* c = new change{};
		c->what = change::kind::add_member;
		c->room_id = room_id;
		c->member = gui_room_member{.info = user_info->user_info, .owner = !!user_info->owner};
		c->new_member = new_member;
		post(c);
	}

	void gui_cache_manager::del_member(const SceNpRoomId& room_id, const SceNpMatchingRoomMember* user_info)
	{
		auto* c = new change{};
		c->what = change::kind::del_member;
		c->room_id = room_id;
		c->member.info = user_info->user_info;
		post(c);
	}

	error_code gui_cache_manager::get_room_member_list(const SceNpRoomId& room_id, u32 buf_len, vm::ptr<void> data) const
	{
		retro_retire_enter(&m_readers);

		const auto* snap = static_cast<const snapshot*>(retro_atomic_load_acquire_ptr(const_cast<retro_atomic_ptr_t*>(&m_published)));
		const std::vector<gui_room_member>* members = nullptr;

		if (snap)
		{
			for (const auto& [id, room_members] : snap->rooms)
			{
				if (std::memcmp(&id, &room_id, sizeof(SceNpRoomId)) == 0)
				{
					members = &room_members;
					break;
				}
			}
		}

		const error_code result = [&]() -> error_code
		{
			if (!members)
				return SCE_NP_MATCHING_ERROR_ROOM_NOT_FOUND;

			const u32 room_size = ::narrow<u32>(utils::align(sizeof(SceNpMatchingRoomStatus), 8) + (utils::align(sizeof(SceNpMatchingRoomMember), 8) * members->size()));

			if (!data)
				return not_an_error(room_size);

			if (buf_len < room_size)
				return SCE_NP_MATCHING_ERROR_INSUFFICIENT_BUFFER;

			std::memset(data.get_ptr(), 0, buf_len);

			vm::ptr<SceNpMatchingRoomStatus> room_status = vm::cast(data.addr());
			std::memcpy(&room_status->id, &room_id, sizeof(SceNpRoomId));
			room_status->num = ::narrow<s32>(members->size());

			vm::ptr<SceNpMatchingRoomMember> cur_member_ptr{};

			for (const auto& member : *members)
			{
				if (!cur_member_ptr)
				{
					room_status->members = vm::cast(data.addr() + utils::align(sizeof(SceNpMatchingRoomStatus), 8));
					cur_member_ptr = room_status->members;
				}
				else
				{
					cur_member_ptr->next = vm::cast(cur_member_ptr.addr() + utils::align(sizeof(SceNpMatchingRoomMember), 8));
					cur_member_ptr = cur_member_ptr->next;
				}

				cur_member_ptr->owner = member.owner ? 1 : 0;
				std::memcpy(&cur_member_ptr->user_info, &member.info, sizeof(SceNpUserInfo));
			}

			return CELL_OK;
		}();

		retro_retire_leave(&m_readers, free_snapshot);
		return result;
	}
} // namespace np
