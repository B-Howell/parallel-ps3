#include "stdafx.h"

#include "Emu/Memory/vm.h"
#include "Common/BufferUtils.h"
#include "Core/RSXReservationLock.hpp"
#include "RSXOffload.h"
#include "RSXThread.h"

#include <retro_spsc.h>
#include "Common/rsx_wake.h"

#include <thread>
#include "util/asm.hpp"

namespace rsx
{
	// The offloader's jobs: a ring of plain packets made once (retro_spsc),
	// one producer - the renderer, which alone hands jobs over - and the
	// offloader taking them. The offloader waits on m_wake while the ring
	// is empty, woken by the next job: no allocation for a job, no spin
	struct dma_manager::offload_thread
	{
		static constexpr usz ring_packets = 4096;

		retro_spsc_t m_ring{};
		atomic_t<u64> m_enqueued_count = 0;
		atomic_t<u64> m_processed_count = 0;

		// Moved by every job handed over and by the end: what the
		// offloader waits on while the ring is empty
		rsx_wake_t m_wake{};

		// The job being done, for fault recovery (get_fault_range)
		transport_packet m_current_job{};

		thread_base* current_thread_ = nullptr;

		offload_thread()
		{
			rsx_wake_init(&m_wake);
			ensure(retro_spsc_init(&m_ring, ring_packets * sizeof(transport_packet)));
		}

		~offload_thread()
		{
			retro_spsc_free(&m_ring);
		}

		void wake() noexcept
		{
			// The job published first (the ring's head)
			rsx_wake_signal(&m_wake);
		}

		void operator ()()
		{
			if (!g_cfg.video.multithreaded_rsx)
			{
				// Abort if disabled
				return;
			}

			current_thread_ = thread_ctrl::get_current();
			ensure(current_thread_);

			if (g_cfg.core.thread_scheduler != thread_scheduler_mode::os)
			{
				thread_ctrl::set_thread_affinity_mask(thread_ctrl::get_affinity_mask(thread_class::rsx));
			}

			while (thread_ctrl::state() != thread_state::aborting)
			{
				const int seen = rsx_wake_seen(&m_wake);

				while (retro_spsc_read_avail(&m_ring) >= sizeof(transport_packet))
				{
					ensure(retro_spsc_read(&m_ring, &m_current_job, sizeof(transport_packet)) == sizeof(transport_packet));
					run(m_current_job);
					m_processed_count.release(m_processed_count + 1);
				}

				if (m_enqueued_count.load() == m_processed_count.load())
				{
					m_processed_count.notify_all();
					rsx::get_current_renderer()->attend();
				}

				// Nothing left: waits for the next job or the end
				if (retro_spsc_read_avail(&m_ring) < sizeof(transport_packet) && thread_ctrl::state() != thread_state::aborting)
				{
					rsx_wake_wait(&m_wake, seen);
				}
			}

			m_processed_count = -1;
			m_processed_count.notify_all();
			rsx::get_current_renderer()->attend();
		}

		offload_thread& operator=(thread_state state) noexcept
		{
			if (state == thread_state::aborting)
			{
				// Woken to see it end
				wake();
			}

			return *this;
		}

		static constexpr auto thread_name = "RSX Offloader"sv;
	};

	void dma_manager::run(const transport_packet& job)
	{
		switch (job.type)
		{
		case raw_copy:
		{
			const u32 vm_addr = vm::try_get_addr(job.src).first;
			rsx::reservation_lock<true, 1> rsx_lock(vm_addr, job.length, g_cfg.video.strict_rendering_mode && vm_addr);
			std::memcpy(job.dst, job.src, job.length);
			break;
		}
		case index_emulate:
		{
			write_index_array_for_non_indexed_non_native_primitive_to_buffer(static_cast<char*>(job.dst), static_cast<rsx::primitive_type>(job.aux_param0), job.length);
			break;
		}
		case callback:
		{
			rsx::get_current_renderer()->renderctl(job.aux_param0, job.src);
			break;
		}
		default: fmt::throw_exception("Unreachable");
		}
	}

	void dma_manager::enqueue(const transport_packet& job) const
	{
		auto& thr = *m_thread;

		if (retro_spsc_write_avail(&thr.m_ring) < sizeof(transport_packet)) [[unlikely]]
		{
			// The ring is full: the job is done here, after the ones before
			// it, as the offloader would have done them in order
			sync();
			run(job);
			return;
		}

		thr.m_enqueued_count++;
		ensure(retro_spsc_write(&thr.m_ring, &job, sizeof(transport_packet)) == sizeof(transport_packet));
		thr.wake();
	}

	// initialization
	void dma_manager::init()
	{
		m_thread = std::make_shared<named_thread<offload_thread>>();
	}

	// General transport
	void dma_manager::copy(void *dst, void *src, u32 length) const
	{
		if (length <= max_immediate_transfer_size || !g_cfg.video.multithreaded_rsx)
		{
			const u32 vm_addr = vm::try_get_addr(src).first;
			rsx::reservation_lock<true, 1> rsx_lock(vm_addr, length, g_cfg.video.strict_rendering_mode && vm_addr);
			std::memcpy(dst, src, length);
		}
		else
		{
			enqueue({raw_copy, length, src, dst, 0, 0});
		}
	}

	// Vertex utilities
	void dma_manager::emulate_as_indexed(void *dst, rsx::primitive_type primitive, u32 count)
	{
		if (!g_cfg.video.multithreaded_rsx)
		{
			write_index_array_for_non_indexed_non_native_primitive_to_buffer(
				static_cast<char*>(dst), primitive, count);
		}
		else
		{
			enqueue({index_emulate, count, nullptr, dst, static_cast<u8>(primitive), 0});
		}
	}

	// Backend callback
	void dma_manager::backend_ctrl(u32 request_code, void* args)
	{
		ensure(g_cfg.video.multithreaded_rsx);

		enqueue({callback, 0, args, nullptr, request_code, 0});
	}

	// Synchronization
	bool dma_manager::is_current_thread() const
	{
		if (auto cpu = thread_ctrl::get_current())
		{
			return m_thread->current_thread_ == cpu;
		}

		return false;
	}

	bool dma_manager::sync() const
	{
		auto& _thr = *m_thread;

		if (_thr.m_enqueued_count.load() <= _thr.m_processed_count.load()) [[likely]]
		{
			// Nothing to do
			return true;
		}

		if (auto rsxthr = get_current_renderer(); rsxthr->is_current_thread())
		{
			if (m_mem_fault_flag)
			{
				// Abort if offloader is in recovery mode
				return false;
			}

			// Woken by the offloader catching up or faulting, and by what
			// else the renderer has to answer meanwhile (attend), no spin
			rsxthr->wait_attending([&]()
			{
				return _thr.m_enqueued_count.load() <= _thr.m_processed_count.load();
			});
		}
		else
		{
			// Woken as the offloader catches up (it notifies once its queue
			// is empty, or as it ends), no spin
			for (u64 done = _thr.m_processed_count.load(); _thr.m_enqueued_count.load() > done && done != umax; done = _thr.m_processed_count.load())
			{
				_thr.m_processed_count.wait(done);
			}
		}

		return true;
	}

	void dma_manager::join()
	{
		sync();
		*m_thread = thread_state::aborting;
	}

	void dma_manager::set_mem_fault_flag()
	{
		ensure(is_current_thread()); // "Access denied"
		m_mem_fault_flag.release(true);
	}

	void dma_manager::clear_mem_fault_flag()
	{
		ensure(is_current_thread()); // "Access denied"
		m_mem_fault_flag.release(false);
	}

	// Fault recovery
	utils::address_range32 dma_manager::get_fault_range(bool writing) const
	{
		const auto m_current_job = &m_thread->m_current_job;

		void *address = nullptr;
		u32 range = m_current_job->length;

		switch (m_current_job->type)
		{
		case raw_copy:
			address = (writing) ? m_current_job->dst : m_current_job->src;
			break;
		case index_emulate:
			ensure(writing);
			address = m_current_job->dst;
			range = get_index_count(static_cast<rsx::primitive_type>(m_current_job->aux_param0), m_current_job->length) * sizeof(u32);
			break;
		default:
			fmt::throw_exception("Unreachable");
		}

		return utils::address_range32::start_length(vm::get_addr(address), range);
	}
}
