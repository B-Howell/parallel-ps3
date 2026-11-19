#include "stdafx.h"
#include "upnp_handler.h"
#include "util/logs.hpp"

LOG_CHANNEL(upnp_log, "UPNP");

// How long a request to the gateway may take
static constexpr int upnp_timeout_ms = 2000;

upnp_handler::~upnp_handler()
{
	// The gateway and the bindings are the HTTP thread's: all of this runs there
	m_http.run([this]()
	{
		for (const auto& [protocol, prot_bindings] : m_bindings)
		{
			for (const auto& [internal_port, external_port] : prot_bindings)
			{
				remove_port_redir_external(external_port, protocol);
			}
		}
	});

	retro_atomic_store_release_int(&m_active, 0);
}

void upnp_handler::upnp_enable()
{
	m_http.run([this]() { upnp_enable_owned(); });
}

void upnp_handler::upnp_enable_owned()
{
	m_cfg.load();

	const std::string dev_url = m_cfg.get_device_url();

	if (!dev_url.empty())
	{
		if (net_upnp_gateway_from_url(&m_gateway, dev_url.c_str(), upnp_timeout_ms))
		{
			upnp_log.notice("Saved UPNP(%s) enabled", dev_url);
			retro_atomic_store_release_int(&m_active, 1);
			return;
		}

		upnp_log.error("Saved UPNP(%s) isn't available anymore", dev_url);
	}

	upnp_log.notice("Starting UPNP search");

	if (!net_upnp_discover(&m_gateway, upnp_timeout_ms))
	{
		upnp_log.error("No UPNP IGD device was found");
		return;
	}

	upnp_log.notice("Found UPnP device type:%s at %s", m_gateway.service_type, m_gateway.desc_url);

	m_cfg.set_device_url(m_gateway.desc_url);
	m_cfg.save();

	retro_atomic_store_release_int(&m_active, 1);
}

void upnp_handler::add_port_redir(const std::string& addr, u16 internal_port, std::string_view protocol)
{
	if (!retro_atomic_load_acquire_int(&m_active))
		return;

	m_http.run([&]() { add_port_redir_owned(addr, internal_port, protocol); });
}

void upnp_handler::add_port_redir_owned(const std::string& addr, u16 internal_port, std::string_view protocol)
{
	const std::string protocol_str(protocol);
	const bool udp = protocol_str == "UDP";

	if (m_bindings[protocol_str].contains(internal_port))
		return;

	const u32 max_port = std::min(static_cast<u32>(internal_port) + 100, 0xFFFFu);

	// The mapping goes to this host's address on the route to the gateway,
	// which is the address the caller found for it
	for (u32 external_port = internal_port; external_port <= max_port; external_port++)
	{
		if (net_upnp_add_port_mapping_ext(&m_gateway, external_port, internal_port, udp, "RPCS3", upnp_timeout_ms))
		{
			m_bindings[protocol_str][static_cast<u16>(internal_port)] = static_cast<u16>(external_port);
			upnp_log.notice("Successfully bound %s:%d(%s) to IGD:%d", addr, internal_port, protocol, external_port);
			return;
		}

		// need more testing, may vary per router, etc, for now assume port conflict silently
	}

	upnp_log.error("Failed to bind %s:%d(%s) to IGD:(%d=>%d)", addr, internal_port, protocol, internal_port, max_port);
}

void upnp_handler::remove_port_redir(u16 internal_port, std::string_view protocol)
{
	if (!retro_atomic_load_acquire_int(&m_active))
		return;

	m_http.run([&]() { remove_port_redir_owned(internal_port, protocol); });
}

void upnp_handler::remove_port_redir_owned(u16 internal_port, std::string_view protocol)
{
	const std::string protocol_str(protocol);

	if (!m_bindings.contains(protocol_str) || !::at32(m_bindings, protocol_str).contains(internal_port))
	{
		upnp_log.error("tried to unbind port mapping %d to IGD(%s) but it isn't bound", internal_port, protocol);
		return;
	}

	const u16 external_port = ::at32(::at32(m_bindings, protocol_str), internal_port);

	remove_port_redir_external(external_port, protocol);

	ensure(::at32(m_bindings, protocol_str).erase(internal_port));
	upnp_log.notice("Successfully deleted port mapping %d to IGD:%d(%s)", internal_port, external_port, protocol);
}

void upnp_handler::remove_port_redir_external(u16 external_port, std::string_view protocol, bool verbose)
{
	// On the HTTP thread already
	const bool deleted = net_upnp_delete_port_mapping(&m_gateway, external_port, protocol == "UDP", upnp_timeout_ms);

	if (!deleted && verbose)
		upnp_log.error("Failed to delete port mapping IGD:%d(%s)", external_port, protocol);
}

bool upnp_handler::is_active() const
{
	return retro_atomic_load_acquire_int(&m_active) != 0;
}
