#include "stdafx.h"
#include "overlay_manager.h"
#include <util/asm.hpp>

namespace rsx
{
	namespace overlays
	{
		display_manager::display_manager(int) noexcept
		{
			retro_retire_init(&m_retire);
			retro_atomic_ptr_init(&m_lists, new lists_t);

			m_input_thread = std::make_shared<named_thread<overlay_input_thread>>();
			(*m_input_thread)([this]()
			{
				input_thread_loop();
			});
		}

		display_manager::~display_manager()
		{
			if (m_input_thread)
			{
				// This keeps the input thread from looping again
				m_input_thread_abort.store(true);
				overlay_changed();

				// Wake it if it is asleep
				const input_thread_context_t wakeup_node =
				{
					"stop_node",
					nullptr,
					nullptr,
					nullptr,
					nullptr
				};
				m_input_token_stack.push(wakeup_node);

				// Aborted and joined: waits for it to finish (thread_base::join),
				// no spin
				*m_input_thread = thread_state::finished;
			}

			// No reader is left: the lists retired, and the ones published
			retro_retire_drain(&m_retire, free_lists);
			delete static_cast<lists_t*>(retro_atomic_exchange_ptr(&m_lists, nullptr));
		}

		std::shared_ptr<overlay> display_manager::get(u32 uid) const
		{
			return read([&](const lists_t& lists) -> std::shared_ptr<overlay>
			{
				for (const auto& iface : lists.views)
				{
					if (iface->uid == uid)
						return iface;
				}

				return {};
			});
		}

		void display_manager::remove(u32 uid)
		{
			std::vector<std::shared_ptr<overlay>> removed;

			change([&](lists_t& lists)
			{
				removed.clear();
				remove_uid(lists, uid, removed);
			});

			for (const auto& item : removed)
			{
				on_overlay_removed(item);
			}
		}

		void display_manager::dispose(const std::vector<u32>& uids)
		{
			change([&](lists_t& lists)
			{
				lists.dirty.erase
				(
					std::remove_if(lists.dirty.begin(), lists.dirty.end(), [&uids](std::shared_ptr<overlay>& e)
					{
						return std::find(uids.begin(), uids.end(), e->uid) != uids.end();
					}),
					lists.dirty.end()
				);
			});
		}

		bool display_manager::remove_type(lists_t& lists, u32 type_id, std::vector<std::shared_ptr<overlay>>& removed)
		{
			bool found = false;
			for (auto It = lists.views.begin(); It != lists.views.end();)
			{
				if (It->get()->type_index == type_id)
				{
					removed.push_back(*It);
					lists.dirty.push_back(std::move(*It));
					It = lists.views.erase(It);
					found = true;
				}
				else
				{
					++It;
				}
			}

			return found;
		}

		bool display_manager::remove_uid(lists_t& lists, u32 uid, std::vector<std::shared_ptr<overlay>>& removed)
		{
			for (auto It = lists.views.begin(); It != lists.views.end(); It++)
			{
				const auto e = It->get();
				if (e->uid == uid)
				{
					removed.push_back(*It);
					lists.dirty.push_back(std::move(*It));
					lists.views.erase(It);
					return true;
				}
			}

			return false;
		}

		void display_manager::start_audio(const std::string& audio_path)
		{
			if (audio_path.empty())
			{
				m_audio_player.reset();
				return;
			}

			rsx_log.notice("display_manager::start_audio: path='%s'", audio_path);

			m_audio_player = std::make_unique<audio_player>(audio_path, false, "");
			m_audio_player->set_active(true);
		}

		void display_manager::stop_audio()
		{
			if (m_audio_player)
			{
				rsx_log.notice("display_manager::stop_audio");
				m_audio_player.reset();
			}
		}

		void display_manager::on_overlay_activated(const std::shared_ptr<overlay>& /*item*/)
		{
			// TODO: Internal management, callbacks, etc
		}

		void display_manager::attach_thread_input(
			u32 uid,
			const std::string_view& name,
			std::function<void()> on_input_loop_enter,
			std::function<void(s32)> on_input_loop_exit,
			std::function<s32()> input_loop_override)
		{
			if (auto iface = std::dynamic_pointer_cast<user_interface>(get(uid)))
			{
				// Add our interface to the queue: the input thread takes it up
				m_input_token_stack.push(
					name,
					std::move(iface),
					on_input_loop_enter,
					on_input_loop_exit,
					input_loop_override);

				// Signal input thread loop after pushing to avoid a race.
				m_input_thread_interrupted = true;
				overlay_changed();
			}
		}

		void display_manager::on_overlay_removed(const std::shared_ptr<overlay>& item)
		{
			auto iface = std::dynamic_pointer_cast<user_interface>(item);
			if (!iface)
			{
				// Not instance of UI, ignore
				return;
			}

			iface->detach_input();
		}

		void display_manager::input_thread_loop()
		{
			// Avoid tail recursion by reinserting pushed-down items
			std::vector<input_thread_context_t> interrupted_items;

			// This thread's own stack, oldest first: what it took from the
			// queue and what was interrupted, kept here rather than pushed
			// back, so no other thread's push gets in between
			std::vector<input_thread_context_t> own_stack;
			std::vector<input_thread_context_t> to_run;

			while (!m_input_thread_abort)
			{
				// We're about to load the whole list, interruption makes no sense before this point
				m_input_thread_interrupted = false;

				// Those pushed since, after those kept; then run newest first
				for (auto&& input_context : m_input_token_stack.pop_all())
				{
					own_stack.push_back(input_context);
				}

				to_run.assign(own_stack.rbegin(), own_stack.rend());
				own_stack.clear();

				for (auto&& input_context : to_run)
				{
					if (!input_context.target || input_context.target->is_detached())
					{
						continue;
					}

					if (m_input_thread_interrupted)
					{
						// Someone just pushed something onto the stack. Check if we already saw it.
						if (m_input_token_stack)
						{
							// We actually have new items to read out. Skip the remaining list.
							interrupted_items.push_back(input_context);
							continue;
						}

						// False alarm, we already saw it.
						m_input_thread_interrupted = false;
					}

					if (input_context.input_loop_prologue &&
						!input_context.prologue_completed)
					{
						input_context.input_loop_prologue();
						input_context.prologue_completed = true;
					}

					s32 result = 0;

					if (!input_context.input_loop_override) [[ likely ]]
					{
						result = input_context.target->run_input_loop([this]()
						{
							// Stop if interrupt status is set or input stack is empty
							return !m_input_thread_interrupted || !m_input_token_stack;
						});
					}
					else
					{
						result = input_context.input_loop_override();
					}

					if (result == user_interface::selection_code::interrupted)
					{
						// This dialog was exited prematurely, so we must re-run it's input routine later.
						ensure(m_input_thread_interrupted);
						ensure(m_input_token_stack);
						interrupted_items.push_back(input_context);
						continue;
					}

					if (input_context.input_loop_epilogue)
					{
						input_context.input_loop_epilogue(result);
					}
					else if (result && result != user_interface::selection_code::canceled)
					{
						rsx_log.error("%s exited with error code=%d", input_context.name, result);
					}
				}

				if (!interrupted_items.empty())
				{
					// Kept in this thread's stack, below what was pushed since,
					// which the next round takes up after them
					for (auto it = interrupted_items.crbegin(); it != interrupted_items.crend(); ++it)
					{
						own_stack.push_back(*it);
					}

					// Clear
					interrupted_items.clear();
				}
				else if (!m_input_thread_abort)
				{
					thread_ctrl::wait_on(m_input_token_stack);
				}
			}
		}
	}
}
