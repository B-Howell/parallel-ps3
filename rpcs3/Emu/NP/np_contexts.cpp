#include "stdafx.h"
#include "np_contexts.h"

#include "Emu/Cell/PPUCallback.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/Modules/cellSysutil.h"
#include "np_helpers.h"
#include "Emu/Cell/timers.hpp"

LOG_CHANNEL(sceNp2);

generic_async_transaction_context::generic_async_transaction_context(const SceNpCommunicationId& communicationId, const SceNpCommunicationPassphrase& passphrase, u64 timeout)
	: communicationId(communicationId), passphrase(passphrase), timeout(timeout)
	, idm_id(idm::last_id())
{
}

generic_async_transaction_context::~generic_async_transaction_context()
{
}

// np_requests.cpp: the request's slot let go of, if it still holds it
extern void np_transaction_finished(u32 req_id);

void generic_async_transaction_context::start(u32 id)
{
	req_id = id;
	deadline_us = get_system_time() + timeout;
}

bool generic_async_transaction_context::claim()
{
	return retro_atomic_cas_int(&state, 0, 1) != 0;
}

void generic_async_transaction_context::finish(error_code err)
{
	result_value = err;
	retro_atomic_store_release_int(&state, 2);
	retro_waitaddr_notify_all(&state);
	np_transaction_finished(req_id);
}

void generic_async_transaction_context::complete_now(error_code err)
{
	result_value = err;
	retro_atomic_store_release_int(&state, 2);
	retro_waitaddr_notify_all(&state);
}

void generic_async_transaction_context::reset()
{
	retro_atomic_store_release_int(&state, 0);
}

std::optional<s32> generic_async_transaction_context::get_transaction_status()
{
	if (retro_atomic_load_acquire_int(&state) != 2 && get_system_time() >= deadline_us && claim())
	{
		finish(SCE_NP_COMMUNITY_ERROR_TIMEOUT);
	}

	if (retro_atomic_load_acquire_int(&state) != 2)
	{
		return std::nullopt;
	}

	return result_value;
}

void generic_async_transaction_context::abort_transaction()
{
	if (claim())
	{
		finish(SCE_NP_COMMUNITY_ERROR_ABORTED);
	}
}

error_code generic_async_transaction_context::wait_for_completion()
{
	// Woken as it is finished, or at its deadline - no condition variable
	for (int s = retro_atomic_load_acquire_int(&state); s != 2; s = retro_atomic_load_acquire_int(&state))
	{
		const u64 now = get_system_time();

		if (now >= deadline_us)
		{
			if (claim())
			{
				finish(SCE_NP_COMMUNITY_ERROR_TIMEOUT);
			}
			else if (retro_atomic_load_acquire_int(&state) == 1)
			{
				// Being finished: done in a moment, woken by it
				thread_ctrl::wait_on_custom<1>([&](retro_waitaddr_t* list) { list[0] = {&state, static_cast<u32>(1u)}; });
			}

			continue;
		}

		thread_ctrl::wait_on_custom<1>([&](retro_waitaddr_t* list) { list[0] = {&state, static_cast<u32>(static_cast<u32>(s))}; }, deadline_us - now);
	}

	return not_an_error(result_value);
}

tus_ctx::tus_ctx(vm::cptr<SceNpCommunicationId> communicationId, vm::cptr<SceNpCommunicationPassphrase> passphrase)
{
	ensure(communicationId && np::validate_communication_id(*communicationId), "tus_ctx::tus_ctx: Invalid SceNpCommunicationId");
	memcpy(&this->communicationId, communicationId.get_ptr(), sizeof(SceNpCommunicationId));
	memcpy(&this->passphrase, passphrase.get_ptr(), sizeof(SceNpCommunicationPassphrase));
}
s32 create_tus_context(vm::cptr<SceNpCommunicationId> communicationId, vm::cptr<SceNpCommunicationPassphrase> passphrase)
{
	s32 tus_id = idm::make<tus_ctx>(communicationId, passphrase);

	if (tus_id == id_manager::id_traits<tus_ctx>::invalid)
	{
		return SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
	}

	return static_cast<s32>(tus_id);
}
bool destroy_tus_context(s32 ctx_id)
{
	return idm::remove<tus_ctx>(static_cast<u32>(ctx_id));
}

tus_transaction_ctx::tus_transaction_ctx(const shared_ptr<tus_ctx>& tus)
	: generic_async_transaction_context(tus->communicationId, tus->passphrase, tus->timeout)
{
}

s32 create_tus_transaction_context(const shared_ptr<tus_ctx>& tus)
{
	s32 tus_id = idm::make<tus_transaction_ctx>(tus);

	if (tus_id == id_manager::id_traits<tus_transaction_ctx>::invalid)
	{
		return SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
	}

	return static_cast<s32>(tus_id);
}
bool destroy_tus_transaction_context(s32 ctx_id)
{
	return idm::remove<tus_transaction_ctx>(static_cast<u32>(ctx_id));
}

score_ctx::score_ctx(vm::cptr<SceNpCommunicationId> communicationId, vm::cptr<SceNpCommunicationPassphrase> passphrase)
{
	ensure(communicationId && np::validate_communication_id(*communicationId), "score_ctx::score_ctx: Invalid SceNpCommunicationId");
	memcpy(&this->communicationId, communicationId.get_ptr(), sizeof(SceNpCommunicationId));
	memcpy(&this->passphrase, passphrase.get_ptr(), sizeof(SceNpCommunicationPassphrase));
}
s32 create_score_context(vm::cptr<SceNpCommunicationId> communicationId, vm::cptr<SceNpCommunicationPassphrase> passphrase)
{
	s32 score_id = idm::make<score_ctx>(communicationId, passphrase);

	if (score_id == id_manager::id_traits<score_ctx>::invalid)
	{
		return SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
	}

	return static_cast<s32>(score_id);
}
bool destroy_score_context(s32 ctx_id)
{
	return idm::remove<score_ctx>(static_cast<u32>(ctx_id));
}

score_transaction_ctx::score_transaction_ctx(const shared_ptr<score_ctx>& score)
	: generic_async_transaction_context(score->communicationId, score->passphrase, score->timeout)
{
	pcId = score->pcId;
}

s32 create_score_transaction_context(const shared_ptr<score_ctx>& score)
{
	s32 trans_id = idm::make<score_transaction_ctx>(score);

	if (trans_id == id_manager::id_traits<score_transaction_ctx>::invalid)
	{
		return SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
	}

	return static_cast<s32>(trans_id);
}
bool destroy_score_transaction_context(s32 ctx_id)
{
	return idm::remove<score_transaction_ctx>(static_cast<u32>(ctx_id));
}

match2_ctx::match2_ctx(vm::cptr<SceNpCommunicationId> communicationId, vm::cptr<SceNpCommunicationPassphrase> passphrase, s32 option)
{
	ensure(communicationId && np::validate_communication_id(*communicationId), "match2_ctx::match2_ctx: Invalid SceNpCommunicationId");
	memcpy(&this->communicationId, communicationId.get_ptr(), sizeof(SceNpCommunicationId));
	memcpy(&this->passphrase, passphrase.get_ptr(), sizeof(SceNpCommunicationPassphrase));

	include_onlinename = option & SCE_NP_MATCHING2_CONTEXT_OPTION_USE_ONLINENAME;
	include_avatarurl = option & SCE_NP_MATCHING2_CONTEXT_OPTION_USE_AVATARURL;
}
u16 create_match2_context(vm::cptr<SceNpCommunicationId> communicationId, vm::cptr<SceNpCommunicationPassphrase> passphrase, s32 option)
{
	sceNp2.notice("Creating match2 context with communicationId: <%s>", std::string_view(communicationId->data, 9));
	return static_cast<u16>(idm::make<match2_ctx>(communicationId, passphrase, option));
}
bool destroy_match2_context(u16 ctx_id)
{
	return idm::remove<match2_ctx>(static_cast<u32>(ctx_id));
}
bool check_match2_context(u16 ctx_id)
{
	return (idm::check_unlocked<match2_ctx>(ctx_id) != nullptr);
}
shared_ptr<match2_ctx> get_match2_context(u16 ctx_id)
{
	return idm::get_unlocked<match2_ctx>(ctx_id);
}

lookup_title_ctx::lookup_title_ctx(vm::cptr<SceNpCommunicationId> communicationId)
{
	ensure(communicationId && np::validate_communication_id(*communicationId), "lookup_title_ctx::lookup_title_ctx: Invalid SceNpCommunicationId");
	memcpy(&this->communicationId, communicationId.get_ptr(), sizeof(SceNpCommunicationId));
}
s32 create_lookup_title_context(vm::cptr<SceNpCommunicationId> communicationId)
{
	return static_cast<s32>(idm::make<lookup_title_ctx>(communicationId));
}
bool destroy_lookup_title_context(s32 ctx_id)
{
	return idm::remove<lookup_title_ctx>(static_cast<u32>(ctx_id));
}

lookup_transaction_ctx::lookup_transaction_ctx(s32 lt_ctx)
{
	this->lt_ctx = lt_ctx;
}
s32 create_lookup_transaction_context(s32 lt_ctx)
{
	return static_cast<s32>(idm::make<lookup_transaction_ctx>(lt_ctx));
}
bool destroy_lookup_transaction_context(s32 ctx_id)
{
	return idm::remove<lookup_transaction_ctx>(static_cast<u32>(ctx_id));
}

commerce2_ctx::commerce2_ctx(u32 version, vm::cptr<SceNpId> npid, vm::ptr<SceNpCommerce2Handler> handler, vm::ptr<void> arg)
{
	this->version = version;
	memcpy(&this->npid, npid.get_ptr(), sizeof(SceNpId));
	this->context_callback = handler;
	this->context_callback_param = arg;
}
s32 create_commerce2_context(u32 version, vm::cptr<SceNpId> npid, vm::ptr<SceNpCommerce2Handler> handler, vm::ptr<void> arg)
{
	return static_cast<s32>(idm::make<commerce2_ctx>(version, npid, handler, arg));
}
bool destroy_commerce2_context(u32 ctx_id)
{
	return idm::remove<commerce2_ctx>(static_cast<u32>(ctx_id));
}
shared_ptr<commerce2_ctx> get_commerce2_context(u16 ctx_id)
{
	return idm::get_unlocked<commerce2_ctx>(ctx_id);
}

signaling_ctx::signaling_ctx(vm::ptr<SceNpId> npid, vm::ptr<SceNpSignalingHandler> handler, vm::ptr<void> arg)
{
	memcpy(&this->npid, npid.get_ptr(), sizeof(SceNpId));
	this->handler = handler;
	this->arg = arg;
}
s32 create_signaling_context(vm::ptr<SceNpId> npid, vm::ptr<SceNpSignalingHandler> handler, vm::ptr<void> arg)
{
	return static_cast<s32>(idm::make<signaling_ctx>(npid, handler, arg));
}
bool destroy_signaling_context(u32 ctx_id)
{
	return idm::remove<signaling_ctx>(static_cast<u32>(ctx_id));
}
shared_ptr<signaling_ctx> get_signaling_context(u32 ctx_id)
{
	return idm::get_unlocked<signaling_ctx>(ctx_id);
}

matching_ctx::matching_ctx(vm::ptr<SceNpId> npId, vm::ptr<SceNpMatchingHandler> handler, vm::ptr<void> arg)
{
	memcpy(&this->npid, npId.get_ptr(), sizeof(SceNpId));
	this->handler = handler;
	this->arg = arg;
}
void matching_ctx::queue_callback(u32 req_id, s32 event, s32 error_code) const
{
	if (handler)
	{
		sysutil_register_cb([=, handler = this->handler, ctx_id = this->ctx_id, arg = this->arg](ppu_thread& cb_ppu) -> s32
			{
				handler(cb_ppu, ctx_id, req_id, event, error_code, arg);
				return 0;
			});
	}
}
void matching_ctx::queue_gui_callback(s32 event, s32 error_code) const
{
	if (gui_handler)
	{
		sysutil_register_cb([=, gui_handler = this->gui_handler, ctx_id = this->ctx_id, gui_arg = this->gui_arg](ppu_thread& cb_ppu) -> s32
			{
				gui_handler(cb_ppu, ctx_id, event, error_code, gui_arg);
				return 0;
			});
	}
}
s32 create_matching_context(vm::ptr<SceNpId> npId, vm::ptr<SceNpMatchingHandler> handler, vm::ptr<void> arg)
{
	const u32 ctx_id = idm::make<matching_ctx>(npId, handler, arg);
	auto ctx = get_matching_context(ctx_id);
	ctx->ctx_id = ctx_id;
	return static_cast<s32>(ctx_id);
}
shared_ptr<matching_ctx> get_matching_context(u32 ctx_id)
{
	return idm::get_unlocked<matching_ctx>(ctx_id);
}
bool destroy_matching_context(u32 ctx_id)
{
	return idm::remove<matching_ctx>(static_cast<u32>(ctx_id));
}
