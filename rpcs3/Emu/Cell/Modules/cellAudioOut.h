#pragma once

#include "Emu/Audio/AudioBackend.h"
#include "Emu/Memory/vm_ptr.h"

#include <cstring>
#include <retro_atomic.h>

// Error codes
enum CellAudioOutError : u32
{
	CELL_AUDIO_OUT_ERROR_NOT_IMPLEMENTED        = 0x8002b240,
	CELL_AUDIO_OUT_ERROR_ILLEGAL_CONFIGURATION  = 0x8002b241,
	CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER      = 0x8002b242,
	CELL_AUDIO_OUT_ERROR_PARAMETER_OUT_OF_RANGE = 0x8002b243,
	CELL_AUDIO_OUT_ERROR_DEVICE_NOT_FOUND       = 0x8002b244,
	CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT  = 0x8002b245,
	CELL_AUDIO_OUT_ERROR_UNSUPPORTED_SOUND_MODE = 0x8002b246,
	CELL_AUDIO_OUT_ERROR_CONDITION_BUSY         = 0x8002b247,
};

enum CellAudioOut
{
	CELL_AUDIO_OUT_PRIMARY   = 0,
	CELL_AUDIO_OUT_SECONDARY = 1,
};

enum CellAudioOutDownMixer
{
	CELL_AUDIO_OUT_DOWNMIXER_NONE   = 0,
	CELL_AUDIO_OUT_DOWNMIXER_TYPE_A = 1,
	CELL_AUDIO_OUT_DOWNMIXER_TYPE_B = 2,
};

enum CellAudioOutDeviceMode
{
	CELL_AUDIO_OUT_SINGLE_DEVICE_MODE  = 0,
	CELL_AUDIO_OUT_MULTI_DEVICE_MODE   = 1,
	CELL_AUDIO_OUT_MULTI_DEVICE_MODE_2 = 2,
};

enum CellAudioOutPortType
{
	CELL_AUDIO_OUT_PORT_HDMI      = 0,
	CELL_AUDIO_OUT_PORT_SPDIF     = 1,
	CELL_AUDIO_OUT_PORT_ANALOG    = 2,
	CELL_AUDIO_OUT_PORT_USB       = 3,
	CELL_AUDIO_OUT_PORT_BLUETOOTH = 4,
	CELL_AUDIO_OUT_PORT_NETWORK   = 5,
};

enum CellAudioOutDeviceState
{
	CELL_AUDIO_OUT_DEVICE_STATE_UNAVAILABLE = 0,
	CELL_AUDIO_OUT_DEVICE_STATE_AVAILABLE   = 1,
};

enum CellAudioOutOutputState
{
	CELL_AUDIO_OUT_OUTPUT_STATE_ENABLED   = 0,
	CELL_AUDIO_OUT_OUTPUT_STATE_DISABLED  = 1,
	CELL_AUDIO_OUT_OUTPUT_STATE_PREPARING = 2,
};

enum CellAudioOutCodingType
{
	CELL_AUDIO_OUT_CODING_TYPE_LPCM               = 0,
	CELL_AUDIO_OUT_CODING_TYPE_AC3                = 1,
	CELL_AUDIO_OUT_CODING_TYPE_MPEG1              = 2,
	CELL_AUDIO_OUT_CODING_TYPE_MP3                = 3,
	CELL_AUDIO_OUT_CODING_TYPE_MPEG2              = 4,
	CELL_AUDIO_OUT_CODING_TYPE_AAC                = 5,
	CELL_AUDIO_OUT_CODING_TYPE_DTS                = 6,
	CELL_AUDIO_OUT_CODING_TYPE_ATRAC              = 7,
	CELL_AUDIO_OUT_CODING_TYPE_DOLBY_TRUE_HD      = 8, // Speculative name
	CELL_AUDIO_OUT_CODING_TYPE_DOLBY_DIGITAL_PLUS = 9,
	CELL_AUDIO_OUT_CODING_TYPE_DTS_HD_HIGHRES     = 10, // Speculative name
	CELL_AUDIO_OUT_CODING_TYPE_DTS_HD_MASTER      = 11, // Speculative name
	CELL_AUDIO_OUT_CODING_TYPE_BITSTREAM          = 0xff,
};

enum CellAudioOutChnum
{
	CELL_AUDIO_OUT_CHNUM_2 = 2,
	CELL_AUDIO_OUT_CHNUM_4 = 4,
	CELL_AUDIO_OUT_CHNUM_6 = 6,
	CELL_AUDIO_OUT_CHNUM_8 = 8,
};

enum CellAudioOutFs
{
	CELL_AUDIO_OUT_FS_32KHZ  = 0x01,
	CELL_AUDIO_OUT_FS_44KHZ  = 0x02,
	CELL_AUDIO_OUT_FS_48KHZ  = 0x04,
	CELL_AUDIO_OUT_FS_88KHZ  = 0x08,
	CELL_AUDIO_OUT_FS_96KHZ  = 0x10,
	CELL_AUDIO_OUT_FS_176KHZ = 0x20,
	CELL_AUDIO_OUT_FS_192KHZ = 0x40,
};

enum CellAudioOutSpeakerLayout
{
	CELL_AUDIO_OUT_SPEAKER_LAYOUT_DEFAULT      = 0x00000000,
	CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH          = 0x00000001,
	CELL_AUDIO_OUT_SPEAKER_LAYOUT_6CH_LREClr   = 0x00010000,
	CELL_AUDIO_OUT_SPEAKER_LAYOUT_8CH_LREClrxy = 0x40000000,
};


enum CellAudioOutEvent
{
	CELL_AUDIO_OUT_EVENT_DEVICE_CHANGED       = 0,
	CELL_AUDIO_OUT_EVENT_OUTPUT_DISABLED      = 1,
	CELL_AUDIO_OUT_EVENT_DEVICE_AUTHENTICATED = 2,
	CELL_AUDIO_OUT_EVENT_OUTPUT_ENABLED       = 3,
};

enum CellAudioOutCopyControl
{
	CELL_AUDIO_OUT_COPY_CONTROL_COPY_FREE  = 0,
	CELL_AUDIO_OUT_COPY_CONTROL_COPY_ONCE  = 1,
	CELL_AUDIO_OUT_COPY_CONTROL_COPY_NEVER = 2,
};


struct CellAudioOutConfiguration
{
	u8 channel;
	u8 encoder;
	u8 reserved[10];
	be_t<u32> downMixer;
};

struct CellAudioOutSoundMode
{
	u8 type;
	u8 channel;
	u8 fs;
	u8 reserved;
	be_t<u32> layout;

	ENABLE_BITWISE_SERIALIZATION;
};

struct CellAudioOutDeviceInfo
{
	u8 portType;
	u8 availableModeCount;
	u8 state;
	u8 reserved[3];
	be_t<u16> latency;
	CellAudioOutSoundMode availableModes[16];
};

struct CellAudioOutState
{
	u8 state;
	u8 encoder;
	u8 reserved[6];
	be_t<u32> downMixer;
	CellAudioOutSoundMode soundMode;
};

struct CellAudioOutSoundMode2
{
	u8 type;
	u8 channel;
	be_t<u16> fs;
	u8 reserved[4];
};

struct CellAudioOutDeviceInfo2
{
	u8 portType;
	u8 availableModeCount;
	u8 state;
	u8 deviceNumber;
	u8 reserved[12];
	be_t<u64> deviceId;
	be_t<u64> type;
	char name[64];
	CellAudioOutSoundMode2 availableModes2[16];
};

struct CellAudioOutOption
{
	be_t<u32> reserved;
};

struct CellAudioOutRegistrationOption
{
	be_t<u32> reserved;
};

struct CellAudioOutDeviceConfiguration
{
	u8 volume;
	u8 reserved[31];
};

typedef s32(CellAudioOutCallback)(u32 slot, u32 audioOut, u32 deviceIndex, u32 event, vm::ptr<CellAudioOutDeviceInfo> info, vm::ptr<void> userData);


// FXO Object

struct audio_out_configuration
{
	// What can change of an output's configuration, all of it read and
	// changed in one go (audio_out::settings_word): no lock
	struct audio_out_settings
	{
		u32 state = CELL_AUDIO_OUT_OUTPUT_STATE_ENABLED;
		u32 channels = CELL_AUDIO_OUT_CHNUM_2;
		u32 encoder = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
		u32 downmixer = CELL_AUDIO_OUT_DOWNMIXER_NONE;
		u32 copy_control = CELL_AUDIO_OUT_COPY_CONTROL_COPY_FREE;
		CellAudioOutSoundMode sound_mode{};

		std::pair<AudioChannelCnt, AudioChannelCnt> get_channel_count_and_downmixer() const;
	};

	struct audio_out
	{
		// Made with the configuration (and loaded with a savestate), before
		// any thread reads it; never changed after
		std::vector<CellAudioOutSoundMode> sound_modes;

		// The settings, one 128-bit word: the sound mode's eight bytes, then
		// state, channels, encoder, downmixer and copy control a byte each
		retro_atomic_128_t settings_word{};

		static retro_u128_t pack(const audio_out_settings& s)
		{
			retro_u128_t w{};
			std::memcpy(&w.lo, &s.sound_mode, sizeof(s.sound_mode));
			w.hi = u64{s.state & 0xff} | u64{s.channels & 0xff} << 8 | u64{s.encoder & 0xff} << 16 | u64{s.downmixer & 0xff} << 24 | u64{s.copy_control & 0xff} << 32;
			return w;
		}

		static audio_out_settings unpack(retro_u128_t w)
		{
			audio_out_settings s;
			std::memcpy(&s.sound_mode, &w.lo, sizeof(s.sound_mode));
			s.state = static_cast<u32>(w.hi & 0xff);
			s.channels = static_cast<u32>(w.hi >> 8 & 0xff);
			s.encoder = static_cast<u32>(w.hi >> 16 & 0xff);
			s.downmixer = static_cast<u32>(w.hi >> 24 & 0xff);
			s.copy_control = static_cast<u32>(w.hi >> 32 & 0xff);
			return s;
		}

		audio_out_settings settings()
		{
			return unpack(retro_atomic_load_acquire_128(&settings_word));
		}

		void set_settings(const audio_out_settings& s)
		{
			retro_atomic_store_release_128(&settings_word, pack(s));
		}

		// @f changes the settings it is given; true when it changed them.
		// Done again on what another thread changed meanwhile
		template <typename F>
		bool change_settings(F&& f)
		{
			retro_u128_t seen = retro_atomic_load_acquire_128(&settings_word);
			for (;;)
			{
				audio_out_settings s = unpack(seen);
				if (!f(s))
					return false;
				if (retro_atomic_cas_seen_128(&settings_word, &seen, pack(s)))
					return true;
			}
		}
	};

	std::array<audio_out, 2> out;

	SAVESTATE_INIT_POS(8.9); // Is a dependency of cellAudio
	audio_out_configuration();
	audio_out_configuration(utils::serial& ar);
	audio_out_configuration(const audio_out_configuration&) = delete;
	audio_out_configuration& operator=(const audio_out_configuration&) = delete;
	void save(utils::serial& ar);
};
