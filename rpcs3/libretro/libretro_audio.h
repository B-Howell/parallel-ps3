#pragma once

#include "Emu/Audio/AudioBackend.h"
#include "libretro.h"
#include "libretro_audio_out.h"
#include "libretro_audio_ring.h"

#include <retro_atomic.h>


// Asks the frontend for multi-channel output; true when it takes it, and
// from then on all audio goes through it (the frontend's rule: one or the
// other for the loaded game). Call from retro_load_game.
bool libretro_audio_negotiate_multi(retro_environment_t environ_cb);
bool libretro_audio_multi_available();

// retro_run: one frame's worth of the game's audio to the frontend - the
// sample rate over @frame_rate, the frames a second the core declares
void libretro_audio_process(retro_audio_sample_batch_t audio_batch_cb, double frame_rate);

// LibretroAudioBackend - AudioBackend implementation for libretro. The
// backend has a ring of its own, made with it and lent to cellAudio
// (LendRing), which writes the game's audio into it; retro_run hands the
// frontend the samples where they lie, in the ring's own memory. A source with only a write callback (the RSX audio path) is
// pulled from into the buffer retro_run hands the frontend.
class LibretroAudioBackend : public AudioBackend
{
private:
    retro_atomic_int_t m_playing;
    retro_atomic_int_t m_initialized;

    // Nothing here is locked, and neither side ever waits for the other.
    //
    // The stream's format, one word, from Open and Close for retro_run:
    // channels (bits 0-3, 0 while closed), float (bit 4), bytes a sample
    // (5-7), the frontend's speakers (8-39), and a count of changes (40
    // up). retro_run reads it before and after each pull, and drops what it
    // pulled across a change
    retro_atomic_64_t m_format;

    // The backend's ring, made with it and freed with it, so it is never
    // taken from under a pull; lent to cellAudio while m_lending is set
    lr_audio_ring m_ring{};
    bool m_ring_made = false;
    retro_atomic_int_t m_lending;

    // The write callbacks, in three slots: the one published (m_callback,
    // 3 for none), the one a pull of retro_run's is in (m_callback_in_use,
    // 3 for none) and one to set the next in - never one of the other two
    std::function<u32(u32, void*)> m_callbacks[3];
    retro_atomic_int_t m_callback;
    retro_atomic_int_t m_callback_in_use;

    // The bytes of a frame of what retro_run has on loan from the ring
    size_t m_lent_frame_bytes = 0;

    // Note: We use the base class m_sampling_rate, m_sample_size, m_channels, m_layout
    // DO NOT shadow them here or audio format detection will break!

public:
    LibretroAudioBackend();
    ~LibretroAudioBackend() override;

    std::string_view GetName() const override { return "Libretro"; }

    bool Open(std::string_view dev_id, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout) override;
    void Close() override;

    void SetWriteCallback(std::function<u32(u32, void*)> cb) override;
    void SetStateCallback(std::function<void(AudioStateEvent)> cb) override;
    lr_audio_ring* LendRing(usz bytes) override;
    void EndLend() override;

    f64 GetCallbackFrameLen() override;

    bool IsPlaying() override { return retro_atomic_load_acquire_int(&m_playing) != 0; }

    void Play() override;
    void Pause() override;

    bool Initialized() override { return retro_atomic_load_acquire_int(&m_initialized) != 0; }

    // retro_run's: up to @max_frames frames of the game's audio into @out
    // (room for max_frames of eight floats), in the backend's format, which
    // @format is told; the frames given, 0 when there is nothing to give
    // this run
    size_t GetSamples(void* out, size_t max_frames, lr_audio_format* format);

    // retro_run's: exactly @frames frames of the ring where they lie, at
    // *@ptr, until Return - or 0 when the ring cannot lend that many in one
    // piece (GetSamples then). cellAudio's thread can open, close and end
    // the lend meanwhile: the ring stays where it is.
    size_t Borrow(const void** ptr, size_t frames, lr_audio_format* format);
    void Return(size_t frames);
};

// Global audio backend instance access
LibretroAudioBackend* get_libretro_audio_backend();
