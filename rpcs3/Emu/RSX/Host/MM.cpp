#include "stdafx.h"
#include "MM.h"

#include <Emu/Memory/vm.h>

#include <Emu/RSX/Common/guest_page_locks.h>

namespace rsx
{
	// Before guest memory is locked (read-only or no-access): a guest thread
	// that has one of the pages in hand, making it writable after a write
	// fault, is let finish, so that the lock is applied after that and holds
	// (guest_page_locks.h)
	static void mm_settle_guest_pages(u64 start, u64 length, utils::protection prot)
	{
		if (prot == utils::protection::rw || prot == utils::protection::wx || !length)
		{
			return;
		}

		const u64 base = reinterpret_cast<u64>(vm::g_base_addr);

		if (start >= base && start - base + length <= 0x1'0000'0000ull)
		{
			rsx_page_locks_settle(static_cast<u32>(start - base), static_cast<u32>(start - base + length - 1));
		}
	}

	// Applied there and then, by the texture cache's thread: nothing is put
	// off to be applied later, so no queue is shared and nothing has to be
	// flushed before a submit, a report, a transfer, a fault or an unmap
	void mm_protect(void* ptr, u64 length, utils::protection prot)
	{
		mm_settle_guest_pages(reinterpret_cast<u64>(ptr), length, prot);
		utils::memory_protect(ptr, length, prot);
	}
}
