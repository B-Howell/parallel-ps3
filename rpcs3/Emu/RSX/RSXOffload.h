#pragma once

#include "util/types.hpp"
#include "Utilities/address_range.h"
#include "gcm_enums.h"


template <typename T>
class named_thread;

namespace rsx
{
	class dma_manager
	{
		enum op : u32
		{
			raw_copy = 0,
			index_emulate = 2,
			callback = 3
		};

		// A job, as it goes through the offloader's ring: plain data, copied
		// in and out of a ring made once - no allocation for a job
		struct transport_packet
		{
			op type{};
			u32 length{};
			void* src{};
			void* dst{};
			u32 aux_param0{};
			u32 aux_param1{};
		};

		// Done on the calling thread, as the offloader would have done it
		static void run(const transport_packet& job);

		// Into the ring for the offloader; done here when the ring is full
		void enqueue(const transport_packet& job) const;

		atomic_t<bool> m_mem_fault_flag = false;

		struct offload_thread;
		std::shared_ptr<named_thread<offload_thread>> m_thread;

		// TODO: Improved benchmarks here; value determined by profiling on a Ryzen CPU, rounded to the nearest 512 bytes
		const u32 max_immediate_transfer_size = 3584;

	public:
		dma_manager() = default;

		// initialization
		void init();

		// General tranport
		void copy(void *dst, void *src, u32 length) const;

		// Vertex utilities
		void emulate_as_indexed(void *dst, rsx::primitive_type primitive, u32 count);

		// Renderer callback
		void backend_ctrl(u32 request_code, void* args);

		// Synchronization
		bool is_current_thread() const;
		bool sync() const;
		void join();
		void set_mem_fault_flag();
		void clear_mem_fault_flag();

		// Fault recovery
		utils::address_range32 get_fault_range(bool writing) const;
	};
}
