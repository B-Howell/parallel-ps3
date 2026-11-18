#include "stdafx.h"
#include "system_progress.hpp"

shared_ptr<std::string> progress_dialog_string_t::get_string_ptr() const noexcept
{
	shared_ptr<std::string> text;
	data_t old_val = load();

	while (true)
	{
		text.reset();

		if (old_val.text_count == 0)
		{
			return text;
		}

		if (old_val.text_index != umax)
		{
			if (text = g_progr_text_queue[old_val.text_index].load(); !text)
			{
				// Search for available text (until the time another thread sets index)
				const u64 queue_size = g_progr_text_queue.size();

				for (u64 i = queue_size - 1;; i--)
				{
					if (i == umax)
					{
						return text;
					}

					if ((text = g_progr_text_queue[i].load()))
					{
						return text;
					}
				}
			}
		}

		const data_t new_val = load();

		if (old_val.update_id == new_val.update_id)
		{
			break;
		}

		old_val = new_val;
	}

	return text;
}

scoped_progress_dialog::scoped_progress_dialog(std::string text) noexcept
{
	shared_ptr<std::string> installed_ptr = make_single_value(std::move(text));
	const shared_ptr<std::string> c_null_ptr;

	for (usz init_size = g_progr_text_queue.size(), new_text_index = std::max<usz>(init_size, 1) - 1;;)
	{
		if (new_text_index >= init_size)
		{
			// Search element in new memeory
			new_text_index++;
		}
		else if (new_text_index == 0)
		{
			// Search element in new memeory
			new_text_index = init_size;
		}
		else
		{
			new_text_index--;
		}

		auto& info = g_progr_text_queue[new_text_index];

		if (!info && info.compare_and_swap_test(c_null_ptr, installed_ptr))
		{
			m_text_index = ::narrow<u32>(new_text_index);
			break;
		}
	}

	// One more open, showing this one's text
	ensure(m_text_index < progress_dialog_string_t::c_field_mask);

	for (progress_dialog_string_t::data_t progr = g_progr_text.load();;)
	{
		progress_dialog_string_t::data_t next = progr;
		next.update_id++;
		next.text_index = m_text_index;
		next.text_count++;

		if (g_progr_text.cas(progr, next))
		{
			break;
		}

		progr = g_progr_text.load();
	}
}

scoped_progress_dialog& scoped_progress_dialog::operator=(std::string text) noexcept
{
	// Set text atomically
	g_progr_text_queue[m_text_index].store(make_single_value(std::move(text)));
	return *this;
}

scoped_progress_dialog::~scoped_progress_dialog() noexcept
{
	// Deallocate memory
	g_progr_text_queue[m_text_index].reset();

	for (progress_dialog_string_t::data_t progr = g_progr_text.load();;)
	{
		u64 restored_text_index = umax;

		if (progr.text_index == m_text_index && !g_progr_text_queue[m_text_index])
		{
			// Search for available text
			const u64 queue_size = g_progr_text_queue.size();

			for (u64 i = queue_size - 1;; i--)
			{
				if (i == umax)
				{
					restored_text_index = umax;
					break;
				}

				if (g_progr_text_queue[i].operator bool())
				{
					restored_text_index = i;
					break;
				}
			}
		}

		// One fewer open; if it showed this one's text, the text found
		// instead (or none)
		progress_dialog_string_t::data_t next = progr;
		next.text_count--;

		if (next.text_index == m_text_index)
		{
			next.text_index = restored_text_index == umax ? umax : ::narrow<u32>(restored_text_index);
			next.update_id++;
		}

		if (g_progr_text.cas(progr, next))
		{
			break;
		}

		// Changed in between: looked at again, the search too
		progr = g_progr_text.load();
	}
}
