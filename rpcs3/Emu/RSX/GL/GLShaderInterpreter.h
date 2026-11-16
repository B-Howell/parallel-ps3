#pragma once
#include "glutils/program.h"
#include "../Program/ProgramStateCache.h"
#include "../Program/ShaderInterpreter.h"
#include "../Common/TextureUtils.h"

#include "Utilities/lf_u64_map.h"

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>

namespace rsx
{
	struct shader_loading_dialog;
}

namespace gl
{
	using namespace ::glsl;

	namespace interpreter
	{
		using program_metadata = program_hash_util::fragment_program_utils::fragment_program_metadata;

		enum class texture_pool_flags
		{
			dirty = 1
		};

		struct texture_pool
		{
			int pool_size = 0;
			int num_used = 0;
			u32 flags = 0;
			std::vector<int> allocated;

			bool allocate(int value)
			{
				if (num_used >= pool_size)
				{
					return false;
				}

				if (allocated.size() == unsigned(num_used))
				{
					allocated.push_back(value);
				}
				else
				{
					allocated[num_used] = value;
				}

				num_used++;
				flags |= static_cast<u32>(texture_pool_flags::dirty);
				return true;
			}
		};

		struct bindless_textures_t
		{
			std::array<handle64_t, 16> sampler1D;
			std::array<handle64_t, 16> sampler2D;
			std::array<handle64_t, 16> sampler3D;
			std::array<handle64_t, 16> samplerCUBE;

			u8 dirty = 0xff;

			std::span<handle64_t> get(rsx::texture_dimension_extended type)
			{
				using enum rsx::texture_dimension_extended;
				switch (type)
				{
				default:
				case texture_dimension_2d:
					return sampler2D;
				case texture_dimension_cubemap:
					return samplerCUBE;
				case texture_dimension_1d:
					return sampler1D;
				case texture_dimension_3d:
					return sampler3D;
				}
			}
		};

		// Made once and kept until the interpreter is destroyed: what is looked
		// up is never freed under the renderer. Published to the other threads
		// by a release (the cache's compare-and-swap, or upgraded)
		// A node of the list of every one made, for destroy()
		struct cached_program : mpsc_stack_node_t
		{
			// The renderer's after publication
			u32 flags = program_common::interpreter::CACHED_PIPE_UNINITIALIZED;

			// Compiler options mask - May not always match the storage compiler options in case of compatible pipelines
			// However the storage mask must be a subset of this options mask
			u64 build_compiler_options = 0;

			// The shaders are the caches'; the program is this one's when
			// owns_prog, else borrowed from the base variant's
			glsl::shader* vertex_shader = nullptr;
			glsl::shader* fragment_shader = nullptr;
			glsl::program* prog = nullptr;
			bool owns_prog = false;

			// The optimized build that replaced this one, set once
			retro_atomic_ptr_t upgraded{};
		};
	}

	class shader_interpreter
	{
		using async_build_callback_t = std::function<void(interpreter::cached_program*)>;

		// The three caches are added to from the renderer and the pipeline
		// compiler's threads and only ever gain entries, with no lock and no
		// thread waiting for another (lf_u64_map.h): compiler options (plus
		// one, as a key is never 0) to a glsl::shader* or a cached_program*.
		// A shader two threads made at once is kept once; the other's goes.
		// A program's optimized build hangs off the one it replaces
		lf_u64_map m_vs_cache{};
		lf_u64_map m_fs_cache{};
		lf_u64_map m_program_cache{};

		// Every cached_program made, freed at destroy()
		mpsc_stack_t m_programs{};

		// Texture binding information.
		interpreter::bindless_textures_t m_texture_bindings{};

		void build_vs(u64 compiler_options, interpreter::cached_program& prog_data);
		void build_fs(u64 compiler_options, interpreter::cached_program& prog_data);

		interpreter::cached_program* new_program();
		interpreter::cached_program* find_program(u64 compiler_options) const;
		interpreter::cached_program* build_program(u64 compiler_options);
		void build_program_async(u64 compiler_options, async_build_callback_t callback);
		void init_program(interpreter::cached_program* data, u64 compiler_options);
		void store_program(interpreter::cached_program* data, u64 compiler_options);

		interpreter::cached_program* m_current_interpreter = nullptr;

	public:
		shader_interpreter()
		{
			std::memset(&m_texture_bindings, 0, sizeof(m_texture_bindings));
			mpsc_stack_init(&m_programs);
		}

		void create(rsx::shader_loading_dialog* dlg);
		void destroy();

		// Update texture bindings based on the incoming descriptor structures
		void bind_fragment_texture(int i, handle64_t handle, const rsx::sampled_image_descriptor_base& descriptor);
		void flush_fragment_texture_bindings(glsl::program* program = nullptr);
		void flush_vertex_texture_bindings(glsl::program* program = nullptr);

		glsl::program* get(const interpreter::program_metadata& fp_metadata, u32 vp_ctrl, u32 fp_ctrl);
		bool is_interpreter(const glsl::program* program) const;
	};
}
