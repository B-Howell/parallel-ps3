#pragma once

#include <vector>
#include <map>
#include "Emu/Cell/PPUThread.h"
#include "Emu/CPU/owner_calls.h"
#include "Emu/Cell/Modules/hle_service.h"

#include "nt_p2p_port.h"
#include "net_wake.h"

// All of sys_net is the network thread's: the sockets, their queues, the
// P2P ports and the threads to wake. It alone touches them, so none of it
// takes a lock; any other thread hands what it needs done over to it
// (net_call) and waits for it. The thread waits in poll() untimed, its
// sockets, the P2P ports and a wake-up socket (net_wake.c) polled together:
// a call handed over pokes the wake-up, as does the thread's stop.
struct network_thread
{
	net_wake_t wake{};
	owner_calls_t calls{};

	// The PPU threads to wake at the end of this round
	std::vector<ppu_thread*> ppu_to_awake;

	static constexpr auto thread_name = "Network Thread";

	network_thread();
	~network_thread();
	network_thread(const network_thread&) = delete;
	network_thread& operator=(const network_thread&) = delete;

	// Whether this thread is the network thread
	static bool on_owner();

	// @f run on the network thread, its result given back; at once there
	template <typename F>
	auto on_thread(F&& f) -> decltype(f())
	{
		const bool here = on_owner();
		return cpu_owner_call(here ? nullptr : &calls, here, std::forward<F>(f));
	}

	void poke() { net_wake_poke(&wake); }

	void add_ppu_to_awake(ppu_thread* ppu);
	void del_ppu_to_awake(ppu_thread* ppu);
	void wake_threads();

	void operator()();

	network_thread& operator=(thread_state)
	{
		poke();
		return *this;
	}
};

// The P2P ports: the network thread's, which polls them (p2p_thread's own
// thread has nothing to do)
struct p2p_thread
{
	std::map<u16, nt_p2p_port> list_p2p_ports;

	static constexpr auto thread_name = "Network P2P Thread";

	p2p_thread();

	// On the network thread
	void create_p2p_port(u16 p2p_port);

	// Any thread: handed to the network thread
	void bind_sce_np_port();

	void operator()() {}
};

using network_context = named_thread<network_thread>;
using p2p_context = named_thread<p2p_thread>;

// @f run on the network thread (network_thread::on_thread)
template <typename F>
auto net_call(F&& f) -> decltype(f())
{
	return g_fxo->get<network_context>().on_thread(std::forward<F>(f));
}
