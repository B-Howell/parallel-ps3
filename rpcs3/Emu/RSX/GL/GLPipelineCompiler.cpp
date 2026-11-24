#include "stdafx.h"
#include "GLPipelineCompiler.h"
#include "Utilities/Thread.h"
#include "util/sysinfo.hpp"

namespace gl
{
	// Global list of worker threads
	std::unique_ptr<named_thread_group<pipe_compiler>> g_pipe_compilers;
	int g_num_pipe_compilers = 0;
	retro_atomic_int_t g_compiler_index{0};

	pipe_compiler::pipe_compiler()
	{
	}

	pipe_compiler::~pipe_compiler()
	{
		if (m_frame)
		{
			m_frame->delete_context(m_context);
		}
	}

	void pipe_compiler::initialize(GSFrameBase* frame)
	{
		m_frame = frame;
		m_context = frame ? frame->make_context() : nullptr;
	}

	void pipe_compiler::operator()()
	{
		while (thread_ctrl::state() != thread_state::aborting)
		{
			for (auto&& job : m_work_queue.pop_all())
			{
				if (!retro_atomic_exchange_int(&m_context_ready, 1) && m_frame)
				{
					// Bind context on first use
					m_frame->set_current(m_context);
				}

				auto result = int_compile_graphics_pipe(
					job.post_create_func,
					job.post_link_func);

				job.completion_callback(result);
			}

			thread_ctrl::wait_on(m_work_queue);
		}
	}

	std::unique_ptr<glsl::program> pipe_compiler::compile(
		op_flags flags,
		build_callback_t post_create_func,
		link_callback_t post_link_func,
		storage_callback_t completion_callback_func)
	{
		if (flags == COMPILE_INLINE)
		{
			return int_compile_graphics_pipe(post_create_func, post_link_func);
		}

		m_work_queue.push(post_create_func, post_link_func, completion_callback_func);
		return {};
	}

	std::unique_ptr<glsl::program> pipe_compiler::int_compile_graphics_pipe(
		const build_callback_t& post_create_func,
		link_callback_t post_link_func)
	{
		auto result = std::make_unique<glsl::program>();
		result->create();

		if (post_create_func)
		{
			post_create_func(result.get());
		}

		result->link(post_link_func);
		return result;
	}

	void initialize_pipe_compiler(GSFrameBase* frame, int num_worker_threads)
	{
		if (num_worker_threads == 0)
		{
			// Select optimal number of compiler threads
			const auto hw_threads = utils::get_thread_count();
			if (hw_threads > 12)
			{
				num_worker_threads = 6;
			}
			else if (hw_threads > 8)
			{
				num_worker_threads = 4;
			}
			else if (hw_threads == 8)
			{
				num_worker_threads = 2;
			}
			else
			{
				num_worker_threads = 1;
			}
		}

		ensure(num_worker_threads >= 1);

		// Create the thread pool
		g_pipe_compilers = std::make_unique<named_thread_group<pipe_compiler>>("RSX.W", num_worker_threads);
		g_num_pipe_compilers = num_worker_threads;

		// Initialize the workers. At least one inline compiler shall exist (doesn't actually run)
		for (pipe_compiler& compiler : *g_pipe_compilers.get())
		{
			compiler.initialize(frame);
		}
	}

	void destroy_pipe_compiler()
	{
		g_pipe_compilers.reset();
	}

	pipe_compiler* get_pipe_compiler()
	{
		ensure(g_pipe_compilers);
		int thread_index = retro_atomic_fetch_add_int(&g_compiler_index, 1);

		return g_pipe_compilers.get()->begin() + (thread_index % g_num_pipe_compilers);
	}
}
