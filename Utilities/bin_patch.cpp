#include "bin_patch.h"
#include "ppu_patch.h"
#include "File.h"
#include "Emu/IdManager.h"
#include "Emu/Memory/vm.h"
#include "Emu/emu_callbacks.h"
#include "Emu/System.h"
#include "Emu/VFS.h"

#include "util/types.hpp"
#include "util/asm.hpp"
#include "util/logs.hpp"
#include "Utilities/StrFmt.h"

#include <cstdlib>

LOG_CHANNEL(patch_log, "PAT");

patch_engine::patch_engine()
{
	patch_db_init(&m_db, &patch_engine::log, nullptr);
}

patch_engine::~patch_engine()
{
	patch_db_free(&m_db);
}

std::string patch_engine::get_patch_config_path()
{
	const std::string config_dir = fs::get_config_dir(true);
	const std::string patch_path = config_dir + "patch_config.yml";
#ifdef _WIN32
	if (!fs::create_path(config_dir))
	{
		patch_log.error("Could not create path: %s (%s)", patch_path, fs::g_tls_error);
	}
#endif
	return patch_path;
}

std::string patch_engine::get_patches_path()
{
	return fs::get_config_dir() + "patches/";
}

std::string patch_engine::get_imported_patch_path()
{
	return get_patches_path() + "imported_patch.yml";
}

void patch_engine::log(void*, int level, const char* msg, usz len)
{
	const std::string_view text(msg, len);

	switch (level & PATCH_LOG_LEVEL_MASK)
	{
	case PATCH_LOG_FATAL: patch_log.fatal("%s", text); break;
	case PATCH_LOG_ERROR: patch_log.error("%s", text); break;
	case PATCH_LOG_WARNING: patch_log.warning("%s", text); break;
	case PATCH_LOG_NOTICE: patch_log.notice("%s", text); break;
	case PATCH_LOG_SUCCESS: patch_log.success("%s", text); break;
	default: break;
	}
}

// The whole file in a block of its own (malloc), or null when there is none
static char* read_whole(const std::string& path, usz& len)
{
	fs::file file{path};

	if (!file)
	{
		return nullptr;
	}

	const u64 size = file.size();
	char* const text = static_cast<char*>(std::malloc(size ? size : 1));

	if (text && file.read(text, size) != size)
	{
		std::free(text);
		return nullptr;
	}

	len = size;
	return text;
}

bool patch_engine::load(patch_db_t& db, const std::string& path)
{
	usz len = 0;
	char* const text = read_whole(path, len);

	if (!text)
	{
		// Do nothing
		return true;
	}

	// Which patches are on
	const std::string config_path = get_patch_config_path();
	patch_log.notice("Loading patch config file %s", config_path);

	usz config_len = 0;
	char* const config = read_whole(config_path, config_len);
	const bool ok = patch_db_load(&db, path.c_str(), text, len, config, config_len) != 0;
	std::free(config);
	return ok;
}

void patch_engine::save_config(const patch_db_t& db, const patch_config_choice_t* choices, usz count)
{
	const std::string path = get_patch_config_path();
	patch_log.notice("Saving patch config file %s", path);

	ryaml_writer_t* const w = ryaml_writer_new(nullptr, nullptr);
	usz len = 0;
	const char* const text = w && patch_db_write_config(&db, choices, count, w) ? ryaml_writer_memory(w, &len) : nullptr;

	fs::pending_file file(path);

	if (!text || !file.file || file.file.write(text, len) < len || !file.commit())
	{
		patch_log.error("Failed to create patch config file %s (error=%s)", path, fs::g_tls_error);
	}

	ryaml_writer_free(w);
}

void patch_engine::append_global_patches()
{
	// Regular patch.yml
	load(m_db, get_patches_path() + "patch.yml");

	// Imported patch.yml
	load(m_db, get_imported_patch_path());
}

void patch_engine::append_title_patches(std::string_view title_id)
{
	if (title_id.empty())
	{
		return;
	}

	// Regular patch.yml
	load(m_db, fmt::format("%s%s_patch.yml", get_patches_path(), title_id));
}

static void unmap_vm_area(std::shared_ptr<vm::block_t>& ptr)
{
	if (ptr && ptr->flags & (1ull << 62))
	{
		vm::unmap(0, true, &ptr);
	}
}

namespace
{
	struct apply_context
	{
		u8* (*translate)(void*, u32, u32);
		void* user;
	};

	u8* translate(void* ctx, u32 offset, u32 size)
	{
		const auto c = static_cast<apply_context*>(ctx);
		return c->translate(c->user, offset, size);
	}

	u8* guest(void*, u32 addr)
	{
		return vm::get_super_ptr<u8>(addr);
	}

	// Allocate memory at address (zeroized executable memory)
	u32 alloc(void*, const patch_data_t* p)
	{
		// Do not allow null address or if resultant ptr is not a VM ptr
		const u32 alloc_at = (p->offset & -4096);

		if (!(alloc_at >> 16))
		{
			return 0;
		}

		const u32 alloc_size = utils::align(static_cast<u32>(p->value.u) + alloc_at % 4096, 4096);

		// Allocate map if needed, if allocated flags will indicate that bit 62 is set (unique identifier)
		auto alloc_map = vm::reserve_map(vm::any, alloc_at & -0x10000, utils::align(alloc_size, 0x10000), vm::block_size_64k | (1ull << 62));

		u64 flags = vm::alloc_unwritable;

		switch (p->offset % PATCH_PROT_MASK)
		{
		case PATCH_PROT_WX: flags = vm::alloc_executable; break;
		case PATCH_PROT_RO: break;
		case PATCH_PROT_RX: flags |= vm::alloc_executable; break;
		case PATCH_PROT_RW: flags &= ~vm::alloc_unwritable; break;
		default: ensure(false);
		}

		if (!alloc_map)
		{
			return 0;
		}

		if (!alloc_map->falloc(alloc_at, alloc_size, nullptr, flags))
		{
			// Revert if allocated map before failure
			unmap_vm_area(alloc_map);
			return 0;
		}

		if (flags & vm::alloc_executable)
		{
			ppu_register_range(alloc_at, alloc_size);
		}

		if (vm::check_addr(alloc_at, vm::page_1m_size))
		{
			return alloc_at & -0x100000;
		}

		if (vm::check_addr(alloc_at, vm::page_64k_size))
		{
			return alloc_at & -0x10000;
		}

		return alloc_at & -0x1000;
	}

	void dealloc(void*, u32 addr)
	{
		vm::dealloc(addr);

		auto alloc_map = vm::get(vm::any, addr);
		unmap_vm_area(alloc_map);
	}

	// The PPU instruction an entry is at
	u32 branch_at(apply_context* c, u32 offset, int in_cave)
	{
		return vm::try_get_addr(in_cave ? vm::get_super_ptr<u8>(offset & -4) : translate(c, offset & -4, 4)).first;
	}

	int code_alloc(apply_context* c, patch_data_t* p, u64 value, u32 offset, int in_cave, u32* cave, u32* resval)
	{
		const u32 out_branch = branch_at(c, offset, in_cave);

		// Allow only if points to a PPU executable instruction
		if (out_branch < 0x10000 || out_branch >= 0x4000'0000 || !vm::check_addr<4>(out_branch, vm::page_executable))
		{
			return 0;
		}

		const u32 alloc_size = utils::align(static_cast<u32>(value + 1) * 4, 0x10000);

		// Check if should maybe reuse previous code cave allocation (0 size)
		if (alloc_size - 4 != 0)
		{
			// Nope
			*cave = 0;
		}

		// Always executable
		u64 flags = vm::alloc_executable | vm::alloc_unwritable;

		switch (p->offset & PATCH_PROT_MASK)
		{
		case PATCH_PROT_RW:
		case PATCH_PROT_WX:
		{
			flags &= ~vm::alloc_unwritable;
			break;
		}
		case PATCH_PROT_RO:
		case PATCH_PROT_RX:
		{
			break;
		}
		default: ensure(false);
		}

		const auto alloc_map = ensure(vm::get(vm::any, out_branch));

		// Range allowed for absolute branches to operate at
		// It takes into account that we need to put a branch for return at the end of memory space
		const u32 addr = p->alloc_addr = (*cave ? *cave : alloc_map->alloc(alloc_size, nullptr, 0x10000, flags));

		if (!addr)
		{
			patch_log.error("Failed to allocate 0x%x bytes for code (entry=0x%x)", alloc_size, out_branch);
			return 0;
		}

		patch_log.success("Allocated 0x%x for code at 0x%x (entry=0x%x)", alloc_size, addr, out_branch);

		// NOP filled
		// The word swapped once, not for each one
		const be_t<u32> nop_word = 0x60000000;

		for (be_t<u32>* nop = vm::get_super_ptr<u32>(addr), *end = nop + value; nop != end; nop++)
		{
			*nop = nop_word;
		}

		// Check if already registered by previous code allocation
		if (*cave != addr)
		{
			// Register code
			ppu_register_range(addr, alloc_size);
		}

		*resval = out_branch & -4;

		// Write branch to return to code
		if (!ppu_form_branch_to_code(addr + static_cast<u32>(value) * 4, *resval + 4))
		{
			patch_log.error("Failed to write return jump at 0x%x", addr + static_cast<u32>(value) * 4);
			ensure(alloc_map->dealloc(addr));
			return 0;
		}

		// Write branch to code
		if (!ppu_form_branch_to_code(out_branch, addr))
		{
			patch_log.error("Failed to jump to code cave at 0x%x", out_branch);
			ensure(alloc_map->dealloc(addr));
			return 0;
		}

		// Record the insertion point as a faux block.
		g_fxo->need<ppu_patch_block_registry_t>();
		g_fxo->get<ppu_patch_block_registry_t>().block_addresses.insert(*resval);

		*cave = addr;
		return 1;
	}

	int jump_func(apply_context* c, const patch_data_t* p, u32 offset, int in_cave, u32* resval)
	{
		const std::string_view str(p->original_value.s, p->original_value.len);

		const u32 out_branch = branch_at(c, offset, in_cave);
		const usz sep_pos = str.find_first_of(':');

		// Must contain only a single ':' or none
		// If ':' is found: Left string is the module name, right string is the function name
		// If ':' is not found: The entire string is a direct address of the function's descriptor in hexadecimal
		if (str.size() <= 2 || !sep_pos || sep_pos == str.size() - 1 || sep_pos != str.find_last_of(':'))
		{
			return 0;
		}

		const std::string_view func_name = str.substr(sep_pos + 1);
		u32 id = 0;

		if (func_name.starts_with("0x"))
		{
			// Raw hexadecimal-formatted FNID (real function name cannot contain a digit at the start, derived from C/CPP which were used in PS3 development):
			// all of it, eight digits at most
			if (func_name.size() <= 2 || func_name.size() > 10)
			{
				return 0;
			}

			for (const char ch : func_name.substr(2))
			{
				const int digit = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;

				if (digit < 0)
				{
					return 0;
				}

				id = id * 16 + digit;
			}
		}
		else
		{
			if (sep_pos == umax)
			{
				return 0;
			}

			// Generate FNID using function name
			id = ppu_generate_id(func_name);
		}

		// Allow only if points to a PPU executable instruction
		// FNID/OPD-address is placed at target
		if (!ppu_form_branch_to_code(out_branch, id, true, true, std::string{str.data(), sep_pos != umax ? sep_pos : 0}))
		{
			return 0;
		}

		*resval = out_branch & -4;
		return 1;
	}

	int patch_file(const patch_data_t* p)
	{
		const bool is_hide = p->type == PATCH_HIDE_FILE;
		std::string original_vfs_path(p->original_offset.s, p->original_offset.len);
		std::string dest_vfs_path(p->original_value.s, p->original_value.len);

		if (original_vfs_path.empty())
		{
			patch_log.error("Failed to patch file: original path is empty");
			return 0;
		}

		if (!is_hide && dest_vfs_path.empty())
		{
			patch_log.error("Failed to patch file: destination path is empty");
			return 0;
		}

		if (!original_vfs_path.starts_with("/dev_"))
		{
			original_vfs_path.insert(0, "/dev_");
		}

		if (!is_hide && !dest_vfs_path.starts_with("/dev_"))
		{
			dest_vfs_path.insert(0, "/dev_");
		}

		const std::string dest_path = is_hide ? fs::get_config_dir() + "delete_this_dir.../delete_this..." : vfs::get(dest_vfs_path);

		if (dest_path.empty())
		{
			patch_log.error("Failed to patch file path at '%s': destination is not mounted", original_vfs_path);
			return 0;
		}

		if (!vfs::mount(original_vfs_path, dest_path))
		{
			patch_log.error("Failed to patch file path at '%s': vfs::mount(dest='%s') failed", original_vfs_path, dest_vfs_path);
			return 0;
		}

		return 1;
	}

	int special(void* ctx, patch_data_t* p, u64 value, u32 offset, int in_cave, u32* cave, u32* resval)
	{
		const auto c = static_cast<apply_context*>(ctx);

		switch (p->type)
		{
		case PATCH_CODE_ALLOC:
		{
			return code_alloc(c, p, value, offset, in_cave, cave, resval);
		}
		case PATCH_JUMP:
		case PATCH_JUMP_LINK:
		{
			const u32 out_branch = branch_at(c, offset, in_cave);

			// Allow only if points to a PPU executable instruction
			if (!ppu_form_branch_to_code(out_branch, static_cast<u32>(value), p->type == PATCH_JUMP_LINK))
			{
				return 0;
			}

			*resval = out_branch & -4;
			return 1;
		}
		case PATCH_JUMP_FUNC:
		{
			return jump_func(c, p, offset, in_cave, resval);
		}
		case PATCH_BP_EXEC:
		{
			if (const u32 exec_addr = branch_at(c, offset, in_cave))
			{
				g_emu_callbacks.add_breakpoint(exec_addr);
			}

			return 1;
		}
		case PATCH_MOVE_FILE:
		case PATCH_HIDE_FILE:
		{
			return patch_file(p);
		}
		default: return 0;
		}
	}
}

void patch_engine::apply_with(std::vector<u32>& applied_total, const std::string& name, u8* (*translate_fn)(void*, u32, u32), void* translate_user, u32 filesz, u32 min_addr)
{
	// applied_total may be non-empty, do not clear it
	if (patch_db_find(&m_db, name.data(), name.size()) == PATCH_DB_NONE)
	{
		return;
	}

	apply_context ctx{translate_fn, translate_user};
	const patch_apply_ops_t ops{&ctx, translate, guest, alloc, dealloc, special};

	patch_u32s_t applied{};
	applied.cap = applied_total.size();
	applied.n = applied.cap;
	applied.v = applied.cap ? static_cast<u32*>(std::malloc(applied.cap * sizeof(u32))) : nullptr;

	if (applied.cap && !applied.v)
	{
		return;
	}

	if (applied.n)
	{
		std::memcpy(applied.v, applied_total.data(), applied.n * sizeof(u32));
	}

	patch_db_apply(&m_db, &applied, name.data(), name.size(), Emu.GetTitleID().c_str(), Emu.GetAppVersion().c_str(), &ops, filesz, min_addr);

	applied_total.assign(applied.v, applied.v + applied.n);
	std::free(applied.v);
}

void patch_engine::unload(const std::string& name)
{
	const patch_apply_ops_t ops{nullptr, nullptr, nullptr, nullptr, dealloc, nullptr};
	patch_db_unload(&m_db, name.data(), name.size(), &ops);
}
