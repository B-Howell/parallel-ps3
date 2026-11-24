#pragma once
#include "GLHelpers.h"
#include "glutils/program.h"
#include "Emu/RSX/display.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Utilities/lockless.h"

namespace gl
{
	class pipe_compiler
	{
	public:
		enum op_flags
		{
			COMPILE_DEFAULT = 0,
			COMPILE_INLINE = 1,
			COMPILE_DEFERRED = 2
		};

		// Owned by a deferred job until a worker runs it, with what it captured: kept as std::function
		using storage_callback_t = std::function<void(std::unique_ptr<glsl::program>&)>;
		using build_callback_t = std::function<void(glsl::program*)>;
		// Capturing nothing: a plain function (it was a std::function)
		using link_callback_t = void (*)(glsl::program*);

		pipe_compiler();
		~pipe_compiler();

		// A context of its own from the frame, bound on the worker's first job; none without a frame
		void initialize(GSFrameBase* frame);

		std::unique_ptr<glsl::program> compile(
			op_flags flags,
			build_callback_t post_create_func = {},
			link_callback_t post_link_func = nullptr,
			storage_callback_t completion_callback = {});

		void operator()();

	private:

		struct pipe_compiler_job
		{
			build_callback_t post_create_func;
			link_callback_t post_link_func;
			storage_callback_t completion_callback;

			pipe_compiler_job(build_callback_t post_create, link_callback_t post_link, storage_callback_t completion)
				: post_create_func(std::move(post_create)), post_link_func(post_link), completion_callback(std::move(completion))
			{}
		};

		lf_queue<pipe_compiler_job> m_work_queue;

		draw_context_t m_context = 0;
		retro_atomic_int_t m_context_ready{0}; // 0 or 1

		// The frame the context is made, bound and let go of by: none (no context) where the shaders compile inline.
		// It was three std::functions around lambdas calling it
		GSFrameBase* m_frame = nullptr;

		std::unique_ptr<glsl::program> int_compile_graphics_pipe(
			const build_callback_t& post_create_func, link_callback_t post_link_func);
	};

	void initialize_pipe_compiler(GSFrameBase* frame, int num_worker_threads = -1);

	void destroy_pipe_compiler();
	pipe_compiler* get_pipe_compiler();
}
