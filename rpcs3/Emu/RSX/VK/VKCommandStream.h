#pragma once

#include "VulkanAPI.h"
#include "Utilities/Thread.h"
#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>

namespace vk
{
	struct fence;

	enum rctrl_command : u32 // callback commands
	{
		rctrl_queue_submit = 0x80000000,
	};

	struct submit_packet
	{
		// Core components
		VkQueue      queue;
		fence*       pfence;
		VkSubmitInfo submit_info;

		// Pointer redirection storage
		VkSemaphore  wait_semaphore;
		VkSemaphore  signal_semaphore;
		VkFlags      wait_flags;

		submit_packet(VkQueue _q, fence* _f, const VkSubmitInfo* info) :
			queue(_q), pfence(_f), submit_info(*info),
			wait_semaphore(0), signal_semaphore(0), wait_flags(0)
		{
			if (info->waitSemaphoreCount)
			{
				wait_semaphore = *info->pWaitSemaphores;
				submit_info.pWaitSemaphores = &wait_semaphore;
			}

			if (info->signalSemaphoreCount)
			{
				signal_semaphore = *info->pSignalSemaphores;
				submit_info.pSignalSemaphores = &signal_semaphore;
			}

			if (info->pWaitDstStageMask)
			{
				wait_flags = *info->pWaitDstStageMask;
				submit_info.pWaitDstStageMask = &wait_flags;
			}
		}
	};

	struct driver_manager_t
	{
		static constexpr std::string_view thread_name = "Vulkan Driver Manager"sv;
		void operator()();

		void notify_completed(u64 eid);
		void drain();

	private:
		retro_atomic_64_t m_eid_ctr{0};
		retro_atomic_64_t m_last_completed_eid{0};

		retro_atomic_int_t m_wake_event{0};
		retro_atomic_int_t m_completed_signal{0};  //<- A 32-bit word to wait on, moved as m_last_completed_eid is
	};

	using driver_manager_thread = named_thread<driver_manager_t>;
}
