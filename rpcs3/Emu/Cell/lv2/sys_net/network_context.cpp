#include "Emu/NP/ip_address.h"
#include "stdafx.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/Cell/Modules/sceNp.h" // for SCE_NP_PORT

#include "network_context.h"
#include "sys_net_helpers.h"

LOG_CHANNEL(sys_net);

// Used by RPCN to send signaling packets to RPCN server(for UDP hole punching)
bool send_packet_from_p2p_port_ipv4(const std::vector<u8>& data, const sockaddr_in& addr)
{
	// The ports are the network thread's
	return net_call([&]() -> bool
	{
		auto& nc = g_fxo->get<p2p_context>();

		if (nc.list_p2p_ports.contains(SCE_NP_PORT))
		{
			auto& def_port = ::at32(nc.list_p2p_ports, SCE_NP_PORT);

			if (np::is_ipv6_supported())
			{
				const auto addr6 = np::sockaddr_to_sockaddr6(addr);

				if (::sendto(def_port.p2p_socket, reinterpret_cast<const char*>(data.data()), ::size32(data), 0, reinterpret_cast<const sockaddr*>(&addr6), sizeof(sockaddr_in6)) == -1)
				{
					sys_net.error("Failed to send IPv4 signaling packet on IPv6 socket: %s", get_last_error(false, false));
					return false;
				}
			}
			else if (::sendto(def_port.p2p_socket, reinterpret_cast<const char*>(data.data()), ::size32(data), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(sockaddr_in)) == -1)
			{
				sys_net.error("Failed to send signaling packet on IPv4 socket: %s", get_last_error(false, false));
				return false;
			}
		}
		else
		{
			sys_net.error("send_packet_from_p2p_port_ipv4: port %d not present", +SCE_NP_PORT);
			return false;
		}

		return true;
	});
}

bool send_packet_from_p2p_port_ipv6(const std::vector<u8>& data, const sockaddr_in6& addr)
{
	return net_call([&]() -> bool
	{
		auto& nc = g_fxo->get<p2p_context>();

		if (nc.list_p2p_ports.contains(SCE_NP_PORT))
		{
			auto& def_port = ::at32(nc.list_p2p_ports, SCE_NP_PORT);
			ensure(np::is_ipv6_supported());

			if (::sendto(def_port.p2p_socket, reinterpret_cast<const char*>(data.data()), ::size32(data), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(sockaddr_in6)) == -1)
			{
				sys_net.error("Failed to send signaling packet on IPv6 socket: %s", get_last_error(false, false));
				return false;
			}
		}
		else
		{
			sys_net.error("send_packet_from_p2p_port_ipv6: port %d not present", +SCE_NP_PORT);
			return false;
		}

		return true;
	});
}

// The port is looked up on the network thread; what it took in is taken
// with no lock (nt_p2p_port's own stacks)
static nt_p2p_port* sce_np_port()
{
	return net_call([]() -> nt_p2p_port*
	{
		auto& nc = g_fxo->get<p2p_context>();
		const auto found = nc.list_p2p_ports.find(SCE_NP_PORT);
		return found == nc.list_p2p_ports.end() ? nullptr : &found->second;
	});
}

std::vector<std::vector<u8>> get_rpcn_msgs()
{
	// A port, once made, stays until the context goes
	if (nt_p2p_port* const port = sce_np_port())
	{
		return port->take_rpcn_msgs();
	}

	sys_net.error("get_rpcn_msgs: port %d not present", +SCE_NP_PORT);
	return {};
}

std::vector<signaling_message> get_sign_msgs()
{
	if (nt_p2p_port* const port = sce_np_port())
	{
		return port->take_sign_msgs();
	}

	sys_net.error("get_sign_msgs: port %d not present", +SCE_NP_PORT);
	return {};
}

namespace np
{
	void init_np_handler_dependencies();
}

static thread_local bool t_on_network_thread = false;

network_thread::network_thread()
{
	net_wake_init(&wake);
	owner_calls_init(&calls);

	// A call handed over wakes the thread out of its poll
	owner_calls_set_waker(&calls, [](void* w)
	{
		net_wake_poke(static_cast<net_wake_t*>(w));
	}, &wake);
}

network_thread::~network_thread()
{
	net_wake_deinit(&wake);
}

bool network_thread::on_owner()
{
	return t_on_network_thread;
}

void network_thread::add_ppu_to_awake(ppu_thread* ppu)
{
	ppu_to_awake.emplace_back(ppu);
}

void network_thread::del_ppu_to_awake(ppu_thread* ppu)
{
	for (auto it = ppu_to_awake.begin(); it != ppu_to_awake.end();)
	{
		if (*it == ppu)
		{
			it = ppu_to_awake.erase(it);
			continue;
		}

		it++;
	}
}

void network_thread::wake_threads()
{
	ppu_to_awake.erase(std::unique(ppu_to_awake.begin(), ppu_to_awake.end()), ppu_to_awake.end());
	for (ppu_thread* ppu : ppu_to_awake)
	{
		network_clear_queue(*ppu);
		lv2_obj::append(ppu);
	}

	if (!ppu_to_awake.empty())
	{
		ppu_to_awake.clear();
		lv2_obj::awake_all();
	}
}

p2p_thread::p2p_thread()
{
	np::init_np_handler_dependencies();
}

void p2p_thread::bind_sce_np_port()
{
	net_call([this]()
	{
		create_p2p_port(SCE_NP_PORT);
	});
}

void network_thread::operator()()
{
	t_on_network_thread = true;

	std::vector<shared_ptr<lv2_socket>> socklist;
	socklist.reserve(lv2_socket::id_count);

	// The ports polled, in the order of the map
	std::vector<nt_p2p_port*> portlist;

	ppu_to_awake.clear();

	// The sockets, the P2P ports, and the wake-up last: grown only as ports
	// are made
	std::vector<::pollfd> fds(lv2_socket::id_count + 1);
#ifdef _WIN32
	std::vector<bool> connecting(lv2_socket::id_count + 1);
	std::vector<bool> was_connecting(lv2_socket::id_count + 1);
#endif

	while (thread_ctrl::state() != thread_state::aborting)
	{
		// What was handed over, before the set is made: a call queueing a
		// socket is polled from this round
		owner_calls_run(&calls);

		socklist.clear();
		portlist.clear();

		// Obtain all native active sockets
		idm::select<lv2_socket>([&](u32 id, lv2_socket& s)
			{
				if (s.get_type() == SYS_NET_SOCK_DGRAM || s.get_type() == SYS_NET_SOCK_STREAM)
				{
					socklist.emplace_back(idm::get_unlocked<lv2_socket>(id));
				}
			});

		ensure(socklist.size() <= lv2_socket::id_count);

		for (auto& [_, port] : g_fxo->get<p2p_context>().list_p2p_ports)
		{
			portlist.push_back(&port);
		}

		const usz nfds = socklist.size() + portlist.size() + 1;

		if (fds.size() < nfds)
		{
			fds.resize(nfds);
#ifdef _WIN32
			connecting.resize(nfds);
			was_connecting.resize(nfds);
#endif
		}

		for (usz i = 0; i < socklist.size(); i++)
		{
			auto events = socklist[i]->get_events();

			fds[i].fd = events ? socklist[i]->get_socket() : -1;
			fds[i].events =
				(events & lv2_socket::poll_t::read ? POLLIN : 0) |
				(events & lv2_socket::poll_t::write ? POLLOUT : 0) |
				0;
			fds[i].revents = 0;
#ifdef _WIN32
			const auto cur_connecting = socklist[i]->is_connecting();
			was_connecting[i] = cur_connecting;
			connecting[i] = cur_connecting;
#endif
		}

		for (usz j = 0; j < portlist.size(); j++)
		{
			const usz i = socklist.size() + j;
			fds[i].fd = portlist[j]->p2p_socket;
			fds[i].events = POLLIN;
			fds[i].revents = 0;
#ifdef _WIN32
			was_connecting[i] = false;
			connecting[i] = false;
#endif
		}

		// Untimed: woken by the sockets, the ports, or a poke (a call handed
		// over, the thread to stop). Socket timeouts are the waiting PPU
		// threads' own (lv2_obj::wait_timeout)
		const usz nwake = nfds - 1;
		fds[nwake].fd = static_cast<socket_type>(wake.fd);
		fds[nwake].events = POLLIN;
		fds[nwake].revents = 0;
#ifdef _WIN32
		was_connecting[nwake] = false;
		connecting[nwake] = false;
		windows_poll(fds, static_cast<u32>(nfds), -1, connecting);
#else
		::poll(fds.data(), nfds, -1);
#endif

		// Then looked at again, whatever woke it
		net_wake_drain(&wake);

		for (usz i = 0; i < socklist.size(); i++)
		{
#ifdef _WIN32
			socklist[i]->handle_events(fds[i], was_connecting[i] && !connecting[i]);
#else
			socklist[i]->handle_events(fds[i]);
#endif
		}

		for (usz j = 0; j < portlist.size(); j++)
		{
			const auto revents = fds[socklist.size() + j].revents;

			if ((revents & POLLIN) == POLLIN || (revents & POLLRDNORM) == POLLRDNORM)
			{
				while (portlist[j]->recv_data())
					;
			}
		}

		wake_threads();
	}

	// No more calls: those made are run, and any made after runs on its
	// caller's thread
	owner_calls_close(&calls);
	t_on_network_thread = false;
}

// On the network thread
void p2p_thread::create_p2p_port(u16 p2p_port)
{
	if (!list_p2p_ports.contains(p2p_port))
	{
		list_p2p_ports.emplace(std::piecewise_construct, std::forward_as_tuple(p2p_port), std::forward_as_tuple(p2p_port));
	}
}
