#pragma once

#include <map>
#include <optional>

#include <functional>

#include "Utilities/lf_post.h"
#include <queues/retro_retire.h>
#include <retro_atomic.h>

#include "Emu/Cell/Modules/sceNp.h"
#include "Emu/Cell/Modules/sceNp2.h"

namespace np
{
	struct userinfo_cache
	{
		SceNpId npId{};
		std::optional<SceNpOnlineName> onlineName;
		std::optional<SceNpAvatarUrl> avatarUrl;
	};

	struct memberbin_cache
	{
		memberbin_cache(const SceNpMatching2RoomMemberBinAttrInternal* sce_memberbin);

		SceNpMatching2AttributeId id;
		CellRtcTick updateDate;
		std::vector<u8> data;
	};

	struct member_cache
	{
		member_cache(const SceNpMatching2RoomMemberDataInternal* sce_member);

		userinfo_cache userInfo;
		CellRtcTick joinDate;
		SceNpMatching2RoomMemberId memberId;
		SceNpMatching2TeamId teamId;
		SceNpMatching2RoomGroupId group_id;
		SceNpMatching2NatType natType;
		SceNpMatching2FlagAttr flagAttr;

		std::map<SceNpMatching2AttributeId, memberbin_cache> bins;
	};

	struct password_info_cache
	{
		bool with_password = false;
		SceNpMatching2SessionPassword password{};
	};

	struct room_cache
	{
		void update(const SceNpMatching2RoomDataInternal* sce_roomdata);

		u32 num_slots                                    = 0;
		SceNpMatching2RoomPasswordSlotMask mask_password = 0;
		std::optional<SceNpMatching2SessionPassword> password;

		std::map<SceNpMatching2RoomGroupId, SceNpMatching2RoomGroup> groups;
		std::map<SceNpMatching2RoomMemberId, member_cache> members;

		SceNpMatching2SignalingOptParam opt_param{};

		bool owner = false;
	};

	// The Matching2 rooms, changed from the NP threads and read by the game's,
	// with no lock and no thread waiting for another: a change is posted
	// (lf_post.h) and applied by one thread at a time to the rooms, which are
	// the applier's; after each batch it publishes a copy of them, and the
	// readers read that (retro_retire.h)
	class cache_manager
	{
	public:
		cache_manager();
		~cache_manager();
		cache_manager(const cache_manager&) = delete;
		cache_manager& operator=(const cache_manager&) = delete;

		void insert_room(const SceNpMatching2RoomDataInternal* sce_roomdata);
		bool add_member(SceNpMatching2RoomId room_id, const SceNpMatching2RoomMemberDataInternal* sce_roommemberdata);
		bool del_member(SceNpMatching2RoomId room_id, SceNpMatching2RoomMemberId member_id);
		void update_password(SceNpMatching2RoomId room_id, const std::optional<SceNpMatching2SessionPassword>& password);
		void update_opt_param(SceNpMatching2RoomId room_id, const SceNpMatching2SignalingOptParam* sce_opt_param);

		std::pair<error_code, std::optional<SceNpMatching2RoomSlotInfo>> get_slots(SceNpMatching2RoomId room_id) const;
		std::pair<error_code, std::vector<SceNpMatching2RoomMemberId>> get_memberids(u64 room_id, s32 sort_method) const;
		std::pair<error_code, std::optional<SceNpMatching2SessionPassword>> get_password(SceNpMatching2RoomId room_id) const;
		std::pair<error_code, std::optional<SceNpMatching2SignalingOptParam>> get_opt_param(SceNpMatching2RoomId room_id) const;
		error_code get_member_and_attrs(SceNpMatching2RoomId room_id, SceNpMatching2RoomMemberId member_id, const std::vector<SceNpMatching2AttributeId>& binattrs_list, SceNpMatching2RoomMemberDataInternal* ptr_member, u32 addr_data, u32 size_data, bool include_onlinename, bool include_avatarurl) const;
		std::pair<error_code, std::optional<SceNpId>> get_npid(u64 room_id, u16 member_id) const;
		std::optional<u16> get_memberid(u64 room_id, const SceNpId& npid) const;

	private:
		using rooms_t = std::map<SceNpMatching2RoomId, room_cache>;

		struct change : mpsc_stack_node_t
		{
			std::function<void(rooms_t&)> fn;
		};

		struct snapshot : mpsc_stack_node_t
		{
			rooms_t rooms;
		};

		void post(std::function<void(rooms_t&)> fn);
		static void apply(void* ctx, mpsc_stack_node_t* oldest);
		static void free_snapshot(mpsc_stack_node_t* node);

		// f on the rooms as last published, held while it runs
		template <typename F>
		auto read(F&& f) const
		{
			retro_retire_enter(&m_readers);
			const auto* snap = static_cast<const snapshot*>(retro_atomic_load_acquire_ptr(const_cast<retro_atomic_ptr_t*>(&m_published)));
			static const rooms_t none;
			auto result = f(snap ? snap->rooms : none);
			retro_retire_leave(&m_readers, free_snapshot);
			return result;
		}

		// The applier's alone
		rooms_t m_rooms;

		lf_post_t m_changes{};
		retro_atomic_ptr_t m_published{};
		mutable retro_retire_t m_readers{};
	};
} // namespace np
