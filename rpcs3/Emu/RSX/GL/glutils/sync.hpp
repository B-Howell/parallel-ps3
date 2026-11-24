#pragma once

#include "common.h"

namespace gl
{
	class fence
	{
		GLsync m_value = nullptr;
		mutable GLenum flags = GL_SYNC_FLUSH_COMMANDS_BIT;
		mutable bool signaled = false;

	public:

		fence() = default;
		~fence() = default;

		fence(const fence&) = delete;
		fence& operator = (const fence&) = delete;

		fence(fence&&) noexcept = default;
		fence& operator = (fence&&) noexcept = default;

		void create()
		{
			m_value = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			flags = GL_SYNC_FLUSH_COMMANDS_BIT;
		}

		void destroy()
		{
			glDeleteSync(m_value);
			m_value = nullptr;
		}

		void reset()
		{
			if (m_value != nullptr)
				destroy();

			create();
		}

		bool is_empty() const
		{
			return (m_value == nullptr);
		}

		bool check_signaled() const
		{
			ensure(m_value);

			if (signaled)
				return true;

			if (flags)
			{
				GLenum err = glClientWaitSync(m_value, flags, 0);
				flags = 0;

				if (!(err == GL_ALREADY_SIGNALED || err == GL_CONDITION_SATISFIED))
					return false;
			}
			else
			{
				GLint status = GL_UNSIGNALED;
				GLint tmp;

				glGetSynciv(m_value, GL_SYNC_STATUS, 4, &tmp, &status);

				if (status != GL_SIGNALED)
					return false;
			}

			signaled = true;
			return true;
		}

		// Waited for in the driver, which wakes this as the GPU passes the fence; the sync is kept. It polled the
		// status in a loop with no end, and when the poll saw it pass it still called it not signalled
		bool client_wait() const
		{
			ensure(m_value);

			if (!signaled)
			{
				GLenum err;

				do
				{
					err = glClientWaitSync(m_value, GL_SYNC_FLUSH_COMMANDS_BIT, ~0ull);
				}
				while (err == GL_TIMEOUT_EXPIRED);

				flags = 0;
				signaled = (err == GL_ALREADY_SIGNALED || err == GL_CONDITION_SATISFIED);

				if (!signaled)
				{
					rsx_log.error("gl::fence sync returned unknown error 0x%X", err);
				}
			}

			return signaled;
		}

		bool wait_for_signal()
		{
			client_wait();

			glDeleteSync(m_value);
			m_value = nullptr;

			return signaled;
		}

		void server_wait_sync() const
		{
			ensure(m_value != nullptr);
			glWaitSync(m_value, 0, GL_TIMEOUT_IGNORED);
		}
	};
}
