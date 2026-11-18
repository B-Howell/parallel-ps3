#pragma once
#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>
#include "overlay_animation.h"
#include "overlay_controls.h"

#include "Emu/Io/pad_types.h"

#include "Utilities/Timer.h"

#include "../Common/bitfield.hpp"

#include <set>
#include <functional>
#include <queues/mpsc_stack.h>


// Definition of user interface implementations
namespace rsx
{
	namespace overlays
	{
		enum class sound_effect
		{
			cursor,
			accept,
			cancel,
			osk_accept,
			osk_cancel,
			dialog_ok,
			dialog_error,
			trophy,
		};

		std::string get_sound_filepath(sound_effect sound);
		void play_sound(sound_effect sound, std::optional<f32> volume = std::nullopt);

		// An overlay thread's wait for the next frame's input, or a change it
		// must see (a dialog closed, the input stack changed, the emulator's
		// state): on g_libretro_events, read before looking (@seen) - no poll
		u32 overlay_seen();
		void overlay_wait(u32 seen);
		void overlay_changed();

		// Bitfield of UI signals to overlay manager
		enum status_bits : u32
		{
			invalidate_image_cache = 0x0001, // Flush the address-based image cache
		};

		// Non-interactable UI element
		struct overlay
		{
			u32 uid = umax;
			u32 type_index = umax;

			static constexpr u16 virtual_width = 1280;
			static constexpr u16 virtual_height = 720;
			bool use_window_space = false;

			u32 min_refresh_duration_us = 16600;
			retro_atomic_int_t visible{0}; // 0 or 1: drawn
			atomic_bitmask_t<status_bits> status_flags = {};

			virtual ~overlay() = default;

			virtual void update(u64 /*timestamp_us*/) {}
			virtual compiled_resource get_compiled() = 0;

			// The renderer, before update: what other threads handed over
			virtual void take_input() {}

			void refresh() const;
			virtual u16 get_virtual_width() const { return virtual_width; }
			virtual u16 get_virtual_height() const { return virtual_height; }
			virtual void set_render_viewport(u16 /*width*/, u16 /*height*/) {}
		};

		// Interactable UI element
		class user_interface : public overlay
		{
		public:
			// Move this somewhere to avoid duplication
			enum selection_code
			{
				ok = 0,
				new_save = -1,
				canceled = -2,
				error = -3,
				interrupted = -4,
				no = -5
			};

			static constexpr u64 m_auto_repeat_ms_interval_default = 200;

		protected:
			Timer m_input_timer;
			u64 m_auto_repeat_ms_interval = m_auto_repeat_ms_interval_default;
			std::set<pad_button> m_auto_repeat_buttons = {
				pad_button::dpad_up,
				pad_button::dpad_down,
				pad_button::dpad_left,
				pad_button::dpad_right,
				pad_button::rs_up,
				pad_button::rs_down,
				pad_button::rs_left,
				pad_button::rs_right,
				pad_button::ls_up,
				pad_button::ls_down,
				pad_button::ls_left,
				pad_button::ls_right
			};

			retro_atomic_int_t m_stop_input_loop{0}; // 0 or 1
			retro_atomic_int_t m_interactive{0}; // 0 or 1
			bool m_start_pad_interception = true;
			retro_atomic_int_t m_stop_pad_interception{0}; // 0 or 1
			retro_atomic_int_t m_input_thread_detached{0}; // 0 or 1
			retro_atomic_int_t thread_bits{0}; // the threads using it, a bit each (notified as one goes)
			bool m_keyboard_input_enabled = false; // Allow keyboard input
			bool m_keyboard_pad_handler_active = true; // Initialized as true to prevent keyboard input until proven otherwise.
			bool m_allow_input_on_pause = false;

			static thread_local u32 g_thread_bit;

			u32 alloc_thread_bit();

			std::function<void(s32 status)> on_close = nullptr;

			class thread_bits_allocator
			{
			public:
				thread_bits_allocator(user_interface* parent)
					: m_parent(parent)
				{
					m_thread_bit = m_parent->alloc_thread_bit();
					g_thread_bit = m_thread_bit;
				}

				~thread_bits_allocator()
				{
					retro_atomic_fetch_and_int(&m_parent->thread_bits, static_cast<int>(~m_thread_bit));
					retro_waitaddr_notify_all(&m_parent->thread_bits);
				}

			private:
				user_interface* m_parent;
				u32 m_thread_bit;
			};
			// What the input thread, and any other thread, hands this dialog:
			// run by the renderer before it next draws it, so the dialog's
			// elements are the renderer's alone and take no lock
			struct ui_event
			{
				mpsc_stack_node_t node{}; // first: the stack's link
				std::function<void()> run;
			};

			mpsc_stack_t m_ui_events{};

		public:
			s32 return_code = 0; // CELL_OK

			user_interface() { mpsc_stack_init(&m_ui_events); }
			~user_interface() override;

			// Any thread: @f run by the renderer before it next draws this
			// dialog; a flip asked for, so that it is drawn soon
			void post_call(std::function<void()> f);

			// The renderer's: what was handed over, in the order handed over
			void take_input() override;

			bool is_detached() const { return m_input_thread_detached; }
			void detach_input() { m_input_thread_detached.store(true); }

			compiled_resource get_compiled() override = 0;

			virtual void on_button_pressed(pad_button /*button_press*/, bool /*is_auto_repeat*/) {}
			virtual void on_key_pressed(u32 /*led*/, u32 /*mkey*/, u32 /*key_code*/, u32 /*out_key_code*/, bool /*pressed*/, std::u32string /*key*/) {}

			virtual void close(bool use_callback, bool stop_pad_interception);

			s32 run_input_loop(std::function<bool()> check_state = nullptr);
		};

		// A text set by one thread and taken by the renderer: published
		// whole, taken by exchange - no lock
		struct text_guard_t
		{
			atomic_ptr<std::string> text;

			void set_text(std::string t)
			{
				text = std::move(t);
			}

			std::pair<bool, std::string> get_text()
			{
				if (auto taken = text.exchange(null_ptr))
				{
					return { true, std::move(*taken) };
				}

				return { false, {} };
			}
		};
	}
}
