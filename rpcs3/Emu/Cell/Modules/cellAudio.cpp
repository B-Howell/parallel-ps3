#include "stdafx.h"
#include "Emu/emu_callbacks.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/Audio/audio_utils.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/Cell/lv2/sys_process.h"
#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Cell/lv2/sys_mmapper.h"
#include "Emu/Cell/lv2/sys_memory.h"
#include "Emu/Memory/vm_var.h"
#include "Emu/savestate_utils.hpp"
#include "util/asm.hpp"
#include "util/vm.hpp"
#include "sysPrxForUser.h"
#include "cellAudio.h"
#include "cellAudio_mix.h"
#include "Emu/Cell/Modules/hle_service.h"
#include "util/video_provider.h"

#include <cmath>
#include <retro_atomic.h>
#include <rthreads/retro_waitaddr.h>

LOG_CHANNEL(cellAudio);

extern retro_atomic_int_t g_recording_mode; // a recording_mode

extern void lv2_sleep(u64 timeout, ppu_thread* ppu = nullptr);

template <>
void fmt_class_string<CellAudioError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](CellAudioError value)
	{
		switch (value)
		{
		STR_CASE(CELL_AUDIO_ERROR_ALREADY_INIT);
		STR_CASE(CELL_AUDIO_ERROR_AUDIOSYSTEM);
		STR_CASE(CELL_AUDIO_ERROR_NOT_INIT);
		STR_CASE(CELL_AUDIO_ERROR_PARAM);
		STR_CASE(CELL_AUDIO_ERROR_PORT_FULL);
		STR_CASE(CELL_AUDIO_ERROR_PORT_ALREADY_RUN);
		STR_CASE(CELL_AUDIO_ERROR_PORT_NOT_OPEN);
		STR_CASE(CELL_AUDIO_ERROR_PORT_NOT_RUN);
		STR_CASE(CELL_AUDIO_ERROR_TRANS_EVENT);
		STR_CASE(CELL_AUDIO_ERROR_PORT_OPEN);
		STR_CASE(CELL_AUDIO_ERROR_SHAREDMEMORY);
		STR_CASE(CELL_AUDIO_ERROR_MUTEX);
		STR_CASE(CELL_AUDIO_ERROR_EVENT_QUEUE);
		STR_CASE(CELL_AUDIO_ERROR_AUDIOSYSTEM_NOT_FOUND);
		STR_CASE(CELL_AUDIO_ERROR_TAG_NOT_FOUND);
		}

		return unknown;
	});
}


cell_audio_config::cell_audio_config()
{
	raw = audio::get_raw_config();
}

void cell_audio_config::reset(bool backend_changed)
{
	if (!backend || backend_changed)
	{
		backend.reset();
		backend = g_emu_callbacks.get_audio();
	}

	cellAudio.notice("cellAudio initializing. Backend: %s", backend->GetName());

	const AudioFreq freq = AudioFreq::FREQ_48K;
	const AudioSampleSize sample_size = raw.convert_to_s16 ? AudioSampleSize::S16 : AudioSampleSize::FLOAT;

	const auto& [req_ch_cnt, downmix] = AudioBackend::get_channel_count_and_downmixer(0); // CELL_AUDIO_OUT_PRIMARY
	f64 cb_frame_len = 0.0;
	u32 ch_cnt = 2;
	audio_channel_layout ch_layout = audio_channel_layout::stereo;

	if (backend->Open(raw.audio_device, freq, sample_size, req_ch_cnt, raw.channel_layout))
	{
		cb_frame_len = backend->GetCallbackFrameLen();
		ch_cnt = backend->get_channels();
		ch_layout = backend->get_channel_layout();
		cellAudio.notice("Opened audio backend (sampling_rate=%d, sample_size=%d, channels=%d, layout=%s)", backend->get_sampling_rate(), backend->get_sample_size(), backend->get_channels(), backend->get_channel_layout());
	}
	else
	{
		cellAudio.error("Failed to open audio backend. Make sure that no other application is running that might block audio access (e.g. Netflix).");
	}

	audio_downmix = downmix;
	backend_ch_cnt = ch_cnt;
	backend_channel_layout = ch_layout;
	audio_channels = static_cast<u32>(req_ch_cnt);
	audio_sampling_rate = static_cast<u32>(freq);
	audio_block_period = AUDIO_BUFFER_SAMPLES * 1'000'000 / audio_sampling_rate;
	audio_sample_size = static_cast<u32>(sample_size);
	audio_min_buffer_duration = cb_frame_len + u32{AUDIO_BUFFER_SAMPLES} * 2.0 / audio_sampling_rate; // Add 2 blocks to allow jitter compensation

	audio_buffer_length = AUDIO_BUFFER_SAMPLES * audio_channels;

	desired_buffer_duration = std::max(static_cast<s64>(audio_min_buffer_duration * 1000), raw.desired_buffer_duration) * 1000llu;
	buffering_enabled = raw.buffering_enabled && raw.renderer != audio_renderer::null;

	minimum_block_period = audio_block_period / 2;
	maximum_block_period = (6 * audio_block_period) / 5;

	desired_full_buffers = buffering_enabled ? static_cast<u32>(desired_buffer_duration / audio_block_period) + 3 : 2;
	num_allocated_buffers = desired_full_buffers + EXTRA_AUDIO_BUFFERS;

	fully_untouched_timeout = static_cast<u64>(audio_block_period) * 2;
	partially_untouched_timeout = static_cast<u64>(audio_block_period) * 4;

	// Warn if audio backend does not support all requested features
	if (raw.buffering_enabled && !buffering_enabled)
	{
		cellAudio.error("Audio backend %s does not support buffering, this option will be ignored.", backend->GetName());
	}
}

audio_ringbuffer::audio_ringbuffer(cell_audio_config& _cfg)
	: backend(_cfg.backend)
	, cfg(_cfg)
	, buf_sz(AUDIO_BUFFER_SAMPLES * _cfg.audio_channels)
{
	// Initialize buffers
	if (cfg.num_allocated_buffers > MAX_AUDIO_BUFFERS)
	{
		fmt::throw_exception("MAX_AUDIO_BUFFERS is too small");
	}

	for (u32 i = 0; i < cfg.num_allocated_buffers; i++)
	{
		buffer[i].reset(new float[buf_sz]{});
	}

	// Init audio dumper if enabled
	if (cfg.raw.dump_to_file)
	{
		m_dump.Open(static_cast<AudioChannelCnt>(cfg.audio_channels), static_cast<AudioFreq>(cfg.audio_sampling_rate), AudioSampleSize::FLOAT);
	}

	const f64 buffer_dur_mult = [&]()
	{
		if (cfg.buffering_enabled)
		{
			return cfg.desired_buffer_duration / 1'000'000.0 + 0.02; // Add 20ms to buffer to keep buffering algorithm happy
		}

		return cfg.audio_min_buffer_duration;
	}();

	const usz ring_bytes = static_cast<u32>(cfg.backend_ch_cnt * cfg.audio_sampling_rate * cfg.audio_sample_size * buffer_dur_mult);
	if (lr_audio_ring* lent = backend->LendRing(ring_bytes))
		ring = lent;
	else
		ensure(lr_audio_ring_init(&cb_ringbuf, ring_bytes));
	backend->SetStateCallback(std::bind(&audio_ringbuffer::backend_state_callback, this, std::placeholders::_1));
}

audio_ringbuffer::~audio_ringbuffer()
{
	if (get_backend_playing())
	{
		flush();
	}

	// The backend's ring stays the backend's: nothing waits for its reader
	backend->EndLend();
	backend->Close();
	lr_audio_ring_free(&cb_ringbuf);
}

float* audio_ringbuffer::get_buffer(u32 num) const
{
	AUDIT(num < cfg.num_allocated_buffers);
	AUDIT(buffer[num]);
	return buffer[num].get();
}

void audio_ringbuffer::backend_state_callback(AudioStateEvent event)
{
	if (event == AudioStateEvent::DEFAULT_DEVICE_MAYBE_CHANGED)
	{
		retro_atomic_store_release_int(&backend_device_changed, 1);
	}
}

// The guest's clock, slaved to retro_run: the audio periods come as the
// guest's time passes - none while the frontend is paused, all of them in
// fast-forward - as the RSX audio path's do
u64 audio_ringbuffer::get_timestamp()
{
	return get_hw_clock_time();
}

// The cellAudio thread's waits: @guest_us of the guest's time, at the pace
// retro_run comes, woken early by each frame of it and by a call of the
// game's, which is run then. No sleep of the host's
static void wait_guest(owner_calls_t* calls, u64 guest_us)
{
	const int wake = retro_atomic_load_acquire_int(owner_calls_wake_word(calls));
	owner_calls_run(calls);

	const retro_atomic_int_t* clock_word = static_cast<const retro_atomic_int_t*>(libretro_guest_clock_word());
	const retro_waitaddr_t list[2] =
	{
		{clock_word, static_cast<u32>(retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t*>(clock_word)))},
		{owner_calls_wake_word(calls), static_cast<u32>(wake)},
	};
	retro_waitaddr_wait(list, 2, static_cast<s64>(libretro_guest_wait_as_host(guest_us)));
	owner_calls_run(calls);
}

// On the cellAudio thread: where the game's calls run
static thread_local bool s_on_audio_thread = false;

// @f run on the cellAudio thread, its result given back
template <typename F>
static auto audio_call(F&& f) -> decltype(f())
{
	return cpu_owner_call(&g_fxo->get<cell_audio>().calls, s_on_audio_thread, std::forward<F>(f));
}

float* audio_ringbuffer::get_current_buffer() const
{
	return get_buffer(cur_pos);
}

u64 audio_ringbuffer::get_enqueued_samples() const
{
	AUDIT(cfg.buffering_enabled);
	return lr_audio_ring_used(ring) / (cfg.audio_sample_size * cfg.backend_ch_cnt);
}

u64 audio_ringbuffer::get_enqueued_playtime() const
{
	AUDIT(cfg.buffering_enabled);

	return get_enqueued_samples() * 1'000'000 / cfg.audio_sampling_rate;
}

void audio_ringbuffer::enqueue(bool enqueue_silence, bool force)
{
	AUDIT(cur_pos < cfg.num_allocated_buffers);

	// Prepare buffer
	static float silence_buffer[u32{AUDIO_MAX_CHANNELS_COUNT} * u32{AUDIO_BUFFER_SAMPLES}]{};
	float* buf = silence_buffer;

	if (!enqueue_silence)
	{
		buf = buffer[cur_pos].get();
		cur_pos = (cur_pos + 1) % cfg.num_allocated_buffers;
	}

	if (!lr_audio_ring_active(ring) && !force)
	{
		// backend is not ready yet
		return;
	}

	// Enqueue audio: straight into the backend's buffer. The frontend keeps
	// the stream in step with what it plays (RetroArch's dynamic rate control).
	commit_data(buf, AUDIO_BUFFER_SAMPLES);
}

void audio_ringbuffer::enqueue_silence(u32 buf_count, bool force)
{
	for (u32 i = 0; i < buf_count; i++)
	{
		enqueue(true, force);
	}
}

void audio_ringbuffer::commit_data(f32* buf, u32 sample_cnt)
{
	const u32 sample_cnt_in = sample_cnt * cfg.audio_channels;
	const u32 sample_cnt_out = sample_cnt * cfg.backend_ch_cnt;

	// Dump audio if enabled
	m_dump.WriteData(buf, sample_cnt_in * static_cast<u32>(AudioSampleSize::FLOAT));

	// Record audio if enabled
	if (static_cast<recording_mode>(retro_atomic_load_acquire_int(&g_recording_mode)) != recording_mode::stopped)
	{
		utils::video_provider& provider = g_fxo->get<utils::video_provider>();
		provider.present_samples(reinterpret_cast<const u8*>(buf), sample_cnt, cfg.audio_channels);
	}

	// Downmix if necessary
	AudioBackend::downmix(sample_cnt_in, cfg.audio_channels, cfg.backend_channel_layout, buf, buf);

	if (cfg.backend->get_convert_to_s16())
	{
		AudioBackend::convert_to_s16(sample_cnt_out, buf, buf);
	}

	lr_audio_ring_push(ring, buf, sample_cnt_out * cfg.audio_sample_size, false);
}

void audio_ringbuffer::play()
{
	if (playing)
	{
		return;
	}

	playing = true;
	play_timestamp = get_timestamp();
	backend->Play();
}

void audio_ringbuffer::flush()
{
	backend->Pause();
	lr_audio_ring_flush(ring);
	playing = false;
}

u64 audio_ringbuffer::update(bool emu_is_paused)
{
	// Check emulator pause state
	if (emu_is_paused)
	{
		// Emulator paused
		if (playing)
		{
			flush();
		}
	}
	else
	{
		// Emulator unpaused
		play();
	}

	// Prepare timestamp
	const u64 timestamp = get_timestamp();

	// Store and return timestamp
	update_timestamp = timestamp;
	return timestamp;
}

void audio_port::tag(be_t<f32>* port_buf)
{
	// This tag will be used to make sure that the game has finished writing the audio for the next audio period
	// We use -0.0f in case games check if the buffer is empty. -0.0f == 0.0f evaluates to true, but std::signbit can be used to distinguish them
	const f32 tag = -0.0f;

	const u32 tag_first_pos = num_channels == 2 ? PORT_BUFFER_TAG_FIRST_2CH : PORT_BUFFER_TAG_FIRST_8CH;
	const u32 tag_delta = num_channels == 2 ? PORT_BUFFER_TAG_DELTA_2CH : PORT_BUFFER_TAG_DELTA_8CH;

	for (u32 tag_pos = tag_first_pos, tag_nr = 0; tag_nr < PORT_BUFFER_TAG_COUNT; tag_pos += tag_delta, tag_nr++)
	{
		port_buf[tag_pos] = tag;
		last_tag_value[tag_nr] = -0.0f;
	}

	// Mark the front right channel as well, see PORT_BUFFER_MARK_CHANNEL
	if (num_channels != 2)
	{
		for (u32 tag_nr = 0; tag_nr < PORT_BUFFER_TAG_COUNT; tag_nr++)
		{
			port_buf[mark_position(tag_nr)] = tag;
		}
	}

	prev_touched_tag_nr = -1;
}

namespace
{
	void save_audio_memory(utils::serial& ar, const shared_ptr<lv2_memory>& memory)
	{
		ar(static_cast<bool>(memory));

		if (!memory)
		{
			return;
		}

		const bool registered = retro_atomic_load_acquire_int(&memory->exists) || memory->key == SYS_MMAPPER_MIO_SHM_KEY;
		ar(registered);

		if (registered)
		{
			ar(memory->key);
		}
		else
		{
			memory->save_data(ar);
		}
	}

	shared_ptr<lv2_memory> load_audio_memory(utils::serial& ar)
	{
		if (!ar.pop<bool>())
		{
			return {};
		}

		if (ar.pop<bool>())
		{
			return ensure(g_fxo->get<ipc_manager<lv2_memory, u64>>().get(ar.pop<u64>()));
		}

		auto memory = make_shared<lv2_memory>(stx::exact_t<utils::serial&>(ar));
		ensure(retro_atomic_load_acquire_int(&memory->external_refs));
		ensure((g_fxo->get<ipc_manager<lv2_memory, u64>>().add(memory->key, [&] { return memory; }).first));
		return memory;
	}

	shared_ptr<lv2_memory> acquire_audio_memory(u32 id)
	{
		// Found with no lock over the IDs; the reference it counts on the
		// memory is what freeing the memory asks about, so that waits for
		// this (idm::get_entered)
		return idm::get_entered<lv2_obj, lv2_memory>(id, [](lv2_memory& memory)
		{
			retro_atomic_fetch_add_int(&memory.external_refs, 1);
		});
	}

	void release_audio_memory(shared_ptr<lv2_memory>& memory)
	{
		if (memory)
		{
			// No lock over the IDs: whichever of this and the memory's free
			// sees both its references gone releases it, once (release_memory)
			ensure(retro_atomic_fetch_add_seq_cst_int(&memory->external_refs, -1));
			memory->release_memory();
			memory.reset();
		}
	}
}

cell_audio_thread::cell_audio_thread(utils::serial& ar)
	: cell_audio_thread()
{
	u8 init_saved = 0;
	ar(init_saved, shared_area, shared_address, shared_refs, ports);
	retro_atomic_store_release_int(&init, init_saved);

	if (!init_saved)
	{
		return;
	}

	ar(key_count, event_period);

	keys.resize(ar.pop<u64>());

	for (key_info& k : keys)
	{
		ar(k.start_period, k.flags, k.source);
		k.port = lv2_event_queue::load_ptr(ar, k.port, "audio");
	}

	ar(free_port_count, free_ports, free_indices);
	shared_memory = load_audio_memory(ar);
	for (auto& memory : port_memories)
	{
		memory = load_audio_memory(ar);
	}
}

void cell_audio_thread::save(utils::serial& ar)
{
	USING_SERIALIZATION_VERSION(cellAudio);

	// closed ports keep their descriptor after audio quits
	const u8 init_saved = static_cast<u8>(retro_atomic_load_acquire_int(&init));
	ar(init_saved, shared_area, shared_address, shared_refs, ports);

	if (!init_saved)
	{
		return;
	}

	ar(key_count, event_period);
	ar(static_cast<u64>(keys.size()));

	for (const key_info& k : keys)
	{
		ar(k.start_period, k.flags, k.source);
		lv2_event_queue::save_ptr(ar, k.port.get());
	}

	ar(free_port_count, free_ports, free_indices);
	save_audio_memory(ar, shared_memory);
	for (const auto& memory : port_memories)
	{
		save_audio_memory(ar, memory);
	}
}

be_t<f32>* cell_audio_thread::get_buffer(const audio_port& port, s32 offset) const
{
	// caller holds the audio mutex, port_memories keeps the buffer alive after the game unmaps it
	return reinterpret_cast<be_t<f32>*>((*port_memories[port.number]->shm.observe())->get()) + port.position(offset) * port.block_size();
}

std::tuple<u32, u32, u32, u32> cell_audio_thread::count_port_buffer_tags()
{
	AUDIT(cfg.buffering_enabled);

	u32 active = 0;
	u32 in_progress = 0;
	u32 untouched = 0;
	u32 incomplete = 0;

	for (u32 port_index = 0; port_index < ports.size(); port_index++)
	{
		audio_port& port = ports[port_index];

		if (port.get_state() != audio_port_state::started) continue;
		active++;

		if (!port.num_channels) continue;

		auto port_buf = get_buffer(port);

		// Find the last tag that has been touched
		const u32 tag_first_pos = port.num_channels == 2 ? PORT_BUFFER_TAG_FIRST_2CH : PORT_BUFFER_TAG_FIRST_8CH;
		const u32 tag_delta = port.num_channels == 2 ? PORT_BUFFER_TAG_DELTA_2CH : PORT_BUFFER_TAG_DELTA_8CH;

		u32 last_touched_tag_nr = port.prev_touched_tag_nr;
		bool retouched = false;
		bool tag_moved = false;
		for (u32 tag_pos = tag_first_pos, tag_nr = 0; tag_nr < PORT_BUFFER_TAG_COUNT; tag_pos += tag_delta, tag_nr++)
		{
			const f32 val = port_buf[tag_pos];
			f32& last_val = port.last_tag_value[tag_nr];

			if (val != last_val || (last_val == -0.0f && std::signbit(last_val) && !std::signbit(val)))
			{
				last_val = val;

				retouched |= (tag_nr <= port.prev_touched_tag_nr) && port.prev_touched_tag_nr != umax;
				last_touched_tag_nr = tag_nr;
				tag_moved = true;
			}
		}

		if (tag_moved)
		{
			// This port reaches its surround channels, so its tags track it on their own and the marks below
			// are never consulted for it.
			m_periods_without_tag[port_index] = 0;
			m_front_only_port[port_index] = false;
		}

		// Decide whether the buffer is untouched, in progress, incomplete, or complete
		if (last_touched_tag_nr == umax)
		{
			// No tag has been touched yet. That normally means the buffer is untouched, but it also happens
			// when the game filled the front channels only: no tag can ever move then, and the port would
			// stay silent forever. The marks tell the two apart. They are only consulted once this port has
			// gone long enough without moving a tag to rule out a game writing the buffer one channel at a
			// time, which looks identical in between its front right and its center pass.
			bool& front_only = m_front_only_port[port_index];

			if (!front_only && port.num_channels != 2 && m_periods_without_tag[port_index] > PORT_FRONT_ONLY_SETTLE_PERIODS)
			{
				for (u32 tag_nr = 0; tag_nr < PORT_BUFFER_TAG_COUNT; tag_nr++)
				{
					const f32 val = port_buf[port.mark_position(tag_nr)];

					if (val != -0.0f || !std::signbit(val))
					{
						front_only = true;
						break;
					}
				}

				if (front_only && !m_front_only_reported[port_index])
				{
					m_front_only_reported[port_index] = true;
					cellAudio.notice("Port %u carries front channel audio only (num_channels=%u). Its buffer tags all sit on surround channels and can never move, so the front channel marks decide whether it is silent.", port.number, port.num_channels);
				}
			}

			if (!front_only)
			{
				// nothing has been written at all, or this port has not settled yet
				untouched++;
			}
			else
			{
				// The game only ever fills the front channels of this port, so the block is as complete as it
				// is ever going to get. Incomplete is the verdict that says so, and it still gets mixed.
				incomplete++;
			}
		}
		else if (last_touched_tag_nr == PORT_BUFFER_TAG_COUNT - 1)
		{
			if (retouched)
			{
				// we retouched, so wait at least once more to make sure no more tags get touched
				in_progress++;
			}

			// buffer has been completely filled
			port.prev_touched_tag_nr = last_touched_tag_nr;
		}
		else if (last_touched_tag_nr == port.prev_touched_tag_nr)
		{
			if (retouched)
			{
				// we retouched, so wait at least once more to make sure no more tags get touched
				in_progress++;
			}
			else
			{
				// hasn't been touched since the last call
				incomplete++;
			}
		}
		else
		{
			// the touched tag changed since the last call
			in_progress++;
			port.prev_touched_tag_nr = last_touched_tag_nr;
		}
	}

	return std::make_tuple(active, in_progress, untouched, incomplete);
}

void cell_audio_thread::reset_ports(s32 offset)
{
	// Memset buffer to 0 and tag
	for (u32 port_index = 0; port_index < ports.size(); port_index++)
	{
		audio_port& port = ports[port_index];

		if (port.get_state() != audio_port_state::started) continue;

		auto* buffer = get_buffer(port, offset);
		memset(buffer, 0, port.block_size() * sizeof(float));

		if (cfg.buffering_enabled && port.num_channels)
		{
			port.tag(buffer);

			// One period is about to elapse for this port. count_port_buffer_tags() zeroes this again as
			// soon as it sees a tag move, so it counts periods during which none did.
			if (m_periods_without_tag[port_index] < 0xFF)
			{
				m_periods_without_tag[port_index]++;
			}
		}
	}
}

retro_atomic_int_t g_audio_periods{0};
retro_atomic_int_t g_audio_period_waiters{0};

void cell_audio_thread::advance(u64 timestamp)
{
	// update ports
	reset_ports(0);

	be_t<u64>* indices = nullptr;

	for (audio_port& port : ports)
	{
		if (port.get_state() != audio_port_state::started) continue;

		port.global_counter = m_counter;
		port.active_counter++;
		port.timestamp = timestamp;

		port.cur_pos = port.position(1);

		if (!indices)
		{
			indices = reinterpret_cast<be_t<u64>*>((*shared_memory->shm.observe())->get());
		}

		indices[port.server_index * 2] = port.cur_pos;
	}

	if (cfg.buffering_enabled)
	{
		// Calculate rolling average of enqueued playtime
		m_average_playtime = cfg.period_average_alpha * ringbuffer->get_enqueued_playtime() + (1.0f - cfg.period_average_alpha) * m_average_playtime;
	}

	m_counter++;
	m_last_period_end = timestamp;

	retro_atomic_fetch_add_seq_cst_int(&g_audio_periods, 1);
	if (retro_atomic_load_seq_cst_int(&g_audio_period_waiters))
	{
		retro_waitaddr_notify_all(&g_audio_periods);
	}
	m_dynamic_period = 0;

	// send aftermix event (normal audio event)
	std::array<shared_ptr<lv2_event_queue>, MAX_AUDIO_EVENT_QUEUES> queues;
	u32 queue_count = 0;

	event_period++;

	for (const key_info& key_inf : keys)
	{
		if (key_inf.flags & CELL_AUDIO_EVENTFLAG_NOMIX)
		{
			continue;
		}

		if ((queues[queue_count] = key_inf.port))
		{
			u32 periods = 1;

			if (key_inf.flags & CELL_AUDIO_EVENTFLAG_DECIMATE_2)
			{
				periods *= 2;
			}

			if (key_inf.flags & CELL_AUDIO_EVENTFLAG_DECIMATE_4)
			{
				// If both flags are set periods is set to x8
				periods *= 4;
			}

			if ((event_period ^ key_inf.start_period) & (periods - 1))
			{
				// The time has not come for this key to receive event
				queues[queue_count].reset();
				continue;
			}

			event_sources[queue_count] = key_inf.source;
			event_data3[queue_count] = (key_inf.flags & CELL_AUDIO_EVENTFLAG_BEFOREMIX) ? key_inf.source : 0;
			queue_count++;
		}
	}

	for (u32 i = 0; i < queue_count; i++)
	{
		lv2_obj::notify_all_t notify;
		queues[i]->send(event_sources[i], CELL_AUDIO_EVENT_MIX, 0, event_data3[i]);
	}
}

namespace audio
{
	cell_audio_config::raw_config get_raw_config()
	{
		return
		{
			.audio_device = g_cfg.audio.audio_device,
			.buffering_enabled = static_cast<bool>(g_cfg.audio.enable_buffering),
			.desired_buffer_duration = g_cfg.audio.desired_buffer_duration,
			.convert_to_s16 = static_cast<bool>(g_cfg.audio.convert_to_s16),
			.dump_to_file = static_cast<bool>(g_cfg.audio.dump_to_file),
			.channel_layout = g_cfg.audio.channel_layout,
			.renderer = g_cfg.audio.renderer,
			.provider = g_cfg.audio.provider
		};
	}

	void configure_audio(bool force_reset)
	{
		if (g_cfg.audio.provider != audio_provider::cell_audio)
		{
			return;
		}

		if (auto& g_audio = g_fxo->get<cell_audio>(); g_fxo->is_init<cell_audio>())
		{
			// Asked of the cellAudio thread, which reads the configuration and
			// compares it with its own: 1 to look, 2 to reset anyway. Never
			// waited for: this can be the frontend's thread
			for (int seen = retro_atomic_load_acquire_int(&g_audio.m_config_check);;)
			{
				const int want = force_reset ? 2 : std::max(seen, 1);

				if (want == seen || retro_atomic_cas_int(&g_audio.m_config_check, seen, want))
				{
					break;
				}

				seen = retro_atomic_load_acquire_int(&g_audio.m_config_check);
			}

			owner_calls_wake(&g_audio.calls);
		}
	}
}

void cell_audio_thread::update_config(bool backend_changed)
{
	// Clear ringbuffer
	ringbuffer.reset();

	// Reload config
	cfg.reset(backend_changed);

	// Allocate ringbuffer
	ringbuffer.reset(new audio_ringbuffer(cfg));

	// Reset thread state
	reset_counters();
}

void cell_audio_thread::reset_counters()
{
	m_counter = 0;
	m_start_time = ringbuffer->get_timestamp();
	m_last_period_end = m_start_time;
	m_dynamic_period = 0;
	m_audio_should_restart = true;
}

cell_audio_thread::cell_audio_thread()
{
	owner_calls_init(&calls);
}

void cell_audio_thread::operator()()
{
	s_on_audio_thread = true;

	// The game's calls are run until the thread stops; after, where they are made
	struct close_calls_t
	{
		owner_calls_t* calls;
		~close_calls_t() { owner_calls_close(calls); }
	} close_calls{&calls};

	// Initialize loop variables (regardless of provider in order to initialize timestamps)
	reset_counters();

	if (cfg.raw.provider != audio_provider::cell_audio)
	{
		// No audio of cellAudio's, but its calls are still the thread's
		while (thread_ctrl::state() != thread_state::aborting)
		{
			const int wake = retro_atomic_load_acquire_int(owner_calls_wake_word(&calls));
			owner_calls_run(&calls);
			const retro_waitaddr_t until{owner_calls_wake_word(&calls), static_cast<u32>(wake)};
			retro_waitaddr_wait(&until, 1, -1);
		}

		return;
	}

	// Init audio config
	cfg.reset();

	// Allocate ringbuffer
	ringbuffer.reset(new audio_ringbuffer(cfg));

	thread_ctrl::scoped_priority high_prio(+1);

	while (Emu.IsPausedOrReady())
	{
		wait_guest(&calls, 5000);
	}

	u32 untouched_expected = 0;

	// A port the game feeds irregularly - the audio port of a movie that has just been started and whose
	// decoder is still spinning up, typically - makes untouched oscillate. Without hysteresis the
	// expectation follows it straight back down, so the next period re-arms the wait below and the thread
	// stalls again, up to partially_untouched_timeout every time. Hold the expectation for a while instead:
	// keeping it high only ever means "I already know that port is quiet, do not wait for it".
	u64 untouched_hold_until = 0;

	// Long enough to outlast the few-period oscillation a decoder produces while it spins up, short enough
	// that a port going quiet for good is noticed promptly.
	const u64 untouched_hold_periods = 340'000 / std::max<u32>(1, cfg.audio_block_period);

	const auto expect_untouched = [&](u32 count)
	{
		if (count >= untouched_expected)
		{
			untouched_expected = count;
			untouched_hold_until = m_counter + untouched_hold_periods;
		}
		else if (m_counter >= untouched_hold_until)
		{
			untouched_expected = count;
		}
	};

	u32 loop_count = 0;

	// Main cellAudio loop
	while (thread_ctrl::state() != thread_state::aborting)
	{
		loop_count++;
		owner_calls_run(&calls);

		// A configuration change asked of the thread: only reboot the audio
		// renderer if a relevant setting changed
		if (const int check = retro_atomic_exchange_int(&m_config_check, 0))
		{
			const cell_audio_config::raw_config new_raw = audio::get_raw_config();
			const cell_audio_config::raw_config raw = cfg.raw;

			if (check == 2 ||
				raw.audio_device != new_raw.audio_device ||
				raw.desired_buffer_duration != new_raw.desired_buffer_duration ||
				raw.buffering_enabled != new_raw.buffering_enabled ||
				raw.convert_to_s16 != new_raw.convert_to_s16 ||
				raw.renderer != new_raw.renderer ||
				raw.dump_to_file != new_raw.dump_to_file)
			{
				cellAudio.warning("Updating cell_audio_thread configuration");
				cfg.new_raw = new_raw;
				cfg.raw = new_raw;
				update_config(raw.renderer != new_raw.renderer);
			}
		}

		const bool emu_paused = Emu.IsPaused();
		const u64 timestamp = ringbuffer->update(emu_paused || m_backend_failed);

		if (emu_paused)
		{
			m_audio_should_restart = true;
			ringbuffer->flush();
			wait_guest(&calls, 10000);
			continue;
		}

		// TODO: (no idea how much of this is already implemented)
		// The hardware heartbeat interval of libaudio is ~5.3ms.
		// As soon as one interval starts, libaudio waits for ~2.6ms (half of the interval) before it mixes the audio.
		// There are 2 different types of games:
		// - Normal games:
		//     Once the audio was mixed, we send the CELL_AUDIO_EVENT_MIX event and the game can process audio.
		// - Latency sensitive games:
		//     If CELL_AUDIO_EVENTFLAG_BEFOREMIX is specified, we immediately send the CELL_AUDIO_EVENT_MIX event and the game can process audio.
		//     We then have to wait for a maximum of ~2.6ms for cellAudioSendAck and then mix immediately.

		const u64 time_since_last_period = timestamp - m_last_period_end;

		// Handle audio restart
		if (m_audio_should_restart)
		{
			// align to 5.(3)ms on global clock - some games seem to prefer this
			const s64 audio_period_alignment_delta = (timestamp - m_start_time) % cfg.audio_block_period;
			if (audio_period_alignment_delta > cfg.period_comparison_margin)
			{
				wait_guest(&calls, audio_period_alignment_delta - cfg.period_comparison_margin);
			}

			if (cfg.buffering_enabled)
			{
				// Restart algorithm
				cellAudio.trace("restarting audio");
				ringbuffer->enqueue_silence(cfg.desired_full_buffers, true);
				finish_port_volume_stepping();
				m_average_playtime = static_cast<f32>(ringbuffer->get_enqueued_playtime());
				untouched_expected = 0;
			}

			m_audio_should_restart = false;
			continue;
		}

		bool operational = ringbuffer->get_operational_status();

		if (!operational && loop_count % 128 == 0)
		{
			update_config(true);
			operational = ringbuffer->get_operational_status();
		}

		if (ringbuffer->device_changed())
		{
			cellAudio.warning("Default device changed, attempting to switch...");
			update_config(false);

			if (operational != ringbuffer->get_operational_status())
			{
				continue;
			}
		}

		if (!m_backend_failed && !operational)
		{
			cellAudio.error("Backend stopped unexpectedly (likely device change). Attempting to recover...");
			m_backend_failed = true;
		}
		else if (m_backend_failed && operational)
		{
			cellAudio.success("Backend recovered");
			m_backend_failed = false;
		}

		if (!cfg.buffering_enabled)
		{
			const u64 period_end = (m_counter * cfg.audio_block_period) + m_start_time;
			const s64 time_left = period_end - timestamp;

			if (time_left > cfg.period_comparison_margin)
			{
				wait_guest(&calls, time_left - cfg.period_comparison_margin);
				continue;
			}
		}
		else
		{
			const u64 enqueued_samples = ringbuffer->get_enqueued_samples();
			const u64 enqueued_playtime = ringbuffer->get_enqueued_playtime();
			const u64 enqueued_buffers = enqueued_samples / AUDIO_BUFFER_SAMPLES;

			const auto tag_info = count_port_buffer_tags();
			const u32 active_ports = std::get<0>(tag_info);
			const u32 in_progress  = std::get<1>(tag_info);
			const u32 untouched    = std::get<2>(tag_info);
			const u32 incomplete   = std::get<3>(tag_info);

			// Ratio between the rolling average of the audio period, and the desired audio period
			const f32 average_playtime_ratio = m_average_playtime / cfg.audio_buffer_length;

			// Use the above average ratio to decide how much buffer we should be aiming for
			f32 desired_duration_adjusted = cfg.desired_buffer_duration + (cfg.audio_block_period / 2.0f);
			if (average_playtime_ratio < 1.0f)
			{
				desired_duration_adjusted /= std::max(average_playtime_ratio, 0.25f);
			}

			//  1.0 means exactly as desired
			// <1.0 means not as full as desired
			// >1.0 means more full than desired
			const f32 desired_duration_rate = enqueued_playtime / desired_duration_adjusted;

			if (desired_duration_rate >= 1.0f)
			{
				// more full than desired
				const f32 multiplier = 1.0f / desired_duration_rate;
				m_dynamic_period = cfg.maximum_block_period - static_cast<u64>((cfg.maximum_block_period - cfg.audio_block_period) * multiplier);
			}
			else
			{
				// not as full as desired
				const f32 multiplier = desired_duration_rate * desired_duration_rate; // quite aggressive, but helps more times than it hurts
				m_dynamic_period = cfg.minimum_block_period + static_cast<u64>((cfg.audio_block_period - cfg.minimum_block_period) * multiplier);
			}

			const s64 time_left = m_dynamic_period - time_since_last_period;
			if (time_left > cfg.period_comparison_margin)
			{
				wait_guest(&calls, time_left - cfg.period_comparison_margin);
				continue;
			}

			// Fast path for 0 ports active
			if (active_ports == 0)
			{
				// no need to mix, just enqueue silence and advance time
				cellAudio.trace("enqueuing silence: no active ports, enqueued_buffers=%llu", enqueued_buffers);
				ringbuffer->enqueue_silence();
				untouched_expected = 0;
				advance(timestamp);
				continue;
			}

			// Wait until buffers have been touched
			//cellAudio.error("active=%u, in_progress=%u, untouched=%u, incomplete=%u", active_ports, in_progress, untouched, incomplete);
			bool waited_long_enough = false;

			if (untouched > untouched_expected)
			{
				// Games may sometimes "skip" audio periods entirely if they're falling behind (a sort of "frameskip" for audio)
				// As such, if the game doesn't touch buffers for too long we stop waiting for them
				if (
					(untouched == active_ports && time_since_last_period > cfg.fully_untouched_timeout) ||
					(time_since_last_period > cfg.partially_untouched_timeout) || g_cfg.audio.disable_sampling_skip
				   )
				{
					expect_untouched(untouched);

					if (untouched >= active_ports)
					{
						// There's no audio in any buffer, simply advance time and hope the game recovers
						cellAudio.trace("advancing time: untouched=%u/%u (expected=%u), enqueued_buffers=%llu", untouched, active_ports, untouched_expected, enqueued_buffers);
						advance(timestamp);
						continue;
					}

					// Some ports do hold audio for this period. Advancing without enqueueing would throw that
					// away as well, one block at a time, for as long as a port keeps alternating between
					// touched and untouched. Mix what is there instead: the untouched ports were memset, so
					// they contribute silence, which is exactly what they hold.
					cellAudio.trace("mixing partially untouched buffers: untouched=%u/%u, enqueued_buffers=%llu", untouched, active_ports, enqueued_buffers);
					waited_long_enough = true;
				}
				else
				{
					cellAudio.trace("waiting: untouched=%u/%u (expected=%u), enqueued_buffers=%llu", untouched, active_ports, untouched_expected, enqueued_buffers);
					wait_guest(&calls, 1000);
					continue;
				}
			}

			// Fast-path for when there is no audio in the buffers
			// (unreachable once we waited long enough: that path only runs with untouched < active_ports)
			if (untouched == active_ports)
			{
				// There's no audio in the buffers, simply advance time
				cellAudio.trace("enqueuing silence: untouched=%u/%u (expected=%u), enqueued_buffers=%llu", untouched, active_ports, untouched_expected, enqueued_buffers);
				ringbuffer->enqueue_silence();
				expect_untouched(untouched);
				advance(timestamp);
				continue;
			}

			// Wait for buffer(s) to be completely filled
			if (!waited_long_enough && in_progress > 0)
			{
				cellAudio.trace("waiting: in_progress=%u/%u, enqueued_buffers=%u", in_progress, active_ports, enqueued_buffers);
				wait_guest(&calls, 500);
				continue;
			}

			//cellAudio.error("active=%u, untouched=%u, in_progress=%d, incomplete=%d, enqueued_buffers=%u", active_ports, untouched, in_progress, incomplete, enqueued_buffers);

			// Store number of untouched buffers for future reference
			expect_untouched(untouched);

			// Log if we enqueued untouched/incomplete buffers
			if (untouched > 0 || incomplete > 0)
			{
				cellAudio.trace("enqueueing: untouched=%u/%u (expected=%u), incomplete=%u/%u enqueued_buffers=%llu", untouched, active_ports, untouched_expected, incomplete, active_ports, enqueued_buffers);
			}
		}

		// Mix
		float* buf = ringbuffer->get_current_buffer();

		switch (cfg.audio_channels)
		{
		case 2:
			switch (cfg.audio_downmix)
			{
			case AudioChannelCnt::SURROUND_7_1:
				mix<AudioChannelCnt::STEREO, AudioChannelCnt::SURROUND_7_1>(buf);
				break;
			case AudioChannelCnt::STEREO:
				mix<AudioChannelCnt::STEREO, AudioChannelCnt::STEREO>(buf);
				break;
			case AudioChannelCnt::SURROUND_5_1:
				mix<AudioChannelCnt::STEREO, AudioChannelCnt::SURROUND_5_1>(buf);
				break;
			}
			break;

		case 6:
			switch (cfg.audio_downmix)
			{
			case AudioChannelCnt::SURROUND_7_1:
				mix<AudioChannelCnt::SURROUND_5_1, AudioChannelCnt::SURROUND_7_1>(buf);
				break;
			case AudioChannelCnt::STEREO:
				mix<AudioChannelCnt::SURROUND_5_1, AudioChannelCnt::STEREO>(buf);
				break;
			case AudioChannelCnt::SURROUND_5_1:
				mix<AudioChannelCnt::SURROUND_5_1, AudioChannelCnt::SURROUND_5_1>(buf);
				break;
			}
			break;

		case 8:
			switch (cfg.audio_downmix)
			{
			case AudioChannelCnt::SURROUND_7_1:
				mix<AudioChannelCnt::SURROUND_7_1, AudioChannelCnt::SURROUND_7_1>(buf);
				break;
			case AudioChannelCnt::STEREO:
				mix<AudioChannelCnt::SURROUND_7_1, AudioChannelCnt::STEREO>(buf);
				break;
			case AudioChannelCnt::SURROUND_5_1:
				mix<AudioChannelCnt::SURROUND_7_1, AudioChannelCnt::SURROUND_5_1>(buf);
				break;
			}
			break;

		default:
			fmt::throw_exception("Unsupported channel count in cell_audio_config: %d", cfg.audio_channels);
		}

		// Enqueue
		ringbuffer->enqueue();

		// Advance time
		advance(timestamp);
	}

	// Destroy ringbuffer
	ringbuffer.reset();

	// Destroy the audio backend on this thread (the one that created it in cfg.reset()).
	// The backend's ctor calls CoInitializeEx here on Windows; releasing it on a different
	// thread (g_fxo->clear() runs on the GUI thread during Kill()) would land the matching
	// CoUninitialize on the GUI thread, draining its OLE reference and silently breaking the
	// main window's file drag&drop. Keep COM init/teardown balanced on this thread.
	cfg.backend.reset();
}

audio_port* cell_audio_thread::open_port()
{
	if (!free_port_count)
	{
		return nullptr;
	}

	const u32 index = --free_port_count;
	auto& port = ports[free_ports[index]];
	port.server_index = free_indices[index];
	port.set_state(audio_port_state::opened);
	m_front_only_reported[port.number] = false;
	m_periods_without_tag[port.number] = 0;
	m_front_only_port[port.number] = false;
	return &port;
}

// The caller's: a shared memory block of @size for a port, mapped into
// @area. Its id and address, and the memory; 0 when it could not be had
static u32 map_port_memory(ppu_thread& ppu, u32 size, u32 area, u32& address, shared_ptr<lv2_memory>& memory)
{
	auto* ct = g_ps3_process_info.sdk_ver > 0x21ffff ? &g_fxo->get<lv2_memory_container>() : nullptr;

	for (u64 key = SYS_MMAPPER_MIO_SHM_KEY + 2;; key++)
	{
		if (g_fxo->get<ipc_manager<lv2_memory, u64>>().get(key))
		{
			continue;
		}

		const auto result = lv2_obj::create<lv2_memory>(SYS_SYNC_PROCESS_SHARED, key, SYS_SYNC_NEWLY_CREATED, [&]()
		{
			return make_shared<lv2_memory>(size, 0x10000, 0x200, key, true, ct);
		});

		if (result + 0u == CELL_EEXIST)
		{
			continue;
		}

		if (result)
		{
			return 0;
		}

		const u32 memory_id = idm::last_id<lv2_memory>();
		const vm::var<u32> addr;
		memory = acquire_audio_memory(memory_id);

		if (!memory || sys_mmapper_search_and_map(ppu, area, memory_id, 0x40000, addr))
		{
			sys_mmapper_free_shared_memory(ppu, memory_id);
			release_audio_memory(memory);
			return 0;
		}

		address = *addr;
		return memory_id;
	}
}

cell_audio_thread::port_unmap cell_audio_thread::take_port(audio_port& port)
{
	port.set_state(audio_port_state::closed);

	port_unmap gone;
	gone.mapped = port.mapped;
	gone.addr = port.addr.addr();
	gone.memory = std::move(port_memories[port.number]);

	port.mapped = false;
	free_ports[free_port_count] = port.number;
	free_indices[free_port_count] = port.server_index;
	free_port_count++;
	return gone;
}

void cell_audio_thread::unmap_port(ppu_thread& ppu, port_unmap& gone)
{
	const vm::var<u32> memory_id;

	if (gone.mapped && !sys_mmapper_unmap_shared_memory(ppu, gone.addr, memory_id))
	{
		// an extra mapping can keep the handle alive after the port closes
		sys_mmapper_free_shared_memory(ppu, *memory_id);
	}

	release_audio_memory(gone.memory);
}

u32 cell_audio_thread::take_shared_ref()
{
	if (--shared_refs || !shared_address)
	{
		return 0;
	}

	return shared_address;
}

void cell_audio_thread::unmap_shared(ppu_thread& ppu, u32 address)
{
	if (!address)
	{
		return;
	}

	const vm::var<u32> memory_id;

	if (!sys_mmapper_unmap_shared_memory(ppu, address, memory_id))
	{
		sys_mmapper_free_shared_memory(ppu, *memory_id);
	}
}

template <AudioChannelCnt channels, AudioChannelCnt downmix>
void cell_audio_thread::mix(float* out_buffer, s32 offset)
{
	AUDIT(out_buffer != nullptr);

	constexpr u32 out_channels = static_cast<u32>(channels);
	constexpr u32 out_buffer_sz = out_channels * AUDIO_BUFFER_SAMPLES;

	const float master_volume = audio::get_volume();

	// Reset out_buffer
	std::memset(out_buffer, 0, out_buffer_sz * sizeof(float));

	// mixing
	for (audio_port& port : ports)
	{
		if (port.get_state() != audio_port_state::started) continue;

		if (port.num_channels != 2 && port.num_channels != 8 && port.num_channels != 0)
		{
			fmt::throw_exception("Unknown channel count (port=%u, channel=%d)", port.number, port.num_channels);
		}

		// The port's block into the output's, at the port's volume
		// (cellAudio_mix.c). Part of cellAudioSetPortLevel: a change of the
		// volume is spread over 13 ms, a step a frame. The level it is going
		// to cannot change meanwhile - cellAudioSetPortLevel takes the mutex
		// held here - so it is read once for the block, not once a frame.
		const audio_port::level_set_t param = port.get_level_set();
		cell_audio_mix_level level{port.level, param.value, param.inc};

		constexpr cell_audio_mix_down down = downmix == AudioChannelCnt::STEREO ? CELL_AUDIO_MIX_STEREO
			: downmix == AudioChannelCnt::SURROUND_5_1 ? CELL_AUDIO_MIX_5_1 : CELL_AUDIO_MIX_7_1;

		const bool arrived = cell_audio_mix_port(out_buffer, out_channels, get_buffer(port, offset),
			port.num_channels ? port.num_channels : 1, down, AUDIO_BUFFER_SAMPLES, master_volume, &level) != 0;

		port.level = level.level;

		if (arrived)
		{
			port.cas_level_set(param, { param.value, 0.0f });
		}
	}
}

void cell_audio_thread::finish_port_volume_stepping()
{
	// part of cellAudioSetPortLevel functionality
	for (audio_port& port : ports)
	{
		if (port.get_state() != audio_port_state::started) continue;

		const audio_port::level_set_t param = port.get_level_set();
		port.level = param.value;
		port.cas_level_set(param, { param.value, 0.0f });
	}
}

error_code cellAudioInit(ppu_thread& ppu)
{
	cellAudio.warning("cellAudioInit()");

	const std::unique_lock savestate_lock{ g_fxo->get<hle_locks_t>(), std::try_to_lock };

	if (!savestate_lock)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	auto& g_audio = g_fxo->get<cell_audio>();

	// Whether this call is the one to set the audio up: decided on the
	// cellAudio thread. Its memory is mapped here, then taken there
	const error_code claim = audio_call([&]() -> error_code
	{
		if (retro_atomic_load_acquire_int(&g_audio.init) || g_audio.initialising)
		{
			return CELL_AUDIO_ERROR_ALREADY_INIT;
		}

		if (g_audio.shared_refs++)
		{
			g_audio.take_shared_ref();
			return CELL_AUDIO_ERROR_TRANS_EVENT;
		}

		g_audio.shared_address = 0;
		g_audio.initialising = true;
		return CELL_OK;
	});

	if (claim)
	{
		return claim;
	}

	// Given up: as release_shared_memory did with nothing mapped yet
	const auto give_up = [&](u32 refs)
	{
		audio_call([&]()
		{
			for (u32 i = 0; i < refs; i++)
			{
				g_audio.take_shared_ref();
			}

			g_audio.initialising = false;
		});
	};

	const vm::var<u32> memory_id;
	const auto result = sys_mmapper_allocate_shared_memory(ppu, SYS_MMAPPER_MIO_SHM_KEY, 0x10000, 0x200, memory_id);

	if (result)
	{
		// mio and audio both release this on an already open handle
		if (result + 0u == CELL_EEXIST)
		{
			give_up(2);
			return CELL_AUDIO_ERROR_TRANS_EVENT;
		}

		give_up(1);
		return result;
	}

	const vm::var<u32> area;
	const vm::var<u32> address;
	auto memory = acquire_audio_memory(*memory_id);

	if (!memory)
	{
		give_up(1);
		return CELL_ESRCH;
	}

	auto mapping_result = sys_mmapper_get_shared_memory_area(ppu, 0x20full, +area);

	if (!mapping_result)
	{
		mapping_result = sys_mmapper_search_and_map(ppu, *area, *memory_id, 0x40000, address);
	}

	if (mapping_result)
	{
		sys_mmapper_free_shared_memory(ppu, *memory_id);
		release_audio_memory(memory);
		give_up(1);
		return mapping_result;
	}

	const u32 shared_area = *area;
	const u32 shared_address = *address;

	return audio_call([&]() -> error_code
	{
		g_audio.shared_memory = std::move(memory);
		g_audio.shared_area = shared_area;
		g_audio.shared_address = shared_address;

		const u32 port_count = g_ps3_process_info.sdk_ver <= 0x35ffff ? CELL_AUDIO_MAX_PORT : CELL_AUDIO_MAX_PORT_2;
		g_audio.free_port_count = port_count;

		for (u32 i = 0; i < AUDIO_PORT_COUNT; i++)
		{
			g_audio.ports[i].set_state(audio_port_state::closed);
			g_audio.ports[i].mapped = false;
			g_audio.ports[i].number = i;
			// closed ports keep their buffer fields and their slot in the new index mapping
			g_audio.ports[i].index = vm::cast(g_audio.shared_address + g_audio.ports[i].server_index * 16);

			if (i < port_count)
			{
				g_audio.free_ports[i] = port_count - i - 1;
				g_audio.free_indices[i] = port_count - i;
			}
		}

		g_audio.initialising = false;
		retro_atomic_store_release_int(&g_audio.init, 1);
		return CELL_OK;
	});
}

error_code cellAudioQuit(ppu_thread& ppu)
{
	cellAudio.warning("cellAudioQuit()");

	const std::unique_lock savestate_lock{ g_fxo->get<hle_locks_t>(), std::try_to_lock };

	if (!savestate_lock)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	auto& g_audio = g_fxo->get<cell_audio>();

	// The ports closed and the memory given up on the cellAudio thread;
	// unmapped here
	std::array<cell_audio_thread::port_unmap, AUDIO_PORT_COUNT> ports_gone{};
	u32 shared_gone = 0;
	shared_ptr<lv2_memory> shared_memory;

	const error_code result = audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		for (auto& port : g_audio.ports)
		{
			if (port.get_state() != audio_port_state::closed)
			{
				ports_gone[port.number] = g_audio.take_port(port);
			}
		}

		shared_gone = g_audio.take_shared_ref();
		shared_memory = std::move(g_audio.shared_memory);
		// NOTE: Do not clear event queues here. They are handled independently.
		retro_atomic_store_release_int(&g_audio.init, 0);
		return CELL_OK;
	});

	if (result)
	{
		return result;
	}

	for (auto& gone : ports_gone)
	{
		cell_audio_thread::unmap_port(ppu, gone);
	}

	cell_audio_thread::unmap_shared(ppu, shared_gone);
	release_audio_memory(shared_memory);
	return CELL_OK;
}

error_code cellAudioPortOpen(ppu_thread& ppu, vm::ptr<CellAudioPortParam> audioParam, vm::ptr<u32> portNum)
{
	cellAudio.warning("cellAudioPortOpen(audioParam=*0x%x, portNum=*0x%x)", audioParam, portNum);

	const std::unique_lock savestate_lock{ g_fxo->get<hle_locks_t>(), std::try_to_lock };

	if (!savestate_lock)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	auto& g_audio = g_fxo->get<cell_audio>();

	if (!retro_atomic_load_acquire_int(&g_audio.init))
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (!audioParam || !portNum)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	const u64 num_channels = audioParam->nChannel;
	const u64 num_blocks = audioParam->nBlock;
	const u64 attr = audioParam->attr;

	// check attributes
	if (num_channels != CELL_AUDIO_PORT_2CH &&
		num_channels != CELL_AUDIO_PORT_8CH &&
		num_channels != 0)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	if (num_blocks != 2 &&
		num_blocks != 4 &&
		num_blocks != CELL_AUDIO_BLOCK_8 &&
		num_blocks != CELL_AUDIO_BLOCK_16 &&
		num_blocks != CELL_AUDIO_BLOCK_32)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	// list unsupported flags
	if (attr & CELL_AUDIO_PORTATTR_BGM)
	{
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_BGM");
	}
	if (attr & CELL_AUDIO_PORTATTR_OUT_SECONDARY)
	{
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_SECONDARY");
	}
	if (attr & CELL_AUDIO_PORTATTR_OUT_PERSONAL_0)
	{
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_PERSONAL_0");
	}
	if (attr & CELL_AUDIO_PORTATTR_OUT_PERSONAL_1)
	{
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_PERSONAL_1");
	}
	if (attr & CELL_AUDIO_PORTATTR_OUT_PERSONAL_2)
	{
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_PERSONAL_2");
	}
	if (attr & CELL_AUDIO_PORTATTR_OUT_PERSONAL_3)
	{
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_PERSONAL_3");
	}
	if (attr & CELL_AUDIO_PORTATTR_OUT_NO_ROUTE)
	{
		cellAudio.todo("cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_NO_ROUTE");
	}
	if (attr & 0xFFFFFFFFF0EFEFEEULL)
	{
		cellAudio.todo("cellAudioPortOpen(): unknown attributes (0x%llx)", attr);
	}

	// Waiting for VSH and doing some more things
	lv2_sleep(200);

	// A port taken on the cellAudio thread, its memory mapped here and given
	// to it there
	audio_port* port = nullptr;
	u32 shared_area = 0;
	u32 size = 0;

	const error_code taken = audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		// Open audio port
		port = g_audio.open_port();

		if (!port)
		{
			return CELL_AUDIO_ERROR_PORT_FULL;
		}

		port->num_channels   = ::narrow<u32>(num_channels);
		port->num_blocks     = ::narrow<u32>(num_blocks);
		port->attr           = attr;
		port->size           = ::narrow<u32>(std::max<u64>(1, num_channels) * num_blocks * AUDIO_BUFFER_SAMPLES * sizeof(f32));
		port->cur_pos        = 0;
		port->global_counter = g_audio.m_counter;
		port->active_counter = 0;
		port->timestamp      = get_guest_system_time_of_slaved(g_audio.m_last_period_end);

		if (attr & CELL_AUDIO_PORTATTR_INITLEVEL)
		{
			port->level = audioParam->level;
		}
		else
		{
			port->level = 1.0f;
		}

		port->set_level_set({ port->level, 0.0f });

		shared_area = g_audio.shared_area;
		size = std::max<u32>(0x10000, utils::align(port->size, 0x10000));
		return CELL_OK;
	});

	if (taken)
	{
		return taken;
	}

	u32 address = 0;
	shared_ptr<lv2_memory> memory;

	if (!map_port_memory(ppu, size, shared_area, address, memory))
	{
		auto gone = audio_call([&]() { return g_audio.take_port(*port); });
		cell_audio_thread::unmap_port(ppu, gone);
		return CELL_AUDIO_ERROR_SHAREDMEMORY;
	}

	std::memset(vm::base(address), 0, size);

	// Given to the port, unless a cellAudioQuit came between and closed it
	const bool given = audio_call([&]()
	{
		if (port->get_state() != audio_port_state::opened || port->mapped)
		{
			return false;
		}

		g_audio.port_memories[port->number] = std::move(memory);
		port->addr = vm::cast(address);
		port->mapped = true;
		port->index = vm::cast(g_audio.shared_address + port->server_index * 16);
		return true;
	});

	if (!given)
	{
		cell_audio_thread::port_unmap gone{true, address, std::move(memory)};
		cell_audio_thread::unmap_port(ppu, gone);
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	*portNum = port->number;
	return CELL_OK;
}

error_code cellAudioGetPortConfig(u32 portNum, vm::ptr<CellAudioPortConfig> portConfig)
{
	cellAudio.trace("cellAudioGetPortConfig(portNum=%d, portConfig=*0x%x)", portNum, portConfig);

	auto& g_audio = g_fxo->get<cell_audio>();

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		if (!portConfig || portNum >= AUDIO_PORT_COUNT)
		{
			return CELL_AUDIO_ERROR_PARAM;
		}

		audio_port& port = g_audio.ports[portNum];

		portConfig->readIndexAddr = port.index;

		switch (audio_port_state state = port.get_state())
		{
		case audio_port_state::closed:
			portConfig->status = CELL_AUDIO_STATUS_CLOSE;
			break;
		case audio_port_state::opened:
			portConfig->status = CELL_AUDIO_STATUS_READY;
			break;
		case audio_port_state::started:
			portConfig->status = CELL_AUDIO_STATUS_RUN;
			break;
		default:
			cellAudio.error("Invalid port state (%d: %d)", portNum, static_cast<u32>(state));
			return CELL_AUDIO_ERROR_AUDIOSYSTEM;
		}

		portConfig->nChannel = port.num_channels;
		portConfig->nBlock   = port.num_blocks;
		portConfig->portSize = port.size;
		portConfig->portAddr = port.addr.addr();
		return CELL_OK;
	});
}

error_code cellAudioPortStart(u32 portNum)
{
	cellAudio.warning("cellAudioPortStart(portNum=%d)", portNum);

	auto& g_audio = g_fxo->get<cell_audio>();

	if (!retro_atomic_load_acquire_int(&g_audio.init))
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	if (portNum >= AUDIO_PORT_COUNT)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	// Waiting for VSH
	lv2_sleep(30);

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		auto& port = g_audio.ports[portNum];

		switch (audio_port_state state = port.get_state())
		{
		case audio_port_state::closed: return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
		case audio_port_state::started: return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;
		case audio_port_state::opened:
			std::memset(port.addr.get_ptr(), 0, port.size);
			port.set_state(audio_port_state::started);
			return CELL_OK;
		default: fmt::throw_exception("Invalid port state (%d: %d)", portNum, static_cast<u32>(state));
		}
	});
}

error_code cellAudioPortClose(ppu_thread& ppu, u32 portNum)
{
	cellAudio.warning("cellAudioPortClose(portNum=%d)", portNum);

	const std::unique_lock savestate_lock{ g_fxo->get<hle_locks_t>(), std::try_to_lock };

	if (!savestate_lock)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	auto& g_audio = g_fxo->get<cell_audio>();

	cell_audio_thread::port_unmap gone;

	const error_code result = audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		if (portNum >= AUDIO_PORT_COUNT)
		{
			return CELL_AUDIO_ERROR_PARAM;
		}

		auto& port = g_audio.ports[portNum];

		if (port.get_state() == audio_port_state::closed)
		{
			return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
		}

		gone = g_audio.take_port(port);
		return CELL_OK;
	});

	if (result)
	{
		return result;
	}

	// Unmapped here, the port closed on the cellAudio thread first
	cell_audio_thread::unmap_port(ppu, gone);
	return CELL_OK;
}

error_code cellAudioPortStop(ppu_thread& ppu, u32 portNum)
{
	ppu.state += cpu_flag::wait;

	cellAudio.warning("cellAudioPortStop(portNum=%d)", portNum);

	auto& g_audio = g_fxo->get<cell_audio>();

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		if (portNum >= AUDIO_PORT_COUNT)
		{
			return CELL_AUDIO_ERROR_PARAM;
		}

		switch (audio_port_state state = g_audio.ports[portNum].cas_state(audio_port_state::started, audio_port_state::opened))
		{
		case audio_port_state::closed: return CELL_AUDIO_ERROR_PORT_NOT_RUN;
		case audio_port_state::started: return CELL_OK;
		case audio_port_state::opened: return CELL_AUDIO_ERROR_PORT_NOT_RUN;
		default: fmt::throw_exception("Invalid port state (%d: %d)", portNum, static_cast<u32>(state));
		}
	});
}

error_code cellAudioGetPortTimestamp(u32 portNum, u64 tag, vm::ptr<u64> stamp)
{
	cellAudio.trace("cellAudioGetPortTimestamp(portNum=%d, tag=0x%llx, stamp=*0x%x)", portNum, tag, stamp);

	auto& g_audio = g_fxo->get<cell_audio>();

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		if (portNum >= AUDIO_PORT_COUNT)
		{
			return CELL_AUDIO_ERROR_PARAM;
		}

		audio_port& port = g_audio.ports[portNum];

		if (port.get_state() == audio_port_state::closed)
		{
			return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
		}

		if (port.global_counter < tag)
		{
			return CELL_AUDIO_ERROR_TAG_NOT_FOUND;
		}

		const u64 delta_tag = port.global_counter - tag;
		const u64 delta_tag_stamp = delta_tag * g_audio.cfg.audio_block_period;

		// Apparently no error is returned if stamp is null
		*stamp = get_guest_system_time_of_slaved(port.timestamp - delta_tag_stamp);

		return CELL_OK;
	});
}

error_code cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, vm::ptr<u64> tag)
{
	cellAudio.trace("cellAudioGetPortBlockTag(portNum=%d, blockNo=0x%llx, tag=*0x%x)", portNum, blockNo, tag);

	auto& g_audio = g_fxo->get<cell_audio>();

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		if (portNum >= AUDIO_PORT_COUNT)
		{
			return CELL_AUDIO_ERROR_PARAM;
		}

		audio_port& port = g_audio.ports[portNum];

		if (port.get_state() == audio_port_state::closed)
		{
			return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
		}

		if (blockNo >= port.num_blocks)
		{
			return CELL_AUDIO_ERROR_PARAM;
		}

		// Apparently no error is returned if tag is null
		*tag = port.global_counter + blockNo - port.cur_pos;

		return CELL_OK;
	});
}

error_code cellAudioSetPortLevel(ppu_thread& ppu, u32 portNum, float level)
{
	ppu.state += cpu_flag::wait;

	cellAudio.trace("cellAudioSetPortLevel(portNum=%d, level=%f)", portNum, level);

	auto& g_audio = g_fxo->get<cell_audio>();

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		if (portNum >= AUDIO_PORT_COUNT)
		{
			return CELL_AUDIO_ERROR_PARAM;
		}

		audio_port& port = g_audio.ports[portNum];

		if (port.get_state() == audio_port_state::closed)
		{
			return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
		}

		if (level >= 0.0f)
		{
			port.set_level_set({ level, (port.level - level) / 624.0f });
		}
		else
		{
			cellAudio.todo("cellAudioSetPortLevel(%d): negative level value (%f)", portNum, level);
		}

		return CELL_OK;
	});
}

static error_code AudioCreateNotifyEventQueue(ppu_thread& ppu, vm::ptr<u32> id, vm::ptr<u64> key, u32 queue_type)
{
	vm::var<sys_event_queue_attribute_t> attr;
	attr->protocol = SYS_SYNC_FIFO;
	attr->type     = queue_type;
	attr->name_u64 = 0;

	for (u64 i = 0; i < MAX_AUDIO_EVENT_QUEUES; i++)
	{
		// Create an event queue "bruteforcing" an available key
		const u64 key_value = 0x80004d494f323221ull + i;

		// This originally reads from a global sdk value set by cellAudioInit
		// So check initialization as well
		const u32 queue_depth = retro_atomic_load_acquire_int(&g_fxo->get<cell_audio>().init) && g_ps3_process_info.sdk_ver <= 0x35FFFF ? 2 : 8;

		if (CellError res{sys_event_queue_create(ppu, id, attr, key_value, queue_depth) + 0u})
		{
			if (res != CELL_EEXIST)
			{
				return res;
			}
		}
		else
		{
			*key = key_value;
			return CELL_OK;
		}
	}

	return CELL_AUDIO_ERROR_EVENT_QUEUE;
}

error_code cellAudioCreateNotifyEventQueue(ppu_thread& ppu, vm::ptr<u32> id, vm::ptr<u64> key)
{
	cellAudio.warning("cellAudioCreateNotifyEventQueue(id=*0x%x, key=*0x%x)", id, key);

	return AudioCreateNotifyEventQueue(ppu, id, key, SYS_PPU_QUEUE);
}

error_code cellAudioCreateNotifyEventQueueEx(ppu_thread& ppu, vm::ptr<u32> id, vm::ptr<u64> key, u32 iFlags)
{
	cellAudio.warning("cellAudioCreateNotifyEventQueueEx(id=*0x%x, key=*0x%x, iFlags=0x%x)", id, key, iFlags);

	if (iFlags & ~CELL_AUDIO_CREATEEVENTFLAG_SPU)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	const u32 queue_type = (iFlags & CELL_AUDIO_CREATEEVENTFLAG_SPU) ? SYS_SPU_QUEUE : SYS_PPU_QUEUE;
	return AudioCreateNotifyEventQueue(ppu, id, key, queue_type);
}

error_code AudioSetNotifyEventQueue(ppu_thread& ppu, u64 key, u32 iFlags)
{
	auto& g_audio = g_fxo->get<cell_audio>();

	if (!retro_atomic_load_acquire_int(&g_audio.init))
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	// Waiting for VSH
	lv2_sleep(20, &ppu);

	// Dirty hack for sound: confirm the creation of _mxr000 event queue by _cellsurMixerMain thread
	constexpr u64 c_mxr000 = 0x8000cafe02460300;

	if (key == c_mxr000 || key == 0)
	{
		const bool has_sur_mixer_thread = idm::select<named_thread<ppu_thread>>([&](u32 id, named_thread<ppu_thread>& test_ppu)
		{
			// Confirm thread existence
			if (id == ppu.id)
			{
				return false;
			}

			const auto ptr = test_ppu.ppu_tname.load();

			if (!ptr)
			{
				return false;
			}

			return *ptr == "_cellsurMixerMain"sv;
		}).ret;

		bool was_mxr000_queue_found = false;

		for (usz count = 0; has_sur_mixer_thread && count < 100; count++)
		{
			if (lv2_event_queue::find(c_mxr000))
			{
				was_mxr000_queue_found = true;
				break;
			}

			if (ppu.is_stopped())
			{
				ppu.state += cpu_flag::again;
				return {};
			}

			(count < 3 ? cellAudio.warning : cellAudio.error)("AudioSetNotifyEventQueue(): Waiting for _mxr000. x%d", count);

			lv2_sleep(50'000, &ppu);
		}

		if (key == 0 && was_mxr000_queue_found)
		{
			// Correct key value argument
			key = c_mxr000;
		}
	}

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		auto q = lv2_event_queue::find(key);

		if (!q)
		{
			return CELL_AUDIO_ERROR_TRANS_EVENT;
		}

		for (auto i = g_audio.keys.cbegin(); i != g_audio.keys.cend();) // check for duplicates
		{
			if (i->port == q)
			{
				return CELL_AUDIO_ERROR_TRANS_EVENT;
			}

			if (!lv2_obj::check(i->port))
			{
				// Cleanup, avoid cases where there are multiple ports with the same key
				i = g_audio.keys.erase(i);
			}
			else
			{
				i++;
			}
		}

		// Set unique source associated with the key
		g_audio.keys.push_back
		({
			.start_period = g_audio.event_period,
			.flags = iFlags,
			.source = ((process_getpid() + u64{}) << 32) + lv2_event_port::id_base + (g_audio.key_count++ * lv2_event_port::id_step),
			.ack_timestamp = 0,
			.port = std::move(q)
		});

		g_audio.key_count %= lv2_event_port::id_count;

		return CELL_OK;
	});
}

error_code cellAudioSetNotifyEventQueue(ppu_thread& ppu, u64 key)
{
	ppu.state += cpu_flag::wait;

	cellAudio.warning("cellAudioSetNotifyEventQueue(key=0x%llx)", key);

	return AudioSetNotifyEventQueue(ppu, key, 0);
}

error_code cellAudioSetNotifyEventQueueEx(ppu_thread& ppu, u64 key, u32 iFlags)
{
	ppu.state += cpu_flag::wait;

	cellAudio.todo("cellAudioSetNotifyEventQueueEx(key=0x%llx, iFlags=0x%x)", key, iFlags);

	if (iFlags & (~0u >> 5))
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	return AudioSetNotifyEventQueue(ppu, key, iFlags);
}

error_code AudioRemoveNotifyEventQueue(u64 key, u32 iFlags)
{
	auto& g_audio = g_fxo->get<cell_audio>();

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		for (auto i = g_audio.keys.cbegin(); i != g_audio.keys.cend(); i++)
		{
			if (lv2_obj::check(i->port) && i->port->key == key)
			{
				if (i->flags != iFlags)
				{
					break;
				}

				g_audio.keys.erase(i);

				return CELL_OK;
			}
		}

		return CELL_AUDIO_ERROR_TRANS_EVENT;
	});
}

error_code cellAudioRemoveNotifyEventQueue(ppu_thread& ppu, u64 key)
{
	ppu.state += cpu_flag::wait;

	cellAudio.warning("cellAudioRemoveNotifyEventQueue(key=0x%llx)", key);

	return AudioRemoveNotifyEventQueue(key, 0);
}

error_code cellAudioRemoveNotifyEventQueueEx(ppu_thread& ppu, u64 key, u32 iFlags)
{
	ppu.state += cpu_flag::wait;

	cellAudio.todo("cellAudioRemoveNotifyEventQueueEx(key=0x%llx, iFlags=0x%x)", key, iFlags);

	if (iFlags & (~0u >> 5))
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	return AudioRemoveNotifyEventQueue(key, iFlags);
}

error_code cellAudioAddData(u32 portNum, vm::ptr<float> src, u32 samples, float volume)
{
	cellAudio.trace("cellAudioAddData(portNum=%d, src=*0x%x, samples=%d, volume=%f)", portNum, src, samples, volume);

	auto& g_audio = g_fxo->get<cell_audio>();

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		if (portNum >= AUDIO_PORT_COUNT || !src || !src.aligned() || samples != CELL_AUDIO_BLOCK_SAMPLES)
		{
			return CELL_AUDIO_ERROR_PARAM;
		}

		const audio_port& port = g_audio.ports[portNum];

		if (!port.num_channels)
		{
			return CELL_OK;
		}

		const auto dst = port.get_vm_ptr();

		volume = std::isfinite(volume) ? std::clamp(volume, -16.f, 16.f) : 0.f;

		for (u32 i = 0; i < samples * port.num_channels; i++)
		{
			dst[i] += src[i] * volume; // mix all channels
		}

		return CELL_OK;
	});
}

error_code cellAudioAdd2chData(u32 portNum, vm::ptr<float> src, u32 samples, float volume)
{
	cellAudio.trace("cellAudioAdd2chData(portNum=%d, src=*0x%x, samples=%d, volume=%f)", portNum, src, samples, volume);

	auto& g_audio = g_fxo->get<cell_audio>();

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		if (portNum >= AUDIO_PORT_COUNT || !src || !src.aligned() || samples != CELL_AUDIO_BLOCK_SAMPLES)
		{
			return CELL_AUDIO_ERROR_PARAM;
		}

		const audio_port& port = g_audio.ports[portNum];

		const auto dst = port.get_vm_ptr();

		volume = std::isfinite(volume) ? std::clamp(volume, -16.f, 16.f) : 0.f;

		if (port.num_channels == 2)
		{
			for (u32 i = 0; i < samples; i++)
			{
				dst[i * 2 + 0] += src[i * 2 + 0] * volume; // mix L ch
				dst[i * 2 + 1] += src[i * 2 + 1] * volume; // mix R ch
			}
		}
		else if (port.num_channels == 8)
		{
			for (u32 i = 0; i < samples; i++)
			{
				dst[i * 8 + 0] += src[i * 2 + 0] * volume; // mix L ch
				dst[i * 8 + 1] += src[i * 2 + 1] * volume; // mix R ch
				//dst[i * 8 + 2] += 0.0f; // center
				//dst[i * 8 + 3] += 0.0f; // LFE
				//dst[i * 8 + 4] += 0.0f; // rear L
				//dst[i * 8 + 5] += 0.0f; // rear R
				//dst[i * 8 + 6] += 0.0f; // side L
				//dst[i * 8 + 7] += 0.0f; // side R
			}
		}
		else
		{
			cellAudio.error("cellAudioAdd2chData(portNum=%d): invalid port.channel value (%d)", portNum, port.num_channels);
		}

		return CELL_OK;
	});
}

error_code cellAudioAdd6chData(u32 portNum, vm::ptr<float> src, float volume)
{
	cellAudio.trace("cellAudioAdd6chData(portNum=%d, src=*0x%x, volume=%f)", portNum, src, volume);

	auto& g_audio = g_fxo->get<cell_audio>();

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		if (portNum >= AUDIO_PORT_COUNT || !src || !src.aligned())
		{
			return CELL_AUDIO_ERROR_PARAM;
		}

		const audio_port& port = g_audio.ports[portNum];

		const auto dst = port.get_vm_ptr();

		volume = std::isfinite(volume) ? std::clamp(volume, -16.f, 16.f) : 0.f;

		if (port.num_channels == 8)
		{
			for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES; i++)
			{
				// Channel order in src is Front Left, Center, Front Right, Surround Left, Surround Right, LFE
				dst[i * 8 + 0] += src[i * 6 + 0] * volume; // mix L ch
				dst[i * 8 + 1] += src[i * 6 + 2] * volume; // mix R ch
				dst[i * 8 + 2] += src[i * 6 + 1] * volume; // mix center
				dst[i * 8 + 3] += src[i * 6 + 5] * volume; // mix LFE
				dst[i * 8 + 4] += src[i * 6 + 3] * volume; // mix rear L
				dst[i * 8 + 5] += src[i * 6 + 4] * volume; // mix rear R
				//dst[i * 8 + 6] += 0.0f; // side L
				//dst[i * 8 + 7] += 0.0f; // side R
			}
		}
		else
		{
			cellAudio.error("cellAudioAdd6chData(portNum=%d): invalid port.channel value (%d)", portNum, port.num_channels);
		}

		return CELL_OK;
	});
}

error_code cellAudioMiscSetAccessoryVolume(ppu_thread& ppu, u32 devNum, float volume)
{
	ppu.state += cpu_flag::wait;

	cellAudio.todo("cellAudioMiscSetAccessoryVolume(devNum=%d, volume=%f)", devNum, volume);

	auto& g_audio = g_fxo->get<cell_audio>();

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		// TODO

		return CELL_OK;
	});
}

error_code cellAudioSendAck(ppu_thread& ppu, u64 data3)
{
	ppu.state += cpu_flag::wait;

	cellAudio.trace("cellAudioSendAck(data3=0x%llx)", data3);

	auto& g_audio = g_fxo->get<cell_audio>();

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		// TODO: error checks

		for (cell_audio_thread::key_info& k : g_audio.keys)
		{
			if (k.source == data3)
			{
				k.ack_timestamp = audio_ringbuffer::get_timestamp();
				break;
			}
		}

		return CELL_OK;
	});
}

error_code cellAudioSetPersonalDevice(ppu_thread& ppu, s32 iPersonalStream, s32 iDevice)
{
	ppu.state += cpu_flag::wait;

	cellAudio.todo("cellAudioSetPersonalDevice(iPersonalStream=%d, iDevice=%d)", iPersonalStream, iDevice);

	auto& g_audio = g_fxo->get<cell_audio>();

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		if (iPersonalStream < 0 || iPersonalStream >= 4 ||
			(iDevice != CELL_AUDIO_PERSONAL_DEVICE_PRIMARY && (iDevice < 0 || iDevice >= 5)))
		{
			return CELL_AUDIO_ERROR_PARAM;
		}

		// TODO

		return CELL_OK;
	});
}

error_code cellAudioUnsetPersonalDevice(ppu_thread& ppu, s32 iPersonalStream)
{
	ppu.state += cpu_flag::wait;

	cellAudio.todo("cellAudioUnsetPersonalDevice(iPersonalStream=%d)", iPersonalStream);

	auto& g_audio = g_fxo->get<cell_audio>();

	return audio_call([&]() -> error_code
	{
		if (!retro_atomic_load_acquire_int(&g_audio.init))
		{
			return CELL_AUDIO_ERROR_NOT_INIT;
		}

		if (iPersonalStream < 0 || iPersonalStream >= 4)
		{
			return CELL_AUDIO_ERROR_PARAM;
		}

		// TODO

		return CELL_OK;
	});
}

DECLARE(ppu_module_manager::cellAudio)("cellAudio", []()
{
	REG_FUNC(cellAudio, cellAudioInit);
	REG_FUNC(cellAudio, cellAudioPortClose);
	REG_FUNC(cellAudio, cellAudioPortStop);
	REG_FUNC(cellAudio, cellAudioGetPortConfig);
	REG_FUNC(cellAudio, cellAudioPortStart);
	REG_FUNC(cellAudio, cellAudioQuit);
	REG_FUNC(cellAudio, cellAudioPortOpen);
	REG_FUNC(cellAudio, cellAudioSetPortLevel);
	REG_FUNC(cellAudio, cellAudioCreateNotifyEventQueue);
	REG_FUNC(cellAudio, cellAudioCreateNotifyEventQueueEx);
	REG_FUNC(cellAudio, cellAudioMiscSetAccessoryVolume);
	REG_FUNC(cellAudio, cellAudioSetNotifyEventQueue);
	REG_FUNC(cellAudio, cellAudioSetNotifyEventQueueEx);
	REG_FUNC(cellAudio, cellAudioGetPortTimestamp);
	REG_FUNC(cellAudio, cellAudioAdd2chData);
	REG_FUNC(cellAudio, cellAudioAdd6chData);
	REG_FUNC(cellAudio, cellAudioAddData);
	REG_FUNC(cellAudio, cellAudioGetPortBlockTag);
	REG_FUNC(cellAudio, cellAudioRemoveNotifyEventQueue);
	REG_FUNC(cellAudio, cellAudioRemoveNotifyEventQueueEx);
	REG_FUNC(cellAudio, cellAudioSendAck);
	REG_FUNC(cellAudio, cellAudioSetPersonalDevice);
	REG_FUNC(cellAudio, cellAudioUnsetPersonalDevice);
});
