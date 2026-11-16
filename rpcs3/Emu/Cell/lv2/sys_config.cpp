#include "stdafx.h"
#include "Emu/IdManager.h"

#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Cell/ErrorCodes.h"

#include "sys_config.h"

LOG_CHANNEL(sys_config);


// Enums
template<>
void fmt_class_string<sys_config_service_id>::format(std::string& out, u64 id)
{
	const s64 s_id = static_cast<s64>(id);

	switch (s_id)
	{
	case SYS_CONFIG_SERVICE_PADMANAGER   : out += "SYS_CONFIG_SERVICE_PADMANAGER"; return;
	case SYS_CONFIG_SERVICE_PADMANAGER2  : out += "SYS_CONFIG_SERVICE_PADMANAGER2"; return;
	case SYS_CONFIG_SERVICE_USER_LIBPAD  : out += "SYS_CONFIG_SERVICE_USER_LIBPAD"; return;
	case SYS_CONFIG_SERVICE_USER_LIBKB   : out += "SYS_CONFIG_SERVICE_USER_LIBKB"; return;
	case SYS_CONFIG_SERVICE_USER_LIBMOUSE: out += "SYS_CONFIG_SERVICE_USER_LIBMOUSE"; return;
	}

	if (s_id < 0)
	{
		fmt::append(out, "SYS_CONFIG_SERVICE_USER_%llx", id & ~(1ull << 63));
	}
	else
	{
		fmt::append(out, "SYS_CONFIG_SERVICE_%llx", id);
	}
}

template<>
void fmt_class_string<sys_config_service_listener_type>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto value)
	{
		switch (value)
		{
			STR_CASE(SYS_CONFIG_SERVICE_LISTENER_ONCE);
			STR_CASE(SYS_CONFIG_SERVICE_LISTENER_REPEATING);
		}

		return unknown;
	});
}

// Utilities
void dump_buffer(std::string& out, const std::vector<u8>& buffer)
{
	if (!buffer.empty())
	{
		out.reserve(out.size() + buffer.size() * 2 + 1);
		fmt::append(out, "0x");

		for (u8 x : buffer)
		{
			fmt::append(out, "%02x", x);
		}
	}
	else
	{
		fmt::append(out, "EMPTY");
	}
}


// LV2 Config
void lv2_config::initialize()
{
	if (retro_atomic_load_acquire_int(&m_state) || !retro_atomic_cas_int(&m_state, 0, 1))
	{
		return;
	}

	// Register padmanager service, notifying vsh that a controller is connected
	static const u8 hid_info[0x1a] = {
		0x01, 0x01, //  2 unk
		0x02, 0x02, //  4
		0x00, 0x00, //  6
		0x00, 0x00, //  8
		0x00, 0x00, // 10
		0x05, 0x4c, // 12 vid
		0x02, 0x68, // 14 pid
		0x00, 0x10, // 16 unk2
		0x91, 0x88, // 18
		0x04, 0x00, // 20
		0x00, 0x07, // 22
		0x00, 0x00, // 24
		0x00, 0x00  // 26
	};

	// user_id for the padmanager seems to signify the controller port number, and the buffer contains some sort of HID descriptor
	lv2_config_service::create(SYS_CONFIG_SERVICE_PADMANAGER , 0, 1, 0, hid_info, 0x1a)->notify();
	lv2_config_service::create(SYS_CONFIG_SERVICE_PADMANAGER2, 0, 1, 0, hid_info, 0x1a)->notify();
}

// One change to the events: an event added, or the ID of one taken out
struct lv2_config::event_change : mpsc_stack_node_t
{
	shared_ptr<lv2_config_service_event> event;
	u32 id = 0;
};

// The events as find_event sees them: made by the applier, never changed
struct lv2_config::events_copy : mpsc_stack_node_t
{
	events_t events;
};

void lv2_config::free_copy(mpsc_stack_node_t* node)
{
	delete static_cast<events_copy*>(node);
}

void lv2_config::post(event_change* c)
{
	lf_post(&m_changes, c, apply, this);
}

void lv2_config::apply(void* ctx, mpsc_stack_node_t* oldest)
{
	auto& self = *static_cast<lv2_config*>(ctx);

	while (oldest)
	{
		mpsc_stack_node_t* next = oldest->next;
		auto* c = static_cast<event_change*>(oldest);

		if (c->event)
		{
			const u32 id = c->event->id;
			self.events.emplace(id, std::move(c->event));
		}
		else if (auto it = self.events.find(c->id); it != self.events.end())
		{
			// Let go of with the change, below: its destructor may post again,
			// which this thread applies before it lets go
			c->event = std::move(it->second);
			self.events.erase(it);
		}

		delete c;
		oldest = next;
	}

	auto* made = new events_copy();
	made->events = self.events;
	auto* old = static_cast<events_copy*>(retro_atomic_exchange_ptr(&self.m_published, made));
	retro_retire(&self.m_readers, old, free_copy);
}

void lv2_config::add_service_event(shared_ptr<lv2_config_service_event> event)
{
	auto* c = new event_change();
	c->event = std::move(event);
	post(c);
}

void lv2_config::remove_service_event(u32 id)
{
	auto* c = new event_change();
	c->id = id;
	post(c);
}

shared_ptr<lv2_config_service_event> lv2_config::find_event(u32 id)
{
	shared_ptr<lv2_config_service_event> found;

	retro_retire_enter(&m_readers);
	if (const auto* copy = static_cast<const events_copy*>(retro_atomic_load_acquire_ptr(&m_published)))
	{
		if (const auto it = copy->events.find(id); it != copy->events.cend())
		{
			found = it->second;
		}
	}
	retro_retire_leave(&m_readers, free_copy);

	return found ? found : null_ptr;
}

lv2_config_service_event& lv2_config_service_event::operator=(thread_state s) noexcept
{
	if (s == thread_state::destroying_context && !retro_atomic_exchange_int(&m_destroyed, 1))
	{
		if (auto global = g_fxo->try_get<lv2_config>())
		{
			global->remove_service_event(id);
		}
	}

	return *this;
}

lv2_config_service_event::~lv2_config_service_event() noexcept
{
	operator=(thread_state::destroying_context);
}

lv2_config::~lv2_config() noexcept
{
	for (auto& [key, event] : events)
	{
		if (event)
		{
			// Avoid collision with lv2_config_service_event destructor
			retro_atomic_store_release_int(&event->m_destroyed, 1);
		}
	}

	if (auto* copy = static_cast<events_copy*>(retro_atomic_exchange_ptr(&m_published, nullptr)))
	{
		for (auto& [key, event] : copy->events)
		{
			if (event)
			{
				retro_atomic_store_release_int(&event->m_destroyed, 1);
			}
		}
		delete copy;
	}
	retro_retire_drain(&m_readers, free_copy);

	for (mpsc_stack_node_t* n = mpsc_stack_drain(&m_changes.posted); n;)
	{
		mpsc_stack_node_t* next = n->next;
		auto* c = static_cast<event_change*>(n);
		if (c->event)
		{
			retro_atomic_store_release_int(&c->event->m_destroyed, 1);
		}
		delete c;
		n = next;
	}
}

// LV2 Config Service Listener
bool lv2_config_service_listener::check_service(const lv2_config_service& service) const
{
	// Filter by type
	if (type == SYS_CONFIG_SERVICE_LISTENER_ONCE && retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t*>(&m_has_events)))
	{
		return false;
	}

	// Filter by service ID or verbosity
	if (service_id != service.id || min_verbosity > service.verbosity)
	{
		return false;
	}

	// realhw only seems to send the pad connected events to the listeners that provided 0x01 as the first byte of their data buffer
	// TODO: Figure out how this filter works more properly
	if (service_id == SYS_CONFIG_SERVICE_PADMANAGER && (data.empty() || data[0] != 0x01))
	{
		return false;
	}

	// Event applies to this listener!
	return true;
}

// A service to tell the listener of
struct lv2_config_service_listener::notify_change : mpsc_stack_node_t
{
	shared_ptr<lv2_config_service> service;
};

// One thread at a time: the listener's events are its
void lv2_config_service_listener::apply_notifies(void* ctx, mpsc_stack_node_t* oldest)
{
	auto& self = *static_cast<lv2_config_service_listener*>(ctx);

	while (oldest)
	{
		mpsc_stack_node_t* next = oldest->next;
		auto* c = static_cast<notify_change*>(oldest);

		if (self.check_service(*c->service))
		{
			// Create service event and notify queue!
			const auto event = lv2_config_service_event::create(self.handle, c->service, self);
			self.service_events.emplace_back(event);
			retro_atomic_store_release_int(&self.m_has_events, 1);

			if (!event->notify())
			{
				// If we fail to deliver the event to the queue just clean the event up or it'll hold the listener alive forever
				g_fxo->get<lv2_config>().remove_service_event(event->id);
				self.service_events.pop_back();
				retro_atomic_store_release_int(&self.m_has_events, self.service_events.empty() ? 0 : 1);
			}
		}

		delete c;
		oldest = next;
	}
}

// The event is made and sent by the thread applying the listener's changes:
// this one, or another already at it, which does it before it lets go
bool lv2_config_service_listener::notify(const shared_ptr<lv2_config_service>& service)
{
	if (!check_service(*service))
		return false;

	auto* c = new notify_change();
	c->service = service;
	lf_post(&m_notifies, c, apply_notifies, this);
	return true;
}

lv2_config_service_listener::~lv2_config_service_listener() noexcept
{
	for (mpsc_stack_node_t* n = mpsc_stack_drain(&m_notifies.posted); n;)
	{
		mpsc_stack_node_t* next = n->next;
		delete static_cast<notify_change*>(n);
		n = next;
	}
}

void lv2_config_service_listener::notify_all()
{
	std::vector<shared_ptr<lv2_config_service>> services;

	// Grab all events
	idm::select<lv2_config_service>([&](u32 /*id*/, lv2_config_service& service)
	{
		if (check_service(service))
		{
			services.push_back(service.get_shared_ptr());
		}
	});

	// Sort services by timestamp
	sort(services.begin(), services.end(), [](const shared_ptr<lv2_config_service>& s1, const shared_ptr<lv2_config_service>& s2)
	{
		return s1->timestamp < s2->timestamp;
	});

	// Notify listener (now with services in sorted order)
	for (auto& service : services)
	{
		this->notify(service);
	}
}


// LV2 Config Service
void lv2_config_service::unregister()
{
	registered = false;

	// Notify listeners
	notify();

	// Allow this object to be destroyed by withdrawing it from the IDM
	// Note that it won't be destroyed while there are service events that hold a reference to it
	idm::remove<lv2_config_service>(idm_id);
}

void lv2_config_service::notify() const
{
	std::vector<shared_ptr<lv2_config_service_listener>> listeners;

	const shared_ptr<lv2_config_service> sptr = get_shared_ptr();

	idm::select<lv2_config_service_listener>([&](u32 /*id*/, lv2_config_service_listener& listener)
	{
		if (listener.check_service(*sptr))
			listeners.push_back(listener.get_shared_ptr());
	});

	for (auto& listener : listeners)
	{
		listener->notify(sptr);
	}
}

bool lv2_config_service_event::notify() const
{
	const auto _handle = handle;

	if (!_handle)
	{
		return false;
	}

	// Send event
	return _handle->notify(SYS_CONFIG_EVENT_SOURCE_SERVICE, (static_cast<u64>(service->is_registered()) << 32) | id, service->get_size());
}


// LV2 Config Service Event
void lv2_config_service_event::write(sys_config_service_event_t *dst) const
{
	const auto registered = service->is_registered();

	dst->service_listener_handle = listener_id;
	dst->registered = registered;
	dst->service_id = service->id;
	dst->user_id = service->user_id;

	if (registered)
	{
		dst->verbosity = service->verbosity;
		dst->padding = service->padding;

		const auto size = service->data.size();
		dst->data_size = static_cast<u32>(size);
		memcpy(dst->data, service->data.data(), size);
	}
}




/*
 * Syscalls
 */
error_code sys_config_open(u32 equeue_hdl, vm::ptr<u32> out_config_hdl)
{
	sys_config.trace("sys_config_open(equeue_hdl=0x%x, out_config_hdl=*0x%x)", equeue_hdl, out_config_hdl);

	// Find queue with the given ID
	const auto queue = idm::get_unlocked<lv2_obj, lv2_event_queue>(equeue_hdl);
	if (!queue)
	{
		return CELL_ESRCH;
	}

	// Initialize lv2_config global state
	auto& global = g_fxo->get<lv2_config>();
	if (true)
	{
		global.initialize();
	}

	// Create a lv2_config_handle object
	const auto config = lv2_config_handle::create(std::move(queue));
	if (config)
	{
		*out_config_hdl = idm::last_id<lv2_config_handle>();
		return CELL_OK;
	}

	// Failed to allocate sys_config object
	return CELL_EAGAIN;
}

error_code sys_config_close(u32 config_hdl)
{
	sys_config.trace("sys_config_close(config_hdl=0x%x)", config_hdl);

	if (!idm::remove<lv2_config_handle>(config_hdl))
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}



error_code sys_config_get_service_event(u32 config_hdl, u32 event_id, vm::ptr<sys_config_service_event_t> dst, u64 size)
{
	sys_config.trace("sys_config_get_service_event(config_hdl=0x%x, event_id=0x%llx, dst=*0x%llx, size=0x%llx)", config_hdl, event_id, dst, size);

	// Find sys_config handle object with the given ID
	const auto cfg = idm::get_unlocked<lv2_config_handle>(config_hdl);
	if (!cfg)
	{
		return CELL_ESRCH;
	}

	// Find service_event object
	const auto event = g_fxo->get<lv2_config>().find_event(event_id);
	if (!event || event->handle != cfg)
	{
		return CELL_ESRCH;
	}

	// Check buffer fits
	if (!event->check_buffer_size(size))
	{
		return CELL_EAGAIN;
	}

	// Write event to buffer
	event->write(dst.get_ptr());

	return CELL_OK;
}



error_code sys_config_add_service_listener(u32 config_hdl, sys_config_service_id service_id, u64 min_verbosity, vm::ptr<void> in, u64 size, sys_config_service_listener_type type, vm::ptr<u32> out_listener_hdl)
{
	sys_config.trace("sys_config_add_service_listener(config_hdl=0x%x, service_id=0x%llx, min_verbosity=0x%llx, in=*0x%x, size=%lld, type=0x%llx, out_listener_hdl=*0x%x)", config_hdl, service_id, min_verbosity, in, size, type, out_listener_hdl);

	// Find sys_config handle object with the given ID
	auto cfg = idm::get_unlocked<lv2_config_handle>(config_hdl);
	if (!cfg)
	{
		return CELL_ESRCH;
	}

	// Create service listener
	const auto listener = lv2_config_service_listener::create(cfg, service_id, min_verbosity, type, static_cast<u8*>(in.get_ptr()), size);
	if (!listener)
	{
		return CELL_EAGAIN;
	}

	if (size > 0)
	{
		std::string buf_str;
		dump_buffer(buf_str, listener->data);
		sys_config.todo("Registered service listener for service %llx with non-zero buffer: %s", service_id, buf_str.c_str());
	}

	// Notify listener with all past events
	listener->notify_all();

	// Done!
	*out_listener_hdl = listener->get_id();
	return CELL_OK;
}

error_code sys_config_remove_service_listener(u32 config_hdl, u32 listener_hdl)
{
	sys_config.trace("sys_config_remove_service_listener(config_hdl=0x%x, listener_hdl=0x%x)", config_hdl, listener_hdl);

	// Remove listener from IDM
	if (!idm::remove<lv2_config_service_listener>(listener_hdl))
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}



error_code sys_config_register_service(u32 config_hdl, sys_config_service_id service_id, u64 user_id, u64 verbosity, vm::ptr<u8> data_buf, u64 size, vm::ptr<u32> out_service_hdl)
{
	sys_config.trace("sys_config_register_service(config_hdl=0x%x, service_id=0x%llx, user_id=0x%llx, verbosity=0x%llx, data_but=*0x%llx, size=%lld, out_service_hdl=*0x%llx)", config_hdl, service_id, user_id, verbosity, data_buf, size, out_service_hdl);

	// Find sys_config handle object with the given ID
	const auto cfg = idm::get_unlocked<lv2_config_handle>(config_hdl);
	if (!cfg)
	{
		return CELL_ESRCH;
	}

	// Create service
	const auto service = lv2_config_service::create(service_id, user_id, verbosity, 0, data_buf.get_ptr(), size);
	if (!service)
	{
		return CELL_EAGAIN;
	}

	// Notify all listeners
	service->notify();

	// Done!
	*out_service_hdl = service->get_id();
	return CELL_OK;
}

error_code sys_config_unregister_service(u32 config_hdl, u32 service_hdl)
{
	sys_config.trace("sys_config_unregister_service(config_hdl=0x%x, service_hdl=0x%x)", config_hdl, service_hdl);

	// Remove listener from IDM
	auto service = idm::withdraw<lv2_config_service>(service_hdl);
	if (!service)
	{
		return CELL_ESRCH;
	}

	// Unregister service
	service->unregister();

	// Done!
	return CELL_OK;
}



/*
 * IO Events - TODO
 */
error_code sys_config_get_io_event(u32 config_hdl, u32 event_id /*?*/, vm::ptr<void> out_buf /*?*/, u64 size /*?*/)
{
	sys_config.todo("sys_config_get_io_event(config_hdl=0x%x, event_id=0x%x, out_buf=*0x%x, size=%lld)", config_hdl, event_id, out_buf, size);
	return CELL_OK;
}

error_code sys_config_register_io_error_listener(u32 config_hdl)
{
	sys_config.todo("sys_config_register_io_error_listener(config_hdl=0x%x)", config_hdl);
	return CELL_OK;
}

error_code sys_config_unregister_io_error_listener(u32 config_hdl)
{
	sys_config.todo("sys_config_unregister_io_error_listener(config_hdl=0x%x)", config_hdl);
	return CELL_OK;
}
