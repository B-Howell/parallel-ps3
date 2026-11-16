#include "stdafx.h"
#include "GLShaderInterpreter.h"
#include "GLTextureCache.h"
#include "GLVertexProgram.h"
#include "GLFragmentProgram.h"
#include "GLPipelineCompiler.h"

#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/Overlays/Shaders/shader_loading_dialog.h"
#include "Emu/RSX/Program/GLSLCommon.h"
#include "Emu/localized_string.h"

#include <rthreads/retro_waitaddr.h>

namespace gl
{
	using glsl::shader;
	using enum program_common::interpreter::compiler_option;
	using enum program_common::interpreter::cached_pipeline_flags;

	// Room for every variant the interpreter has, twice over
	static constexpr usz interpreter_cache_slots = 8192;

	// A key is never 0: compiler options with none set are
	static u64 cache_key(u64 compiler_options)
	{
		return compiler_options + 1;
	}

	interpreter::cached_program* shader_interpreter::new_program()
	{
		auto data = new interpreter::cached_program();
		mpsc_stack_push(&m_programs, data);
		return data;
	}

	// The newest build of the variant, or null
	interpreter::cached_program* shader_interpreter::find_program(u64 compiler_options) const
	{
		auto data = reinterpret_cast<interpreter::cached_program*>(lf_u64_map_get(&m_program_cache, cache_key(compiler_options)));

		while (data)
		{
			auto next = static_cast<interpreter::cached_program*>(retro_atomic_load_acquire_ptr(&data->upgraded));
			if (!next)
			{
				break;
			}
			data = next;
		}

		return data;
	}

	void shader_interpreter::create(rsx::shader_loading_dialog* dlg)
	{
		ensure(lf_u64_map_init(&m_vs_cache, interpreter_cache_slots) &&
			lf_u64_map_init(&m_fs_cache, interpreter_cache_slots) &&
			lf_u64_map_init(&m_program_cache, interpreter_cache_slots));

		dlg->create(get_localized_string(localized_string_id::RSX_OVERLAYS_COMPILING_SHADERS), get_localized_string(localized_string_id::RSX_OVERLAYS_COMPILING_SHADERS_TITLE));

		const auto variants = program_common::interpreter::get_interpreter_variants();
		const u32 limit1 = ::size32(variants.base_pipelines);
		const u32 limit2 = ::size32(variants.pipelines);
		dlg->set_limit(0, limit1);
		dlg->set_limit(1, limit2);

		// Counted by the compiler threads as each program is stored; the
		// dialog wakes on each count, not on a timer
		retro_atomic_int_t ctr;
		retro_atomic_int_init(&ctr, 0);
		auto progress_hook = [&](interpreter::cached_program*)
		{
			retro_atomic_inc_int(&ctr);
			retro_waitaddr_notify_all(&ctr);
		};

		auto update_progress = [&](u32 stage)
		{
			const auto completed = static_cast<u32>(retro_atomic_load_acquire_int(&ctr));
			const auto limit = stage ? limit2 : limit1;
			const auto message = get_localized_string(stage ? localized_string_id::RSX_OVERLAYS_COMPILING_SHADERS_OPENGL_LINK : localized_string_id::RSX_OVERLAYS_COMPILING_SHADERS_OPENGL_BUILD, "%u %s %u", completed, get_localized_string(localized_string_id::PROGRESS_DIALOG_OF), limit);
			dlg->update_msg(stage, message);
			dlg->set_value(stage, completed);
		};

		// We only need to build the base "compatible pipeline" pairs.
		for (const auto& variant : variants.base_pipelines)
		{
			build_program_async(variant.first | variant.second, progress_hook);
		}

		for (u32 seen = 0; seen < limit1;)
		{
			retro_waitaddr_t w;
			w.addr = &ctr;
			w.old = seen;
			retro_waitaddr_wait(&w, 1, -1);

			seen = static_cast<u32>(retro_atomic_load_acquire_int(&ctr));
			update_progress(0);
		}

		// Show final progress
		update_progress(0);

		// Second stage. Propagate base pipelines to all compatible. Every
		// base build is stored by now: the count above is taken after it
		for (const auto& variant : variants.pipelines)
		{
			const u64 compiler_options = variant.vs_opts.shader_opt | variant.fs_opts.shader_opt;
			if (find_program(compiler_options))
			{
				// Base variant
				continue;
			}

			const u64 compatible_options = variant.vs_opts.compatible_shader_opts | variant.fs_opts.compatible_shader_opts;
			auto base_pipeline = find_program(compatible_options);
			if (!base_pipeline)
			{
				fmt::throw_exception("Base variant was not found in the cache.");
			}

			// Borrows the base's shaders and program
			auto data = new_program();
			data->flags = base_pipeline->flags | CACHED_PIPE_UNOPTIMIZED;
			data->build_compiler_options = base_pipeline->build_compiler_options;
			data->vertex_shader = base_pipeline->vertex_shader;
			data->fragment_shader = base_pipeline->fragment_shader;
			data->prog = base_pipeline->prog;
			ensure(lf_u64_map_add(&m_program_cache, cache_key(compiler_options), reinterpret_cast<u64>(data)));
		}

		retro_atomic_store_release_int(&ctr, static_cast<int>(limit2));
		update_progress(1);
	}

	static void remove_shader(void*, uint64_t, uint64_t value)
	{
		auto shader = reinterpret_cast<glsl::shader*>(value);
		shader->remove();
		delete shader;
	}

	// The renderer is ending: nothing builds or looks up any more
	void shader_interpreter::destroy()
	{
		auto node = mpsc_stack_drain(&m_programs);
		while (node)
		{
			auto data = static_cast<interpreter::cached_program*>(node);
			node = node->next;

			if (data->owns_prog)
			{
				data->prog->remove();
				delete data->prog;
			}
			delete data;
		}

		lf_u64_map_each(&m_vs_cache, remove_shader, nullptr);
		lf_u64_map_each(&m_fs_cache, remove_shader, nullptr);
		lf_u64_map_free(&m_vs_cache);
		lf_u64_map_free(&m_fs_cache);
		lf_u64_map_free(&m_program_cache);
		m_current_interpreter = nullptr;
	}

	glsl::program* shader_interpreter::get(const interpreter::program_metadata& metadata, u32 vp_ctrl, u32 fp_ctrl)
	{
		// Build options
		u64 opt = 0;
		if (rsx::method_registers.alpha_test_enabled()) [[unlikely]]
		{
			switch (rsx::method_registers.alpha_func())
			{
			case rsx::comparison_function::always:
				break;
			case rsx::comparison_function::never:
				return nullptr;
			case rsx::comparison_function::greater_or_equal:
				opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_GE;
				break;
			case rsx::comparison_function::greater:
				opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_G;
				break;
			case rsx::comparison_function::less_or_equal:
				opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_LE;
				break;
			case rsx::comparison_function::less:
				opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_L;
				break;
			case rsx::comparison_function::equal:
				opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_EQ;
				break;
			case rsx::comparison_function::not_equal:
				opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_NE;
				break;
			}
		}

		if (fp_ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT) opt |= COMPILER_OPT_ENABLE_DEPTH_EXPORT;
		if (fp_ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS) opt |= COMPILER_OPT_ENABLE_F32_EXPORT;
		if (fp_ctrl & RSX_SHADER_CONTROL_USES_KIL) opt |= COMPILER_OPT_ENABLE_KIL;
		if (fp_ctrl & RSX_SHADER_CONTROL_ROP_OUTPUT_REMAP) opt |= COMPILER_OPT_ENABLE_ROP_REMAP;
		if (metadata.referenced_textures_mask) opt |= COMPILER_OPT_ENABLE_TEXTURES;
		if (metadata.has_branch_instructions) opt |= COMPILER_OPT_ENABLE_FLOW_CTRL;
		if (metadata.has_pack_instructions) opt |= COMPILER_OPT_ENABLE_PACKING;
		if (rsx::method_registers.polygon_stipple_enabled()) opt |= COMPILER_OPT_ENABLE_STIPPLING;
		if (vp_ctrl & RSX_SHADER_CONTROL_INSTANCED_CONSTANTS) opt |= COMPILER_OPT_ENABLE_INSTANCING;

		auto previous = m_current_interpreter;
		m_current_interpreter = find_program(opt);
		{
			if (!m_current_interpreter || m_current_interpreter != previous)
			{
				// Rebind all textures
				m_texture_bindings.dirty = 0xff;
			}

			if (m_current_interpreter)
			{
				constexpr u32 test_mask = (CACHED_PIPE_UNOPTIMIZED | CACHED_PIPE_RECOMPILING);
				constexpr u32 unoptimized_mask = CACHED_PIPE_UNOPTIMIZED;
				if ((m_current_interpreter->flags & test_mask) == unoptimized_mask)
				{
					// Interpreter is unoptimized and we haven't tried to recompile it
					// NOTE: This operation effectively orphans the current interpreter, but since unoptimized pipelines just have a non-owning reference then it's actually fine and we don't really leak anything.
					m_current_interpreter->flags |= CACHED_PIPE_RECOMPILING;
					build_program_async(opt, {});
				}

				if (m_current_interpreter->flags & CACHED_PIPE_UNINITIALIZED)
				{
					m_current_interpreter->prog->sync();
					init_program(m_current_interpreter, m_current_interpreter->build_compiler_options);
				}

				return m_current_interpreter->prog;
			}
		}

		m_current_interpreter = build_program(opt);
		return m_current_interpreter->prog;
	}

	void shader_interpreter::build_vs(u64 compiler_options, interpreter::cached_program& prog_data)
	{
		compiler_options &= COMPILER_OPT_ALL_VS_MASK;
		{
			if (auto found = lf_u64_map_get(&m_vs_cache, cache_key(compiler_options)))
			{
				prog_data.vertex_shader = reinterpret_cast<glsl::shader*>(found);
				return;
			}
		}

		::glsl::shader_properties properties{};
		properties.domain = ::glsl::program_domain::glsl_vertex_program;
		properties.require_lit_emulation = true;
		properties.require_clip_plane_functions = true;

		// TODO: Extend decompiler thread
		// TODO: Rename decompiler thread, it no longer spawns a thread
		RSXVertexProgram null_prog;
		std::string shader_str;
		ParamArray arr;

		null_prog.ctrl = (compiler_options & COMPILER_OPT_ENABLE_INSTANCING)
			? RSX_SHADER_CONTROL_INSTANCED_CONSTANTS
			: 0;
		GLVertexDecompilerThread comp(null_prog, shader_str, arr);

		// Initialize compiler properties
		comp.properties.has_indexed_constants = true;

		ParamType uniforms = { PF_PARAM_UNIFORM, "vec4" };
		uniforms.items.emplace_back("vc[468]", -1);

		std::stringstream builder;
		comp.insertHeader(builder);

		builder << "#define Z_NEGATIVE_ONE_TO_ONE\n\n";

		comp.insertConstants(builder, { uniforms });
		comp.insertInputs(builder, {});

		// Insert vp stream input
		builder << "\n"
			"layout(std140, binding = " << GL_INTERPRETER_VERTEX_BLOCK << ") readonly restrict buffer VertexInstructionBlock\n"
			"{\n"
			"	uint base_address;\n"
			"	uint entry;\n"
			"	uint output_mask;\n"
			"	uint control;\n"
			"	uvec4 vp_instructions[];\n"
			"};\n\n";

		if (compiler_options & COMPILER_OPT_ENABLE_INSTANCING)
		{
			builder << "#define _ENABLE_INSTANCED_CONSTANTS\n";
		}

		if (compiler_options)
		{
			builder << "\n";
		}

		::glsl::insert_glsl_legacy_function(builder, properties);
		::glsl::insert_vertex_input_fetch(builder, ::glsl::glsl_rules::glsl_rules_opengl4);

		builder << program_common::interpreter::get_vertex_interpreter();
		const std::string s = builder.str();

		auto made = new glsl::shader();
		made->create(::glsl::program_domain::glsl_vertex_program, s);
		made->compile();

		// Another thread's, made meanwhile, is kept: ours goes
		const u64 kept = lf_u64_map_add(&m_vs_cache, cache_key(compiler_options), reinterpret_cast<u64>(made));
		ensure(kept);
		if (kept != reinterpret_cast<u64>(made))
		{
			made->remove();
			delete made;
		}
		prog_data.vertex_shader = reinterpret_cast<glsl::shader*>(kept);
	}

	void shader_interpreter::build_fs(u64 compiler_options, interpreter::cached_program& prog_data)
	{
		// Cache lookup
		compiler_options &= COMPILER_OPT_ALL_FS_MASK;
		{
			if (auto found = lf_u64_map_get(&m_fs_cache, cache_key(compiler_options)))
			{
				prog_data.fragment_shader = reinterpret_cast<glsl::shader*>(found);
				return;
			}
		}

		u32 len {};
		ParamArray arr;
		std::string shader_str;
		RSXFragmentProgram frag;
		GLFragmentDecompilerThread comp(shader_str, arr, frag, len);

		std::stringstream builder;
		builder << "#version 450\n";

		if (compiler_options & COMPILER_OPT_ENABLE_TEXTURES)
		{
			builder << "#extension GL_ARB_bindless_texture : require\n";
		}

		builder << "\n";

		::glsl::insert_subheader_block(builder);
		comp.insertConstants(builder);

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_GE)
		{
			builder << "#define ALPHA_TEST_GEQUAL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_G)
		{
			builder << "#define ALPHA_TEST_GREATER\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_LE)
		{
			builder << "#define ALPHA_TEST_LEQUAL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_L)
		{
			builder << "#define ALPHA_TEST_LESS\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_EQ)
		{
			builder << "#define ALPHA_TEST_EQUAL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_NE)
		{
			builder << "#define ALPHA_TEST_NEQUAL\n";
		}

		if (!(compiler_options & COMPILER_OPT_ENABLE_F32_EXPORT))
		{
			builder << "#define WITH_HALF_OUTPUT_REGISTER\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_DEPTH_EXPORT)
		{
			builder << "#define WITH_DEPTH_EXPORT\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_FLOW_CTRL)
		{
			builder << "#define WITH_FLOW_CTRL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_PACKING)
		{
			builder << "#define WITH_PACKING\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_KIL)
		{
			builder << "#define WITH_KIL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_STIPPLING)
		{
			builder << "#define WITH_STIPPLING\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_TEXTURES)
		{
			builder << "#define WITH_TEXTURES\n\n";

			const char* type_names[] = { "sampler1D", "sampler2D", "samplerCube", "sampler3D" };
			for (int i = 0; i < 4; ++i)
			{
				builder << "layout(bindless_sampler) uniform " << type_names[i] << " " << type_names[i] << "_array[16];\n";
			}

			builder << "\n"
				"#undef  TEX_PARAM\n"
				"#define TEX_PARAM(index) texture_parameters[index + texture_base_index]\n"
				"#define IS_TEXTURE_RESIDENT(index) TEST_BIT(textures_resident, int(index))\n"
				"#define SAMPLER1D(index) sampler1D_array[index]\n"
				"#define SAMPLER2D(index) sampler2D_array[index]\n"
				"#define SAMPLER3D(index) sampler3D_array[index]\n"
				"#define SAMPLERCUBE(index) samplerCube_array[index]\n\n";
		}
		else if (compiler_options)
		{
			builder << "\n";
		}

		builder <<
			"layout(std430, binding =" << GL_INTERPRETER_FRAGMENT_BLOCK << ") readonly restrict buffer FragmentInstructionBlock\n"
			"{\n"
			"	uint shader_control;\n"
			"	uint texture_control;\n"
			"	uint textures_resident;\n"
			"	uint reserved2;\n"
			"	uvec4 fp_instructions[];\n"
			"};\n\n";

		const ::glsl::shader_properties properties
		{
			.domain = ::glsl::program_domain::glsl_fragment_program,
			.require_lit_emulation = true,
			.ROP_channel_remap = !!(compiler_options & COMPILER_OPT_ENABLE_ROP_REMAP),
		};

		::glsl::insert_glsl_legacy_function(builder, properties);
		builder << program_common::interpreter::get_fragment_interpreter();
		const std::string s = builder.str();

		auto made = new glsl::shader();
		made->create(::glsl::program_domain::glsl_fragment_program, s);
		made->compile();

		// Another thread's, made meanwhile, is kept: ours goes
		const u64 kept = lf_u64_map_add(&m_fs_cache, cache_key(compiler_options), reinterpret_cast<u64>(made));
		ensure(kept);
		if (kept != reinterpret_cast<u64>(made))
		{
			made->remove();
			delete made;
		}
		prog_data.fragment_shader = reinterpret_cast<glsl::shader*>(kept);
	}

	interpreter::cached_program* shader_interpreter::build_program(u64 compiler_options)
	{
		auto data = new_program();
		build_fs(compiler_options, *data);
		build_vs(compiler_options, *data);

		data->prog = new glsl::program();
		data->owns_prog = true;
		data->prog->create().
			attach(*data->vertex_shader).
			attach(*data->fragment_shader).
			link();

		init_program(data, compiler_options);
		store_program(data, compiler_options);
		return data;
	}

	void shader_interpreter::build_program_async(u64 compiler_options, async_build_callback_t callback)
	{
		auto data = new_program();

		auto post_create_hook = [=, this](glsl::program* prog)
		{
			build_fs(compiler_options, *data);
			build_vs(compiler_options, *data);

			prog->attach(*data->vertex_shader).
				attach(*data->fragment_shader);
		};

		auto storage_hook = [=, this](std::unique_ptr<glsl::program>& prog)
		{
			// NOTE: We need to do the program bindings in the consumer's context, so we skip the post-init hook and just set a flag
			// The consumer will handle initializing the bindings
			// The synchronization problem doesn't matter much on Windows driver, but Mesa drivers actually care about it.
			data->prog = prog.release();
			data->owns_prog = true;
			store_program(data, compiler_options);

			if (callback)
			{
				callback(data);
			}
		};

		auto compiler = gl::get_pipe_compiler();
		compiler->compile(
			gl::pipe_compiler::COMPILE_DEFERRED,
			post_create_hook,
			{},
			storage_hook
		);
	}

	void shader_interpreter::init_program(interpreter::cached_program* data, u64 compiler_options)
	{
		data->prog->uniforms[0] = GL_STREAM_BUFFER_START + 0;
		data->prog->uniforms[1] = GL_STREAM_BUFFER_START + 1;

		// Initialize texture bindings
		if (compiler_options & COMPILER_OPT_ENABLE_TEXTURES)
		{
			flush_fragment_texture_bindings(data->prog);
		}

		if (compiler_options & COMPILER_OPT_ENABLE_VTX_TEXTURES)
		{
			flush_vertex_texture_bindings(data->prog);
		}

		data->flags &= ~CACHED_PIPE_UNINITIALIZED;
	}

	// Published: the first build of a variant goes in the cache, a later one
	// (the optimized build of an unoptimized one) hangs off the newest there
	void shader_interpreter::store_program(interpreter::cached_program* data, u64 compiler_options)
	{
		data->build_compiler_options = compiler_options;

		const u64 kept = lf_u64_map_add(&m_program_cache, cache_key(compiler_options), reinterpret_cast<u64>(data));
		ensure(kept);
		if (kept == reinterpret_cast<u64>(data))
		{
			return;
		}

		auto last = reinterpret_cast<interpreter::cached_program*>(kept);
		while (!retro_atomic_cas_ptr(&last->upgraded, nullptr, data))
		{
			last = static_cast<interpreter::cached_program*>(retro_atomic_load_acquire_ptr(&last->upgraded));
		}
	}

	bool shader_interpreter::is_interpreter(const glsl::program* program) const
	{
		return (m_current_interpreter && program == m_current_interpreter->prog);
	}

	void shader_interpreter::bind_fragment_texture(int i, handle64_t handle, const rsx::sampled_image_descriptor_base& descriptor)
	{
		auto& bound_handle = m_texture_bindings.get(descriptor.image_type)[i];
		if (bound_handle != handle)
		{
			m_texture_bindings.dirty |= 1u << static_cast<u32>(descriptor.image_type);
			bound_handle = handle;
		}
	}

	void shader_interpreter::flush_fragment_texture_bindings(glsl::program* program)
	{
		using enum rsx::texture_dimension_extended;

		if (!program)
		{
			ensure(m_current_interpreter);
			program = m_current_interpreter->prog;
		}

		const bool is_bound_interpreter = m_current_interpreter && m_current_interpreter->prog == program;
		u32 dirty_mask = m_texture_bindings.dirty;

		if (is_bound_interpreter) [[ likely ]]
		{
			// Check if we even support textures
			dirty_mask = (m_current_interpreter->build_compiler_options & COMPILER_OPT_ENABLE_TEXTURES)
				? 0xff
				: 0x0;
		}

		if (!dirty_mask)
		{
			return;
		}

		const char* type_names[] = { "sampler1D_array", "sampler2D_array", "samplerCube_array", "sampler3D_array" };
		const rsx::texture_dimension_extended types[] = { texture_dimension_1d, texture_dimension_2d, texture_dimension_cubemap, texture_dimension_3d };

		for (int i = 0; i < 4; ++i)
		{
			const auto type_mask = (1u << static_cast<int>(types[i]));
			if ((dirty_mask & type_mask) == 0)
			{
				continue;
			}

			program->uniforms[type_names[i]] = m_texture_bindings.get(types[i]);
		}

		if (is_bound_interpreter)
		{
			m_texture_bindings.dirty = 0;
		}
	}

	void shader_interpreter::flush_vertex_texture_bindings(glsl::program* /*program*/)
	{
		// TODO
	}
}
