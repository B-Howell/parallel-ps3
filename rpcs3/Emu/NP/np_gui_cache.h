#pragma once

#include <map>
#include "Utilities/lf_post.h"

#include <queues/retro_retire.h>
#include <retro_atomic.h>
#include "Emu/Cell/Modules/sceNp.h"

template <>
struct std::less<SceNpRoomId>
{
	bool operator()(const SceNpRoomId& a, const SceNpRoomId& b) const
	{
		return (std::memcmp(a.opt, b.opt, sizeof(a.opt)) < 0);
	}
};

template <>
struct std::less<SceNpId>
{
	bool operator()(const SceNpId& a, const SceNpId& b) const
	{
		return (std::memcmp(a.handle.data, b.handle.data, sizeof(a.handle.data)) < 0);
	}
};

namespace np
{
	struct gui_room_member
	{
		SceNpUserInfo info;
		bool owner;
	};

	struct gui_room_cache
	{
		std::map<SceNpId, gui_room_member> members;
	};

	// The rooms the matching GUI is in, changed from the NP threads and read
	// by the game's, with no lock and no thread waiting for another. A change
	// is posted (lf_post.h) and applied by one thread at a time to the rooms,
	// which are the applier's; after each batch it publishes a snapshot of
	// them, and readers read that (retro_retire.h): a reader never waits on a
	// change, nor a change on a reader
	class gui_cache_manager
	{
	public:
		gui_cache_manager();
		~gui_cache_manager();
		gui_cache_manager(const gui_cache_manager&) = delete;
		gui_cache_manager& operator=(const gui_cache_manager&) = delete;

		void add_room(const SceNpRoomId& room_id);
		void del_room(const SceNpRoomId& room_id);
		void add_member(const SceNpRoomId& room_id, const SceNpMatchingRoomMember* user_info, bool new_member);
		void del_member(const SceNpRoomId& room_id, const SceNpMatchingRoomMember* user_info);

		error_code get_room_member_list(const SceNpRoomId& room_id, u32 buf_len, vm::ptr<void> data) const;

	private:
		struct change;
		struct snapshot;

		void post(change* c);
		static void apply(void* ctx, mpsc_stack_node_t* oldest);
		static void free_snapshot(mpsc_stack_node_t* node);

		// The applier's alone
		std::map<SceNpRoomId, gui_room_cache> rooms;

		lf_post_t m_changes{};
		retro_atomic_ptr_t m_published{};
		mutable retro_retire_t m_readers{};
	};
} // namespace np
