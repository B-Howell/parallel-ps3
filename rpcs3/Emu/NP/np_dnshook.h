#pragma once

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>
#include <optional>
#include <string>
#include <string>
#include <vector>

#include "util/types.hpp"

namespace np
{
	// A made-up DNS reply waiting to be read on its socket: one block, the
	// node and the bytes after it, freed by whoever takes it
	struct dns_reply : mpsc_stack_node_t
	{
		u32 size;
		u8* bytes() { return reinterpret_cast<u8*>(this + 1); }
	};

	// The sockets watched for DNS, with no lock and no thread waiting for
	// another. A fixed table: a slot is claimed for a socket with one
	// compare-and-swap. Replies are pushed by the thread that sends the query
	// and taken by the socket's receiver, oldest first: one receiver at a
	// time on a socket, as lv2 has it
	class dnshook
	{
	public:
		dnshook();
		~dnshook();
		dnshook(const dnshook&) = delete;
		dnshook& operator=(const dnshook&) = delete;

		void add_dns_spy(u32 sock);
		void remove_dns_spy(u32 sock);

		bool is_dns(u32 sock);
		bool is_dns_queue(u32 sock);

		// The oldest reply waiting on @sock (there is one: is_dns_queue), copied to @out; its size
		u32 take_dns_packet(u32 sock, u8* out, u32 capacity);
		s32 analyze_dns_packet(s32 s, const u8* buf, u32 len);

	private:
		std::optional<u32> get_redir(const std::string& hostname) const;

		static constexpr u32 slot_count = 128;

		struct spy_slot
		{
			// The socket plus one; 0 when the slot is free
			retro_atomic_int_t sock;
			// Pushed by the senders, newest first
			mpsc_stack_t incoming;
			// The receiver's: what it drained, oldest first
			dns_reply* pending;
		};

		spy_slot m_slots[slot_count]{};

		spy_slot* find_slot(s32 sock);
		spy_slot* claim_slot(s32 sock);
		static void drop_replies(spy_slot& slot);

		// Made once, at start: read by any thread
		std::vector<std::pair<std::string, u32>> m_redirs; // (lower-case wildcard pattern, IPv4 address)
	};
} // namespace np
