#include "util/logs.hpp"
#include "Utilities/File.h"
#include "Utilities/Thread.h"
#include "Utilities/StrFmt.h"
#include <cstring>
#include <cstdarg>
#include <string>
#include <unordered_map>
#include <thread>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <regex>

#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>
#include <queues/mpsc_stack.h>

using namespace std::literals::chrono_literals;

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <sys/mman.h>
#endif

static std::string default_string()
{
	if (thread_ctrl::is_main())
	{
		return {};
	}

	return fmt::format("TID: %u", thread_ctrl::get_tid());
}

// Thread-specific log prefix provider
thread_local std::string(*g_tls_log_prefix)() = &default_string;

// Another thread-specific callback
thread_local void(*g_tls_log_control)(const char* fmt, u64 progress) = [](const char*, u64){};

template<>
void fmt_class_string<logs::level>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto lev)
	{
		switch (lev)
		{
		case logs::level::always: return "Nothing";
		case logs::level::fatal: return "Fatal";
		case logs::level::error: return "Error";
		case logs::level::todo: return "TODO";
		case logs::level::success: return "Success";
		case logs::level::warning: return "Warning";
		case logs::level::notice: return "Notice";
		case logs::level::trace: return "Trace";
		}

		return unknown;
	});
}

namespace logs
{
	static_assert(std::is_empty_v<message> && sizeof(message) == 1);
	static_assert(sizeof(channel) == alignof(channel));
	static_assert(uchar(level::always) == 0);
	static_assert(uchar(level::fatal) == 1);
	static_assert(uchar(level::trace) == 7);
	static_assert((offsetof(channel, fatal) & 7) == 1);
	static_assert((offsetof(channel, trace) & 7) == 7);

	// Memory-mapped buffer size
	constexpr u64 s_log_size = 32 * 1024 * 1024;
	static_assert(s_log_size * s_log_size > s_log_size && (s_log_size & (s_log_size - 1)) == 0); // Assert on an overflowing value

	class file_writer
	{
		std::thread m_writer{};
		fs::file m_fout{};
		u64 m_max_size{};

		std::unique_ptr<uchar[]> m_fptr{};

		atomic_t<u64, 128> m_buf{0}; // MSB (39 bits): push begin, LSB (25 bis): push size
		atomic_t<u64, 128> m_out{0}; // Amount of bytes written to file

		// No lock: only the writer thread writes to the file. A log line
		// bumps m_commits once its bytes are in (the writer waits on it);
		// the writer bumps m_flushes after each write out (sync waits on
		// it); m_close asks the writer to close the file (1), and is 2 once
		// it has. A line that finds the buffer full is dropped, counted in
		// m_dropped: the thread logging never waits for the disk.
		retro_atomic_int_t m_commits{0};
		retro_atomic_int_t m_flushes{0};
		retro_atomic_int_t m_close{0};
		retro_atomic_64_t m_dropped{0};

		// The writer thread's: write buffered logs out
		bool flush(u64 bufv);
		void wake_writer();

	public:
		file_writer(const std::string& name, u64 max_size);

		virtual ~file_writer();

		// Append raw data
		void log(const char* text, usz size);

		// Ensure written to disk
		void sync();

		// Close file handle after flushing to disk
		void close_prematurely();
	};

	struct file_listener final : file_writer, public listener
	{
		file_listener(const std::string& path, u64 max_size);

		~file_listener() override = default;

		void log(u64 stamp, const message& msg, std::string_view prefix, std::string_view text) override;

		void sync() override
		{
			file_writer::sync();
		}

		void close_prematurely() override
		{
			file_writer::close_prematurely();
		}
	};

	struct root_listener final : public listener
	{
		root_listener() = default;

		~root_listener() override = default;

		// Encode level, current thread name, channel name and write log message
		void log(u64, const message&, std::string_view, std::string_view) override
		{
			// Do nothing
		}

		// Channel registry
		std::unordered_multimap<std::string, channel*> channels{};

		// Messages for delayed listener initialization: pushed by any
		// thread, newest first, taken by set_init
		mpsc_stack_t messages{};
	};

	static root_listener* get_logger()
	{
		// Use magic static
		static root_listener logger{};
		return &logger;
	}

	static u64 get_stamp()
	{
		static struct time_initializer
		{
#ifdef _WIN32
			LARGE_INTEGER freq;
			LARGE_INTEGER start;

			time_initializer()
			{
				QueryPerformanceFrequency(&freq);
				QueryPerformanceCounter(&start);
			}
#else
			steady_clock::time_point start = steady_clock::now();
#endif

			u64 get() const
			{
#ifdef _WIN32
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				const LONGLONG diff = now.QuadPart - start.QuadPart;
				return diff / freq.QuadPart * 1'000'000 + diff % freq.QuadPart * 1'000'000 / freq.QuadPart;
#else
				return (steady_clock::now() - start).count() / 1000;
#endif
			}
		} timebase{};

		return timebase.get();
	}

	// No lock: the channel registry is made at static initialisation, before
	// any thread, and only read after; a channel's level is an atomic; the
	// listeners are added and removed while nothing is logging (the core
	// tearing its logging down, see listener::remove).

	// A message stored until set_init
	struct stored_node
	{
		mpsc_stack_node_t node;
		stored_message msg;
	};

	// Must be set to true in main()
	static atomic_t<bool> g_init{false};

	// Nothing above this level is let through, whatever a channel is set to
	// (set_ceiling)
	static retro_atomic_int_t g_ceiling{static_cast<int>(level::trace)};

	static level ceiling()
	{
		return static_cast<level>(retro_atomic_load_acquire_int(&g_ceiling));
	}

	void reset()
	{
		const level top = ceiling();

		for (auto&& pair : get_logger()->channels)
		{
			pair.second->enabled.release(std::min(level::_default, top));
		}
	}

	void set_ceiling(level value)
	{
		retro_atomic_store_release_int(&g_ceiling, static_cast<int>(value));

		for (auto&& pair : get_logger()->channels)
		{
			pair.second->enabled.release(std::min(level::_default, value));
		}
	}

	void silence()
	{
		for (auto&& pair : get_logger()->channels)
		{
			pair.second->enabled.release(level::always);
		}
	}

	void set_level(const std::string& ch_name, level value)
	{
		const level top = ceiling();

		if (ch_name.find_first_of(".+*?^$()[]{}|\\") != umax)
		{
			const std::regex ex(ch_name);

			// RegEx pattern
			for (auto& channel_pair : get_logger()->channels)
			{
				std::smatch sm;

				if (std::regex_match(channel_pair.first, sm, ex))
				{
					channel_pair.second->enabled.release(std::min(value, top));
				}
			}

			return;
		}

		auto found = get_logger()->channels.equal_range(ch_name);

		while (found.first != found.second)
		{
			found.first->second->enabled.release(std::min(value, top));
			found.first++;
		}
	}

	level get_level(const std::string& ch_name)
	{
		const auto found = get_logger()->channels.equal_range(ch_name);

		if (found.first != found.second)
		{
			return found.first->second->enabled.observe();
		}

		return level::always;
	}

	void set_channel_levels(const std::map<std::string, logs::level, std::less<>>& map)
	{
		for (auto&& pair : map)
		{
			logs::set_level(pair.first, pair.second);
		}
	}

	std::set<std::string> get_channels()
	{
		std::set<std::string> result;

		for (auto&& p : get_logger()->channels)
		{
			if (!p.first.empty())
			{
				result.insert(p.first);
			}
		}

		return result;
	}

	// Must be called in main() to stop accumulating messages in g_messages
	// The messages stored so far, oldest first, sent to the listeners and freed
	static void replay_stored()
	{
		mpsc_stack_node_t* node = mpsc_stack_reverse(mpsc_stack_drain(&get_logger()->messages));

		while (node)
		{
			stored_node* const stored = reinterpret_cast<stored_node*>(node);
			node = node->next;
			get_logger()->broadcast(stored->msg);
			delete stored;
		}
	}

	void set_init(std::initializer_list<stored_message> init_msg)
	{
		if (!g_init)
		{
			// Prepend main messages
			for (const auto& msg : init_msg)
			{
				get_logger()->broadcast(msg);
			}

			// Send initial messages; then those a thread stored while this
			// ran. (One stored after that was sent to the listeners there
			// were as it was logged; it is not sent again.)
			replay_stored();
			g_init = true;
			replay_stored();
		}
	}
}

logs::listener::~listener()
{
	// Shut up all channels on exit
	if (auto logger = get_logger())
	{
		if (logger == this)
		{
			return;
		}

		for (auto&& pair : logger->channels)
		{
			pair.second->enabled.release(level::always);
		}
	}
}

void logs::listener::add(logs::listener* _new)
{
	// Get first (main) listener
	listener* lis = get_logger();

	// Install new listener at the end of linked list
	listener* null = nullptr;

	while (lis->m_next || !lis->m_next.compare_exchange(null, _new))
	{
		lis = lis->m_next;
		null = nullptr;
	}
}

void logs::listener::remove(logs::listener* old)
{
	listener* lis = get_logger();

	// Unlink it from the list add() built
	while (listener* next = lis->m_next)
	{
		if (next == old)
		{
			lis->m_next.release(next->m_next.load());
			next->m_next.release(nullptr);
			return;
		}

		lis = next;
	}
}

void logs::listener::broadcast(const logs::stored_message& msg) const
{
	for (auto lis = m_next.load(); lis; lis = lis->m_next)
	{
		lis->log(msg.stamp, msg.m, msg.prefix, msg.text);
	}
}

void logs::listener::sync()
{
}

void logs::listener::close_prematurely()
{
}

void logs::listener::sync_all()
{
	for (listener* lis = get_logger(); lis; lis = lis->m_next)
	{
		lis->sync();
	}
}

void logs::listener::shutdown_all()
{
	for (listener* lis = get_logger()->m_next.exchange(nullptr); lis;)
	{
		lis = lis->m_next.exchange(nullptr);
	}
}

void logs::listener::close_all_prematurely()
{
	for (listener* lis = get_logger(); lis; lis = lis->m_next)
	{
		lis->close_prematurely();
	}
}

logs::registerer::registerer(channel& _ch)
{
	// At static initialisation, before any thread
	get_logger()->channels.emplace(_ch.name, &_ch);
}

void logs::message::broadcast(const char* fmt, const fmt_type_info* sup, ...) const
{
	// Get timestamp
	const u64 stamp = get_stamp();

	// Notify start operation
	g_tls_log_control(fmt, 0);

	// Get text, extract va_args. Plain locals, not thread_local: a
	// thread_local with a destructor ties the library to the thread until
	// it ends, and the libretro frontend's main thread logs here too and
	// never ends, so the core was never unloaded and the next content ran
	// on its old state
	std::string text;
	std::vector<u64> args;

	static constexpr fmt_type_info empty_sup{};

	usz args_count = 0;
	for (auto v = sup; v && v->fmt_string; v++)
		args_count++;

	text.clear();
	args.resize(args_count);

	va_list c_args;
	va_start(c_args, sup);
	for (u64& arg : args)
		arg = va_arg(c_args, u64);
	va_end(c_args);
	fmt::raw_append(text, fmt, sup ? sup : &empty_sup, args.data());
	std::string prefix = g_tls_log_prefix();

	// Get first (main) listener
	listener* lis = get_logger();

	if (!g_init)
	{
		while (lis)
		{
			lis->log(stamp, *this, prefix, text);
			lis = lis->m_next;
		}

		// Store message additionally, for set_init
		stored_node* const stored = new stored_node{{}, stored_message{*this, stamp, std::move(prefix), text}};
		mpsc_stack_push(&get_logger()->messages, &stored->node);
	}

	// Send message to all listeners
	while (lis)
	{
		lis->log(stamp, *this, prefix, text);
		lis = lis->m_next;
	}

	// Notify end operation
	g_tls_log_control(fmt, -1);
}

logs::file_writer::file_writer(const std::string& name, u64 max_size)
	: m_max_size(max_size)
{
	if (name.empty() || !max_size)
	{
		return;
	}

	// Initialize ringbuffer
	m_fptr = std::make_unique<uchar[]>(s_log_size);

	// Actual log file (allowed to fail)
	if (!m_fout.open(name, fs::rewrite))
	{
		fprintf(stderr, "Log file open failed: %s (error %d)\n", name.c_str(), errno);
	}

	m_writer = std::thread([this]()
	{
		thread_base::set_name("Log Writer");

		thread_ctrl::scoped_priority low_prio(-1);

		while (true)
		{
			// Read before looking: a line committed after wakes the wait
			const int commits = retro_atomic_load_acquire_int(&m_commits);
			const u64 bufv = m_buf;

			if (retro_atomic_load_acquire_int(&m_close) == 1)
			{
				m_fout.close();
				retro_atomic_store_release_int(&m_close, 2);
				retro_atomic_inc_int(&m_flushes);
				retro_waitaddr_notify_all(&m_flushes);
			}

			if (bufv % s_log_size)
			{
				// A thread is putting its line in: until it has
				const retro_waitaddr_t list[1] = {{&m_commits, static_cast<u32>(commits)}};
				retro_waitaddr_wait(list, 1, -1);
				continue;
			}

			if (flush(bufv))
			{
				retro_atomic_inc_int(&m_flushes);
				retro_waitaddr_notify_all(&m_flushes);
				continue;
			}

			if (m_out == umax)
			{
				break;
			}

			// Nothing to write: until a line is
			const retro_waitaddr_t list[1] = {{&m_commits, static_cast<u32>(commits)}};
			retro_waitaddr_wait(list, 1, -1);
		}
	});
}

logs::file_writer::~file_writer()
{
	if (!m_fptr)
	{
		return;
	}

	// Stop writer thread
	file_writer::sync();

	m_out = -1;
	wake_writer();
	m_writer.join();
}

void logs::file_writer::wake_writer()
{
	retro_atomic_inc_int(&m_commits);
	retro_waitaddr_notify_all(&m_commits);
}

bool logs::file_writer::flush(u64 bufv)
{
	const u64 read_pos = m_out;
	const u64 out_index = read_pos % s_log_size;
	const u64 pushed = (bufv / s_log_size) % s_log_size;
	const u64 end = std::min<u64>(out_index <= pushed ? read_pos - out_index + pushed : ((read_pos + s_log_size) & ~(s_log_size - 1)), m_max_size);

	if (end > read_pos)
	{
		// Everything that is waiting, in one write
		const u64 size = end - read_pos;

		if (m_fout && m_fout.write(m_fptr.get() + out_index, size) != size)
		{
			m_fout.close();
		}

		m_out += size;
		return true;
	}

	return false;
}

void logs::file_writer::log(const char* text, usz size)
{
	if (!m_fptr)
	{
		return;
	}

	// TODO: write bigger fragment directly in blocking manner
	while (size && size < s_log_size)
	{
		const auto [bufv, pos] = m_buf.fetch_op([&](u64& v) -> uchar*
		{
			const u64 out = m_out % s_log_size;
			const u64 v1 = (v / s_log_size) % s_log_size;
			const u64 v2 = v % s_log_size;

			if (v1 + v2 + size >= (out <= v1 ? out + s_log_size : out)) [[unlikely]]
			{
				return nullptr;
			}

			v += size;
			return m_fptr.get() + (v1 + v2) % s_log_size;
		});

		if (!pos) [[unlikely]]
		{
			if (m_out >= m_max_size || !m_fout)
			{
				// Logging is inactive
				return;
			}

			// The buffer is full: the line is dropped, and the writer woken.
			// A thread logging never waits for the disk
			(void)bufv;
			retro_atomic_fetch_add_64(&m_dropped, 1);
			wake_writer();
			return;
		}

		if (pos - m_fptr.get() + size > s_log_size)
		{
			const auto frag = s_log_size - (pos - m_fptr.get());
			std::memcpy(pos, text, frag);
			std::memcpy(m_fptr.get(), text + frag, size - frag);
		}
		else
		{
			std::memcpy(pos, text, size);
		}

		m_buf += (size * s_log_size) - size;

		// The writer, if it waits for this line
		retro_atomic_inc_int(&m_commits);
		retro_waitaddr_notify_one(&m_commits);
		break;
	}
}

void logs::file_writer::sync()
{
	if (!m_fptr)
	{
		return;
	}

	// Wait for the writer thread to have written out what is in
	for (;;)
	{
		const int flushes = retro_atomic_load_acquire_int(&m_flushes);

		if ((m_out % s_log_size) * s_log_size == m_buf % (s_log_size * s_log_size) || m_out >= m_max_size)
		{
			break;
		}

		wake_writer();
		const retro_waitaddr_t list[1] = {{&m_flushes, static_cast<u32>(flushes)}};
		retro_waitaddr_wait(list, 1, -1);
	}

	if (thread_ctrl::get_current())
	{
		return;
	}

	// Ensure written to disk
	if (m_fout)
	{
		m_fout.sync();
	}
}

void logs::file_writer::close_prematurely()
{
	if (!m_fptr)
	{
		return;
	}

	// Ensure written to disk
	sync();

	// The writer thread closes it, between its writes
	retro_atomic_store_release_int(&m_close, 1);
	for (;;)
	{
		const int flushes = retro_atomic_load_acquire_int(&m_flushes);
		if (retro_atomic_load_acquire_int(&m_close) == 2)
			break;
		wake_writer();
		const retro_waitaddr_t list[1] = {{&m_flushes, static_cast<u32>(flushes)}};
		retro_waitaddr_wait(list, 1, -1);
	}
}

logs::file_listener::file_listener(const std::string& path, u64 max_size)
	: file_writer(path, max_size)
	, listener()
{
	// Write UTF-8 BOM
	file_writer::log("\xEF\xBB\xBF", 3);
}

void logs::file_listener::log(u64 stamp, const logs::message& msg, std::string_view prefix, std::string_view _text)
{
	/*constinit thread_local*/ std::string text;
	text.reserve(50000);

	// Used character: U+00B7 (Middle Dot)
	switch (msg)
	{
	case level::always:  text = reinterpret_cast<const char*>(u8"·A "); break;
	case level::fatal:   text = reinterpret_cast<const char*>(u8"·F "); break;
	case level::error:   text = reinterpret_cast<const char*>(u8"·E "); break;
	case level::todo:    text = reinterpret_cast<const char*>(u8"·U "); break;
	case level::success: text = reinterpret_cast<const char*>(u8"·S "); break;
	case level::warning: text = reinterpret_cast<const char*>(u8"·W "); break;
	case level::notice:  text = reinterpret_cast<const char*>(u8"·! "); break;
	case level::trace:   text = reinterpret_cast<const char*>(u8"·T "); break;
	}

	// Print microsecond timestamp
	const u64 hours = stamp / 3600'000'000;
	const u64 mins = (stamp % 3600'000'000) / 60'000'000;
	const u64 secs = (stamp % 60'000'000) / 1'000'000;
	const u64 frac = (stamp % 1'000'000);
	fmt::append(text, "%u:%02u:%02u.%06u ", hours, mins, secs, frac);

	if (stamp == 0)
	{
		// Workaround for first special messages to keep backward compatibility
		text.clear();
	}

	if (!prefix.empty())
	{
		text += "{";
		text += prefix;
		text += "} ";
	}

	if (stamp && msg->name && '\0' != *msg->name)
	{
		text += msg->name;
		text += msg == level::todo ? " TODO: " : ": ";
	}
	else if (msg == level::todo)
	{
		text += "TODO: ";
	}

	text += _text;
	text += '\n';

	file_writer::log(text.data(), text.size());
}

std::unique_ptr<logs::listener> logs::make_file_listener(const std::string& path, u64 max_size)
{
	std::unique_ptr<logs::listener> result = std::make_unique<logs::file_listener>(path, max_size);

	// Register file listener
	result->add(result.get());
	return result;
}
