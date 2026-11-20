#include "stdafx.h"
#include "VKCommandStream.h"
#include "VKResourceManager.h"
#include "vkutils/descriptors.h"
#include "vkutils/sync.h"
#include "VKLibretro.h"

#include "Emu/IdManager.h"
#include "Emu/RSX/RSXOffload.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/system_config.h"

namespace vk
{
	// What keeps the queue's submits apart, the core's threads' and the
	// frontend's: the frontend's own lock, which the libretro Vulkan
	// interface lends with the queue (every submit, and every wait for the
	// device to go idle, of the core's goes through here, so it alone keeps
	// the core's threads apart too). The core's own lock only for a frontend
	// that lends none.
	shared_mutex g_submit_mutex;

	void acquire_global_submit_lock()
	{
		if (vk::libretro::has_queue_lock())
		{
			vk::libretro::lock_queue();
			return;
		}

		g_submit_mutex.lock();
	}

	void release_global_submit_lock()
	{
		if (vk::libretro::has_queue_lock())
		{
			vk::libretro::unlock_queue();
			return;
		}

		g_submit_mutex.unlock();
	}

	FORCE_INLINE
	static void queue_submit_impl(const queue_submit_t& submit_info)
	{
		ensure(submit_info.pfence);
		acquire_global_submit_lock();
		VkSubmitInfo info
		{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.pNext = nullptr,
			.waitSemaphoreCount = submit_info.wait_semaphores_count,
			.pWaitSemaphores = submit_info.wait_semaphores.data(),
			.pWaitDstStageMask = submit_info.wait_stages.data(),
			.commandBufferCount = 1,
			.pCommandBuffers = &submit_info.commands,
			.signalSemaphoreCount = submit_info.signal_semaphores_count,
			.pSignalSemaphores = submit_info.signal_semaphores.data()
		};

		vkQueueSubmit(submit_info.queue, 1, &info, submit_info.pfence->handle);
		release_global_submit_lock();

		// Signal fence
		submit_info.pfence->signal_flushed();
	}

	void queue_submit(const queue_submit_t& submit_info, VkBool32 flush)
	{
		rsx::get_current_renderer()->get_stats().submit_count++;

		// Access to this method must be externally synchronized.
		// Offloader is guaranteed to never call this for async flushes.
		vk::descriptors::flush();

		if (!flush && g_cfg.video.multithreaded_rsx)
		{
			auto packet = new queue_submit_t(submit_info);
			g_fxo->get<rsx::dma_manager>().backend_ctrl(rctrl_queue_submit, packet);
		}
		else
		{
			queue_submit_impl(submit_info);
		}
	}

	void queue_submit(const queue_submit_t* packet)
	{
		// Flush-only version used by asynchronous submit processing (MTRSX)
		queue_submit_impl(*packet);
	}

	void driver_manager_t::operator()()
	{
		while (thread_ctrl::state() != thread_state::aborting)
		{
			const u32 wake_token = static_cast<u32>(retro_atomic_load_acquire_int(&m_wake_event));
			const u64 last_eid = static_cast<u64>(retro_atomic_load_acquire_64(&m_last_completed_eid));

			if (const u64 eid = static_cast<u64>(retro_atomic_load_acquire_64(&m_eid_ctr)); eid != last_eid)
			{
				vk::get_resource_manager()->eid_completed(eid);

				retro_atomic_store_release_64(&m_last_completed_eid, static_cast<s64>(eid));
				retro_atomic_fetch_add_int(&m_completed_signal, 1);
				retro_waitaddr_notify_all(&m_completed_signal);
			}

			// Woken by a completed event, or this thread told to end
			thread_ctrl::wait_on_custom<1>([&](retro_waitaddr_t* list) { list[0] = {&m_wake_event, static_cast<u32>(wake_token)}; });
		}
	}

	void driver_manager_t::notify_completed(u64 eid)
	{
		// Raised to eid, never lowered
		for (s64 value = retro_atomic_load_acquire_64(&m_eid_ctr); static_cast<u64>(value) < eid;)
		{
			if (retro_atomic_cas_64(&m_eid_ctr, value, static_cast<s64>(eid)))
				break;
			value = retro_atomic_load_acquire_64(&m_eid_ctr);
		}

		retro_atomic_fetch_add_int(&m_wake_event, 1);
		retro_waitaddr_notify_one(&m_wake_event);
	}

	void driver_manager_t::drain()
	{
		const u64 target_eid = static_cast<u64>(retro_atomic_load_acquire_64(&m_eid_ctr)); //<- Last request at time of calling

		// Now, we spam queue wake until our EID is reached
		while (true)
		{
			const u32 completion_token = static_cast<u32>(retro_atomic_load_acquire_int(&m_completed_signal));
			if (static_cast<u64>(retro_atomic_load_acquire_64(&m_last_completed_eid)) >= target_eid)
			{
				// Abort, watermark reached.
				break;
			}

			if (thread_ctrl::state() == thread_state::aborting)
			{
				// Abort, emulation state changed.
				break;
			}

			// Wake the worker thread.
			retro_atomic_fetch_add_int(&m_wake_event, 1);
			retro_waitaddr_notify_one(&m_wake_event);

			// Wait for worker thread.
			thread_ctrl::wait_on_custom<1>([&](retro_waitaddr_t* list) { list[0] = {&m_completed_signal, static_cast<u32>(completion_token)}; });
		}
	}
}
