#include "stdafx.h"

#include "libretro_audio.h"
#include "libretro_core.h"

#include <audio/conversion/float_to_s16.h>


LOG_CHANNEL(libretro_audio_log, "LibretroAudio");

static LibretroAudioBackend* s_audio_backend = nullptr;

// RPCS3 can output more than two channels (its Audio Channels setting, which
// the core used to pin to stereo). With a frontend that takes them, they go
// out as they are and the frontend fits them to the user's speakers, its
// Output Layout setting: discrete on a device that has them, folded to
// stereo otherwise.
static retro_audio_sample_multi_callback s_multi{};
static bool s_multi_available = false;

bool libretro_audio_negotiate_multi(retro_environment_t environ_cb)
{
    s_multi = {};
    s_multi_available = environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_BATCH_MULTI, &s_multi) && s_multi.batch_int16;
    lr_audio_out_set_multi(s_multi_available ? s_multi.batch_int16 : nullptr, s_multi_available ? s_multi.batch_float : nullptr);
    return s_multi_available;
}

bool libretro_audio_multi_available()
{
    return s_multi_available;
}

// The speakers of RPCS3's layouts, in the order RPCS3 interleaves them - the
// order cubeb's layouts give them in standalone, ascending speaker bits, which
// is the order the frontend takes. 0 for a layout that names none: the
// usual speakers for the channel count then (libretro_audio_out.c), as for a
// layout whose speakers are not as many as the channels - the frontend
// refuses a frame whose channel count and mask disagree, which was silence
// when a stereo game ran under a 5.1 layout (NNshi).
static unsigned speakers_of(audio_channel_layout layout)
{
    constexpr unsigned FL = RETRO_AUDIO_SPEAKER_FRONT_LEFT, FR = RETRO_AUDIO_SPEAKER_FRONT_RIGHT,
        FC = RETRO_AUDIO_SPEAKER_FRONT_CENTER, LFE = RETRO_AUDIO_SPEAKER_LOW_FREQUENCY,
        BL = RETRO_AUDIO_SPEAKER_BACK_LEFT, BR = RETRO_AUDIO_SPEAKER_BACK_RIGHT,
        SL = RETRO_AUDIO_SPEAKER_SIDE_LEFT, SR = RETRO_AUDIO_SPEAKER_SIDE_RIGHT;
    switch (layout)
    {
    case audio_channel_layout::mono: return FC;
    case audio_channel_layout::stereo_lfe: return FL | FR | LFE;
    case audio_channel_layout::quadraphonic: return FL | FR | BL | BR;
    case audio_channel_layout::quadraphonic_lfe: return FL | FR | LFE | BL | BR;
    case audio_channel_layout::surround_5_1: return FL | FR | FC | LFE | SL | SR;
    case audio_channel_layout::surround_7_1: return FL | FR | FC | LFE | BL | BR | SL | SR;
    default: return 0;
    }
}

LibretroAudioBackend* get_libretro_audio_backend()
{
    return s_audio_backend;
}

static size_t pull_samples(void* ctx, void* out, size_t max_frames, lr_audio_format* format)
{
    return static_cast<LibretroAudioBackend*>(ctx)->GetSamples(out, max_frames, format);
}

static size_t borrow_samples(void* ctx, const void** ptr, size_t frames, lr_audio_format* format)
{
    return static_cast<LibretroAudioBackend*>(ctx)->Borrow(ptr, frames, format);
}

static void return_samples(void* ctx, size_t frames)
{
    static_cast<LibretroAudioBackend*>(ctx)->Return(frames);
}

void libretro_audio_process(retro_audio_sample_batch_t audio_batch_cb, double frame_rate)
{
    // The pacing, the silence and the hand-over are libretro_audio_out.c's
    if (s_audio_backend)
        lr_audio_out_run_lent(audio_batch_cb, pull_samples, borrow_samples, return_samples, s_audio_backend, frame_rate);
}

// The backend's ring: as much as cellAudio asks for at its most - eight
// channels of floats at 48 kHz for its longest buffering (250 ms) and the
// 20 ms it adds - made once, with the backend
static constexpr usz s_ring_most = 8 * 48000 * sizeof(float) * 270 / 1000;

static constexpr int s_no_callback = 3;

static u64 format_word(u32 channels, bool is_float, u32 sample_bytes, unsigned speakers, u64 changes)
{
    return (channels & 0xf) | (is_float ? 0x10 : 0) | ((sample_bytes & 7) << 5) | (static_cast<u64>(speakers) << 8) | (changes << 40);
}

static u32 format_channels(u64 f) { return static_cast<u32>(f & 0xf); }
static bool format_float(u64 f) { return (f & 0x10) != 0; }
static u32 format_sample_bytes(u64 f) { return static_cast<u32>((f >> 5) & 7); }
static unsigned format_speakers(u64 f) { return static_cast<unsigned>((f >> 8) & 0xffffffffu); }

LibretroAudioBackend::LibretroAudioBackend()
{
    retro_atomic_int_init(&m_playing, 0);
    retro_atomic_int_init(&m_initialized, 0);
    retro_atomic_64_init(&m_format, 0);
    retro_atomic_int_init(&m_lending, 0);
    retro_atomic_int_init(&m_callback, s_no_callback);
    retro_atomic_int_init(&m_callback_in_use, s_no_callback);
    m_ring_made = lr_audio_ring_init(&m_ring, s_ring_most);
    convert_float_to_s16_init_simd();
    s_audio_backend = this;
}

LibretroAudioBackend::~LibretroAudioBackend()
{
    Close();
    if (s_audio_backend == this)
        s_audio_backend = nullptr;
    lr_audio_ring_free(&m_ring);
}

bool LibretroAudioBackend::Open(std::string_view dev_id, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout)
{
    (void)dev_id;

    // Set base class members - use what the config passes us
    // DO NOT force S16 here - let the config control this via raw.convert_to_s16
    // Floats go to the frontend as they are when it takes them, else as s16.
    // These are the opening thread's; retro_run goes by the format word
    m_sampling_rate = freq;
    m_sample_size = sample_size;  // Accept what config gives us (usually FLOAT)
    // As many channels as the game configured, up to 8 when the frontend
    // takes multi-channel output and 2 otherwise, as standalone's backends
    // fit the game's to the device
    setup_channel_layout(static_cast<u32>(ch_cnt), libretro_audio_multi_available() ? 8 : 2, layout, libretro_audio_log);

    const u64 changes = (static_cast<u64>(retro_atomic_load_acquire_64(&m_format)) >> 40) + 1;
    retro_atomic_store_release_64(&m_format, static_cast<s64>(format_word(get_channels(), m_sample_size == AudioSampleSize::FLOAT, get_sample_size(), speakers_of(get_channel_layout()), changes)));
    retro_atomic_store_release_int(&m_initialized, 1);
    return true;
}

void LibretroAudioBackend::Close()
{
    retro_atomic_store_release_int(&m_playing, 0);
    retro_atomic_store_release_int(&m_initialized, 0);
    const u64 changes = (static_cast<u64>(retro_atomic_load_acquire_64(&m_format)) >> 40) + 1;
    retro_atomic_store_release_64(&m_format, static_cast<s64>(changes << 40));
}

// The setting thread's. The slot set is neither the published one nor the
// one a pull is in: the pull says which it is in, then looks again at which
// is published, and takes it only if that has not changed; this looks at
// which it is in after the last publish (fences between, on both sides).
// So a pull never calls a callback being set, and nobody waits.
void LibretroAudioBackend::SetWriteCallback(audio_write_cb cb)
{
    const int published = retro_atomic_load_acquire_int(&m_callback);
    retro_atomic_thread_fence_seq_cst();
    const int in_use = retro_atomic_load_acquire_int(&m_callback_in_use);

    if (!cb)
    {
        retro_atomic_store_release_int(&m_callback, s_no_callback);
        return;
    }

    int slot = 0;
    while (slot == published || slot == in_use)
        slot++;
    m_callbacks[slot] = cb;
    retro_atomic_store_release_int(&m_callback, slot);
}

// cellAudio's thread: the backend's ring, empty, holding @bytes. The ring
// is never freed while the backend lives, so a pull still in it, or a loan
// not yet given back, is harmless: a flush makes the reader skip what was
// written before it, and the count of changes drops a pull across it
lr_audio_ring* LibretroAudioBackend::LendRing(usz bytes)
{
    if (!m_ring_made)
        return nullptr;
    lr_audio_ring_flush(&m_ring);
    lr_audio_ring_set_limit(&m_ring, bytes);
    retro_atomic_store_release_int(&m_lending, 1);
    return &m_ring;
}

void LibretroAudioBackend::EndLend()
{
    if (!m_ring_made)
        return;
    retro_atomic_store_release_int(&m_lending, 0);
    const u64 f = static_cast<u64>(retro_atomic_load_acquire_64(&m_format));
    retro_atomic_store_release_64(&m_format, static_cast<s64>((f & ((u64{1} << 40) - 1)) | (((f >> 40) + 1) << 40)));
    lr_audio_ring_flush(&m_ring);
}

void LibretroAudioBackend::SetStateCallback(audio_state_cb cb)
{
    // The setting thread's alone: nothing of retro_run's calls it
    m_state_callback = cb;
}

f64 LibretroAudioBackend::GetCallbackFrameLen()
{
    // How long cellAudio has to hold on to audio before it is collected - it
    // sizes its ring from this. Collection happens once per retro_run, not
    // every 5.3 ms like a sound card callback, and with buffering off the ring
    // was 5.3 ms + two 256-sample blocks = 16 ms: less than one 60 Hz frame.
    // Blocks that did not fit were dropped, RetroArch starved (NNshi: 98 %
    // underrun at 60 fps, fine from 66 fps up). Three frames leave room for a
    // late run; the ring is emptied every run, so this adds no latency.
    return 3.0 / 60.0;
}

void LibretroAudioBackend::Play()
{
    retro_atomic_store_release_int(&m_playing, 1);
}

void LibretroAudioBackend::Pause()
{
    retro_atomic_store_release_int(&m_playing, 0);
}

size_t LibretroAudioBackend::GetSamples(void* out, size_t max_frames, lr_audio_format* format)
{
    if (!IsPlaying() || !Initialized())
        return 0;

    const u64 f = static_cast<u64>(retro_atomic_load_acquire_64(&m_format));
    const u32 ch = format_channels(f);
    const size_t bytes_per_frame = static_cast<size_t>(ch) * format_sample_bytes(f);
    if (!ch || ch > LR_AUDIO_OUT_CHANNELS || !bytes_per_frame)
        return 0;

    size_t bytes = 0;
    if (retro_atomic_load_acquire_int(&m_lending))
    {
        // What is there of what is due, copied out: the ring could not lend
        // it (less than is due, or in two pieces)
        bytes = lr_audio_ring_read(&m_ring, out, max_frames * bytes_per_frame);
    }
    else
    {
        // The published callback, once it is marked as the one in use and
        // still published
        int slot;
        for (;;)
        {
            slot = retro_atomic_load_acquire_int(&m_callback);
            if (slot == s_no_callback)
                break;
            retro_atomic_store_relaxed_int(&m_callback_in_use, slot);
            retro_atomic_thread_fence_seq_cst();
            if (retro_atomic_load_acquire_int(&m_callback) == slot)
                break;
        }
        if (slot != s_no_callback)
            bytes = m_callbacks[slot](static_cast<u32>(max_frames * bytes_per_frame), out);
        retro_atomic_store_release_int(&m_callback_in_use, s_no_callback);
    }

    // Opened, closed or the lend ended meanwhile: not this format's
    if (static_cast<u64>(retro_atomic_load_acquire_64(&m_format)) != f)
        return 0;

    format->is_float = format_float(f);
    format->channels = ch;
    format->speakers = format_speakers(f);
    return bytes / bytes_per_frame;
}

size_t LibretroAudioBackend::Borrow(const void** ptr, size_t frames, lr_audio_format* format)
{
    if (!IsPlaying() || !Initialized() || !retro_atomic_load_acquire_int(&m_lending))
        return 0;

    const u64 f = static_cast<u64>(retro_atomic_load_acquire_64(&m_format));
    const u32 ch = format_channels(f);
    const size_t bytes_per_frame = static_cast<size_t>(ch) * format_sample_bytes(f);
    if (!ch || ch > LR_AUDIO_OUT_CHANNELS || !bytes_per_frame)
        return 0;

    if (lr_audio_ring_borrow(&m_ring, ptr) < frames * bytes_per_frame
        || static_cast<u64>(retro_atomic_load_acquire_64(&m_format)) != f)
        return 0;

    format->is_float = format_float(f);
    format->channels = ch;
    format->speakers = format_speakers(f);
    m_lent_frame_bytes = bytes_per_frame;
    return frames;
}

void LibretroAudioBackend::Return(size_t frames)
{
    lr_audio_ring_consume(&m_ring, frames * m_lent_frame_bytes);
}
