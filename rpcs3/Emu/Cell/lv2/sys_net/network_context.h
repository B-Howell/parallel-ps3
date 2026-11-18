#pragma once

#include <vector>
#include <map>
#include "Utilities/mutex.h"
#include "Emu/Cell/PPUThread.h"

#include "nt_p2p_port.h"
#include "net_wake.h"

struct base_network_thread
{
	// The thread waits in poll(), untimed, with this among its sockets: poked
	// as what it polls changes, and as it is to stop (net_wake.c)
	net_wake_t wake{};

	base_network_thread() { net_wake_init(&wake); }
	~base_network_thread() { net_wake_deinit(&wake); }
	base_network_thread(const base_network_thread&) = delete;
	base_network_thread& operator=(const base_network_thread&) = delete;

	void poke() { net_wake_poke(&wake); }

	void add_ppu_to_awake(ppu_thread* ppu);
	void del_ppu_to_awake(ppu_thread* ppu);

	shared_mutex mutex_ppu_to_awake;
	std::vector<ppu_thread*> ppu_to_awake;
	shared_mutex mutex_thread_loop;

	void wake_threads();
};

struct network_thread : base_network_thread
{
	atomic_t<u32> num_polls = 0;

	static constexpr auto thread_name = "Network Thread";

	void operator()();

	network_thread& operator=(thread_state)
	{
		poke();
		return *this;
	}
};

struct p2p_thread : base_network_thread
{
	shared_mutex list_p2p_ports_mutex;
	std::map<u16, nt_p2p_port> list_p2p_ports;
	atomic_t<u32> num_p2p_ports = 0;

	static constexpr auto thread_name = "Network P2P Thread";

	p2p_thread();

	void create_p2p_port(u16 p2p_port);

	void bind_sce_np_port();
	void operator()();

	p2p_thread& operator=(thread_state)
	{
		poke();
		return *this;
	}
};

using network_context = named_thread<network_thread>;
using p2p_context = named_thread<p2p_thread>;
