#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <retro_atomic.h>

#include "util/types.hpp"
#include "util/atomic.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#else
#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#ifdef __clang__
#pragma GCC diagnostic pop
#endif
#endif

#ifdef _WIN32
using socket_type = uptr;
#else
using socket_type = int;
#endif

template <>
struct std::hash<std::array<u8, 16>>
{
	std::size_t operator()(const std::array<u8, 16>& array) const noexcept
	{
		std::size_t hash = 0;
		std::hash<std::uint8_t> hasher;

		for (auto byte : array)
		{
			hash ^= hasher(byte) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
		}

		return hash;
	}
};

namespace np
{
	class ip_address_translator
	{
	public:
		static bool is_ipv6(u32 addr_be);

		u32 register_ipv6(const std::array<u8, 16>& ipv6_addr);
		sockaddr_in6 get_ipv6_sockaddr(u32 addr_be, u16 port_be);

	private:
		static constexpr u32 translation_range_begin = 0x00'00'00'01;
		static constexpr u32 translation_range_end = 0x00'00'FF'FF;

		// One slot per address handed out, claimed by a fetch-and-add and
		// published by its flag once written: any thread registers or reads,
		// no lock. Made once, for every address there can be. An address two
		// threads register at once for the first time may get two; each maps
		// back to it, and the first one found is the one handed out after.
		struct slot
		{
			retro_atomic_int_t ready; // 1 once addr is written
			std::array<u8, 16> addr;
		};

		static constexpr u32 slot_count = translation_range_end - translation_range_begin;

		retro_atomic_int_t claimed{0};
		std::unique_ptr<slot[]> slots = std::make_unique<slot[]>(slot_count);
	};

	u32 register_ip(std::string_view ip_bytes);

	enum class IPV6_SUPPORT : u32 // kept in a retro_atomic_int_t and waited on
	{
		IPV6_UNKNOWN,
		IPV6_UNSUPPORTED,
		IPV6_SUPPORTED,
		IPV6_CHECKING, // one thread finding out; the others wait for its answer
	};

	bool is_ipv6_supported(std::optional<IPV6_SUPPORT> force_state = std::nullopt);
	s32 sendto_possibly_ipv6(socket_type native_socket, const char* data, u32 size, const sockaddr_in* addr, int native_flags);
	// As one datagram, @head then @body, gathered by the system: neither is
	// copied into a packet first
	s32 sendto_possibly_ipv6(socket_type native_socket, const char* head, u32 head_size, const char* body, u32 body_size, const sockaddr_in* addr, int native_flags);
	sockaddr_in6 sockaddr_to_sockaddr6(const sockaddr_in& addr);
	sockaddr_in sockaddr6_to_sockaddr(const sockaddr_in6& addr);

	void close_socket(socket_type socket);
	void set_socket_non_blocking(socket_type socket);
	void set_dgram_socket_disable_connreset(socket_type socket);
} // namespace np
