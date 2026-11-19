#pragma once

#include <unordered_map>

#include <net/net_upnp.h>
#include <retro_atomic.h>

#include "upnp_config.h"
#include "net_http_thread.h"

class upnp_handler
{
public:
	// Talks to the gateway on @http, the thread every HTTP transfer runs on
	explicit upnp_handler(np::net_http_thread& http) : m_http(http) {}
	~upnp_handler();

	void upnp_enable();
	void add_port_redir(const std::string& addr, u16 internal_port, std::string_view protocol);
	void remove_port_redir(u16 internal_port, std::string_view protocol);

	bool is_active() const;

private:
	// The HTTP thread's, as are the gateway, the config and the bindings:
	// the calls above hand these to it (net_http_thread::run), no lock
	void upnp_enable_owned();
	void add_port_redir_owned(const std::string& addr, u16 internal_port, std::string_view protocol);
	void remove_port_redir_owned(u16 internal_port, std::string_view protocol);
	void remove_port_redir_external(u16 external_port, std::string_view protocol, bool verbose = true);

private:
	np::net_http_thread& m_http;
	retro_atomic_int_t m_active{0}; // 0 or 1

	cfg_upnp m_cfg;
	net_upnp_gateway m_gateway{};
	std::unordered_map<std::string, std::unordered_map<u16, u16>> m_bindings;
};
