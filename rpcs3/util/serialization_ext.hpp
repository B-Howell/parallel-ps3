#pragma once

#include "util/serialization.hpp"
#include "util/shared_ptr.hpp"

#include "Utilities/Thread.h"
#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>

#include <deque>

namespace fs
{
	class file;
}

// Uncompressed file serialization handler
struct uncompressed_serialization_file_handler : utils::serialization_file_handler
{
	const std::unique_ptr<fs::file> m_file_storage;
	const std::add_pointer_t<const fs::file> m_file;

	explicit uncompressed_serialization_file_handler(fs::file&& file) noexcept
		: utils::serialization_file_handler()
		, m_file_storage(std::make_unique<fs::file>(std::move(file)))
		, m_file(m_file_storage.get())
	{
	}

	explicit uncompressed_serialization_file_handler(const fs::file& file) noexcept
		: utils::serialization_file_handler()
		, m_file_storage(nullptr)
		, m_file(std::addressof(file))
	{
	}

	uncompressed_serialization_file_handler(const uncompressed_serialization_file_handler&) = delete;
	uncompressed_serialization_file_handler& operator=(const uncompressed_serialization_file_handler&) = delete;

	// Handle file read and write requests
	bool handle_file_op(utils::serial& ar, usz pos, usz size, const void* data) override;

	// Get available memory or file size
	// Preferably memory size if is already greater/equal to recommended to avoid additional file ops
	usz get_size(const utils::serial& ar, usz recommended) const override;

	void finalize(utils::serial& ar) override;
};

template <typename File> requires (std::is_same_v<std::remove_cvref_t<File>, fs::file>)
inline std::unique_ptr<uncompressed_serialization_file_handler> make_uncompressed_serialization_file_handler(File&& file)
{
	ensure(file);
	return std::make_unique<uncompressed_serialization_file_handler>(std::forward<File>(file));
}

struct compressed_stream_data;

// Compressed file serialization handler
struct compressed_serialization_file_handler : utils::serialization_file_handler
{
	explicit compressed_serialization_file_handler(fs::file&& file) noexcept
		: utils::serialization_file_handler()
		, m_file_storage(std::make_unique<fs::file>(std::move(file)))
		, m_file(m_file_storage.get())
	{
	}

	explicit compressed_serialization_file_handler(const fs::file& file) noexcept
		: utils::serialization_file_handler()
		, m_file_storage(nullptr)
		, m_file(std::addressof(file))
	{
	}

	compressed_serialization_file_handler(const compressed_serialization_file_handler&) = delete;
	compressed_serialization_file_handler& operator=(const compressed_serialization_file_handler&) = delete;

	// Handle file read and write requests
	bool handle_file_op(utils::serial& ar, usz pos, usz size, const void* data) override;

	// Get available memory or file size
	// Preferably memory size if is already greater/equal to recommended to avoid additional file ops
	usz get_size(const utils::serial& ar, usz recommended) const override;
	void skip_until(utils::serial& ar) override;

	bool is_valid() const override
	{
		return !m_errored;
	}

	void finalize(utils::serial& ar) override;

private:
	const std::unique_ptr<fs::file> m_file_storage;
	const std::add_pointer_t<const fs::file> m_file;
	std::vector<u8> m_stream_data;
	usz m_stream_data_index = 0;
	usz m_file_read_index = 0;
	retro_atomic_64_t m_pending_bytes{0}; // usz
	// Counted up at each wake for m_pending_bytes: the 32-bit word a writer
	// waiting for pending memory to go down waits on
	retro_atomic_int_t m_pending_wake{0};
	retro_atomic_int_t m_pending_signal{0}; // 0 or 1
	bool m_write_inited = false;
	bool m_read_inited = false;
	bool m_errored = false;
	std::shared_ptr<compressed_stream_data> m_stream;
	std::unique_ptr<named_thread<std::function<void()>>> m_stream_data_prepare_thread;
	std::unique_ptr<named_thread<std::function<void()>>> m_file_writer_thread;

	usz read_at(utils::serial& ar, usz read_pos, void* data, usz size);
	void initialize(utils::serial& ar);
	void stream_data_prepare_thread_op();
	void file_writer_thread_op();
	void blocked_compressed_write(const std::vector<u8>& data);
};

template <typename File> requires (std::is_same_v<std::remove_cvref_t<File>, fs::file>)
inline std::unique_ptr<compressed_serialization_file_handler> make_compressed_serialization_file_handler(File&& file)
{
	ensure(file);
	return std::make_unique<compressed_serialization_file_handler>(std::forward<File>(file));
}

struct compressed_zstd_stream_data;

// Compressed file serialization handler
struct compressed_zstd_serialization_file_handler : utils::serialization_file_handler
{
	explicit compressed_zstd_serialization_file_handler(fs::file&& file) noexcept
		: utils::serialization_file_handler()
		, m_file_storage(std::make_unique<fs::file>(std::move(file)))
		, m_file(m_file_storage.get())
	{
	}

	explicit compressed_zstd_serialization_file_handler(const fs::file& file) noexcept
		: utils::serialization_file_handler()
		, m_file_storage(nullptr)
		, m_file(std::addressof(file))
	{
	}

	compressed_zstd_serialization_file_handler(const compressed_zstd_serialization_file_handler&) = delete;
	compressed_zstd_serialization_file_handler& operator=(const compressed_zstd_serialization_file_handler&) = delete;

	// Handle file read and write requests
	bool handle_file_op(utils::serial& ar, usz pos, usz size, const void* data) override;

	// Get available memory or file size
	// Preferably memory size if is already greater/equal to recommended to avoid additional file ops
	usz get_size(const utils::serial& ar, usz recommended) const override;
	void skip_until(utils::serial& ar) override;

	bool is_valid() const override
	{
		return !retro_atomic_load_acquire_int(&m_errored);
	}

	void finalize(utils::serial& ar) override;

private:
	const std::unique_ptr<fs::file> m_file_storage;
	const std::add_pointer_t<const fs::file> m_file;
	std::vector<u8> m_stream_data;
	usz m_stream_data_index = 0;
	usz m_file_read_index = 0;
	bool m_write_inited = false;
	bool m_read_inited = false;
	mutable retro_atomic_int_t m_errored{0}; // 0 or 1

	usz m_input_buffer_index = 0;
	retro_atomic_64_t m_output_buffer_index{0}; // usz
	retro_atomic_64_t m_thread_buffer_index{0}; // usz

	// Bumped, and its waiters woken, as any thread takes data in or out of
	// a context: what a thread waiting for a slot to empty waits on (an
	// atomic_ptr can be waited on to fill, not to empty)
	retro_atomic_int_t m_taken{0};

	void taken()
	{
		retro_atomic_fetch_add_int(&m_taken, 1);
		retro_waitaddr_notify_all(&m_taken);
	}

	u32 taken_seen() const { return static_cast<u32>(retro_atomic_load_acquire_int(&m_taken)); }

	void wait_taken_from(u32 seen)
	{
		const retro_waitaddr_t w{&m_taken, seen};
		retro_waitaddr_wait(&w, 1, -1);
	}

	// Until @busy() is false, woken as data is taken: no clock
	template <typename F>
	void wait_taken(F&& busy)
	{
		for (u32 seen = taken_seen(); busy(); seen = taken_seen())
		{
			if (busy())
				wait_taken_from(seen);
		}
	}

	struct compression_thread_context_t
	{
		atomic_ptr<std::vector<u8>> m_input;
		atomic_ptr<std::vector<u8>> m_output;
		bool notify_pending = false;
		bool notified = false;
		std::unique_ptr<named_thread<std::function<void()>>> m_thread;
	};

	// One for every compression thread, made before any thread starts (they do not move)
	std::unique_ptr<compression_thread_context_t[]> m_compression_threads;
	usz m_compression_thread_count = 0;
	std::shared_ptr<compressed_zstd_stream_data> m_stream;
	std::unique_ptr<named_thread<std::function<void()>>> m_file_writer_thread;

	usz read_at(utils::serial& ar, usz read_pos, void* data, usz size);
	void initialize(utils::serial& ar);
	void stream_data_prepare_thread_op();
	void file_writer_thread_op();
};

template <typename File> requires (std::is_same_v<std::remove_cvref_t<File>, fs::file>)
inline std::unique_ptr<compressed_zstd_serialization_file_handler> make_compressed_zstd_serialization_file_handler(File&& file)
{
	ensure(file);
	return std::make_unique<compressed_zstd_serialization_file_handler>(std::forward<File>(file));
}

// Null file serialization handler
struct null_serialization_file_handler : utils::serialization_file_handler
{
	explicit null_serialization_file_handler() noexcept
	{
	}

	// Handle file read and write requests
	bool handle_file_op(utils::serial& ar, usz pos, usz size, const void* data) override;

	void finalize(utils::serial& ar) override;

	bool is_null() const override
	{
		return true;
	}
};

inline std::unique_ptr<null_serialization_file_handler> make_null_serialization_file_handler()
{
	return std::make_unique<null_serialization_file_handler>();
}
