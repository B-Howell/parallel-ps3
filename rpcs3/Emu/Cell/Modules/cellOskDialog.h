#pragma once

#include "util/types.hpp"
#include "util/atomic.hpp"
#include "util/init_mutex.hpp"
#include "osk_text.h"
#include "Emu/Memory/vm_ptr.h"
#include <string>
#include <functional>

// error codes
enum CellOskDialogError : u32
{
	CELL_OSKDIALOG_ERROR_IME_ALREADY_IN_USE = 0x8002b501,
	CELL_OSKDIALOG_ERROR_GET_SIZE_ERROR     = 0x8002b502,
	CELL_OSKDIALOG_ERROR_UNKNOWN            = 0x8002b503,
	CELL_OSKDIALOG_ERROR_PARAM              = 0x8002b504,
};

// OSK status for the callback
enum
{
	CELL_SYSUTIL_OSKDIALOG_LOADED               = 0x0502,
	CELL_SYSUTIL_OSKDIALOG_FINISHED             = 0x0503,
	CELL_SYSUTIL_OSKDIALOG_UNLOADED             = 0x0504,
	CELL_SYSUTIL_OSKDIALOG_INPUT_ENTERED        = 0x0505,
	CELL_SYSUTIL_OSKDIALOG_INPUT_CANCELED       = 0x0506,
	CELL_SYSUTIL_OSKDIALOG_INPUT_DEVICE_CHANGED = 0x0507,
	CELL_SYSUTIL_OSKDIALOG_DISPLAY_CHANGED      = 0x0508,
};

enum CellOskDialogInputFieldResult
{
	CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK = 0,
	CELL_OSKDIALOG_INPUT_FIELD_RESULT_CANCELED = 1,
	CELL_OSKDIALOG_INPUT_FIELD_RESULT_ABORT = 2,
	CELL_OSKDIALOG_INPUT_FIELD_RESULT_NO_INPUT_TEXT = 3,
};

enum CellOskDialogInitialKeyLayout
{
	CELL_OSKDIALOG_INITIAL_PANEL_LAYOUT_SYSTEM = 0,
	CELL_OSKDIALOG_INITIAL_PANEL_LAYOUT_10KEY = 1,
	CELL_OSKDIALOG_INITIAL_PANEL_LAYOUT_FULLKEY = 2,
};

enum CellOskDialogInputDevice
{
	CELL_OSKDIALOG_INPUT_DEVICE_PAD = 0,
	CELL_OSKDIALOG_INPUT_DEVICE_KEYBOARD = 1,
};

enum CellOskDialogContinuousMode
{
	CELL_OSKDIALOG_CONTINUOUS_MODE_NONE = 0,
	CELL_OSKDIALOG_CONTINUOUS_MODE_REMAIN_OPEN = 1,
	CELL_OSKDIALOG_CONTINUOUS_MODE_HIDE = 2,
	CELL_OSKDIALOG_CONTINUOUS_MODE_SHOW = 3,
};

enum CellOskDialogDisplayStatus
{
	CELL_OSKDIALOG_DISPLAY_STATUS_HIDE = 0,
	CELL_OSKDIALOG_DISPLAY_STATUS_SHOW = 1,
};

enum CellOskDialogFilterCallbackReturnValue
{
	CELL_OSKDIALOG_NOT_CHANGE = 0,
	CELL_OSKDIALOG_CHANGE_WORD = 1,
};

enum CellOskDialogActionValue
{
	CELL_OSKDIALOG_CHANGE_NO_EVENT = 0,
	CELL_OSKDIALOG_CHANGE_EVENT_CANCEL = 1,
	CELL_OSKDIALOG_CHANGE_WORDS_INPUT = 3,
	CELL_OSKDIALOG_CHANGE_WORDS_INSERT = 4,
	CELL_OSKDIALOG_CHANGE_WORDS_REPLACE_ALL = 6,
};

enum CellOskDialogFinishReason
{
	CELL_OSKDIALOG_CLOSE_CONFIRM = 0,
	CELL_OSKDIALOG_CLOSE_CANCEL = 1,
};

enum CellOskDialogFinishReasonFake // Helper. Must be negative values.
{
	FAKE_CELL_OSKDIALOG_CLOSE_ABORT = -1,
	FAKE_CELL_OSKDIALOG_CLOSE_TERMINATE = -2,
};

enum CellOskDialogType
{
	CELL_OSKDIALOG_TYPE_SINGLELINE_OSK = 0,
	CELL_OSKDIALOG_TYPE_MULTILINE_OSK = 1,
	CELL_OSKDIALOG_TYPE_FULL_KEYBOARD_SINGLELINE_OSK = 2,
	CELL_OSKDIALOG_TYPE_FULL_KEYBOARD_MULTILINE_OSK = 3,
	CELL_OSKDIALOG_TYPE_SEPARATE_SINGLELINE_TEXT_WINDOW = 4,
	CELL_OSKDIALOG_TYPE_SEPARATE_MULTILINE_TEXT_WINDOW = 5,
	CELL_OSKDIALOG_TYPE_SEPARATE_INPUT_PANEL_WINDOW = 6,
	CELL_OSKDIALOG_TYPE_SEPARATE_FULL_KEYBOARD_INPUT_PANEL_WINDOW = 7,
	CELL_OSKDIALOG_TYPE_SEPARATE_CANDIDATE_WINDOW = 8,
};

enum
{
	CELL_OSKDIALOG_STRING_SIZE = 512, // Theroretical maximum for osk input, games can specify a lower limit
};

enum
{
	CELL_OSKDIALOG_PANELMODE_DEFAULT             = 0x00000000,
	CELL_OSKDIALOG_PANELMODE_GERMAN              = 0x00000001,
	CELL_OSKDIALOG_PANELMODE_ENGLISH             = 0x00000002,
	CELL_OSKDIALOG_PANELMODE_SPANISH             = 0x00000004,
	CELL_OSKDIALOG_PANELMODE_FRENCH              = 0x00000008,
	CELL_OSKDIALOG_PANELMODE_ITALIAN             = 0x00000010,
	CELL_OSKDIALOG_PANELMODE_DUTCH               = 0x00000020,
	CELL_OSKDIALOG_PANELMODE_PORTUGUESE          = 0x00000040,
	CELL_OSKDIALOG_PANELMODE_RUSSIAN             = 0x00000080,
	CELL_OSKDIALOG_PANELMODE_JAPANESE            = 0x00000100,
	CELL_OSKDIALOG_PANELMODE_DEFAULT_NO_JAPANESE = 0x00000200,
	CELL_OSKDIALOG_PANELMODE_POLISH              = 0x00000400,
	CELL_OSKDIALOG_PANELMODE_KOREAN              = 0x00001000,
	CELL_OSKDIALOG_PANELMODE_TURKEY              = 0x00002000,
	CELL_OSKDIALOG_PANELMODE_TRADITIONAL_CHINESE = 0x00004000,
	CELL_OSKDIALOG_PANELMODE_SIMPLIFIED_CHINESE  = 0x00008000,
	CELL_OSKDIALOG_PANELMODE_PORTUGUESE_BRAZIL   = 0x00010000,
	CELL_OSKDIALOG_PANELMODE_DANISH              = 0x00020000,
	CELL_OSKDIALOG_PANELMODE_SWEDISH             = 0x00040000,
	CELL_OSKDIALOG_PANELMODE_NORWEGIAN           = 0x00080000,
	CELL_OSKDIALOG_PANELMODE_FINNISH             = 0x00100000,
	CELL_OSKDIALOG_PANELMODE_JAPANESE_HIRAGANA   = 0x00200000,
	CELL_OSKDIALOG_PANELMODE_JAPANESE_KATAKANA   = 0x00400000,
	CELL_OSKDIALOG_PANELMODE_ALPHABET_FULL_WIDTH = 0x00800000,
	CELL_OSKDIALOG_PANELMODE_ALPHABET            = 0x01000000,
	CELL_OSKDIALOG_PANELMODE_LATIN               = 0x02000000,
	CELL_OSKDIALOG_PANELMODE_NUMERAL_FULL_WIDTH  = 0x04000000,
	CELL_OSKDIALOG_PANELMODE_NUMERAL             = 0x08000000,
	CELL_OSKDIALOG_PANELMODE_URL                 = 0x10000000,
	CELL_OSKDIALOG_PANELMODE_PASSWORD            = 0x20000000
};

enum
{
	CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_LEFT   = 0x00000200,
	CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_CENTER = 0x00000400,
	CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_RIGHT  = 0x00000800,
	CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_TOP    = 0x00001000,
	CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_CENTER = 0x00002000,
	CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_BOTTOM = 0x00004000
};

enum
{
	CELL_OSKDIALOG_NO_SPACE          = 0x00000001,
	CELL_OSKDIALOG_NO_RETURN         = 0x00000002,
	CELL_OSKDIALOG_NO_INPUT_ANALOG   = 0x00000008,
	CELL_OSKDIALOG_NO_STARTUP_EFFECT = 0x00001000,
};

enum
{
	CELL_OSKDIALOG_10KEY_PANEL   = 0x00000001,
	CELL_OSKDIALOG_FULLKEY_PANEL = 0x00000002
};

enum
{
	CELL_OSKDIALOG_DEVICE_MASK_PAD = 0x000000ff
};

enum
{
	CELL_OSKDIALOG_EVENT_HOOK_TYPE_FUNCTION_KEY  = 0x00000001,
	CELL_OSKDIALOG_EVENT_HOOK_TYPE_ASCII_KEY     = 0x00000002,
	CELL_OSKDIALOG_EVENT_HOOK_TYPE_ONLY_MODIFIER = 0x00000004
};

static const f32 CELL_OSKDIALOG_SCALE_MIN = 0.80f;
static const f32 CELL_OSKDIALOG_SCALE_MAX = 1.05f;

struct CellOskDialogInputFieldInfo
{
	vm::bptr<u16> message;
	vm::bptr<u16> init_text;
	be_t<u32> limit_length;
};

struct CellOskDialogPoint
{
	be_t<f32> x;
	be_t<f32> y;
};

struct CellOskDialogParam
{
	be_t<u32> allowOskPanelFlg;
	be_t<u32> firstViewPanel;
	CellOskDialogPoint controlPoint;
	be_t<s32> prohibitFlgs;
};

// Actual input data
struct CellOskDialogCallbackReturnParam
{
	be_t<s32> result; // CellOskDialogInputFieldResult
	be_t<s32> numCharsResultString;
	vm::bptr<u16> pResultString;
};

struct CellOskDialogLayoutInfo
{
	be_t<s32> layoutMode;
	CellOskDialogPoint position;
};

struct CellOskDialogSeparateWindowOption
{
	be_t<u32> continuousMode; // CellOskDialogContinuousMode
	be_t<s32> deviceMask;
	be_t<s32> inputFieldWindowWidth;
	be_t<f32> inputFieldBackgroundTrans;
	vm::bptr<CellOskDialogLayoutInfo> inputFieldLayoutInfo;
	vm::bptr<CellOskDialogLayoutInfo> inputPanelLayoutInfo;
	vm::bptr<void> reserved;
};

struct CellOskDialogKeyMessage
{
	be_t<u32> led;
	be_t<u32> mkey;
	be_t<u16> keycode;
};

struct CellOskDialogImeDictionaryInfo
{
	be_t<u32> targetLanguage;
	vm::bcptr<char> dictionaryPath;
};

using cellOskDialogConfirmWordFilterCallback = int(vm::ptr<u16> pConfirmString, s32 wordLength);
using cellOskDialogHardwareKeyboardEventHookCallback = class b8(vm::ptr<CellOskDialogKeyMessage> keyMessage, vm::ptr<u32> action, vm::ptr<void> pActionInfo);
using cellOskDialogForceFinishCallback = class b8();

struct osk_window_layout
{
	u32 layout_mode = CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_LEFT | CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_TOP;
	u32 x_align = CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_LEFT;
	u32 y_align = CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_TOP;
	f32 x_offset = 0.0f;
	f32 y_offset = 0.0f;

	std::string to_string() const
	{
		return fmt::format("{ layout_mode=0x%x, x_align=0x%x, y_align=0x%x, x_offset=%.2f, y_offset=%.2f }", layout_mode, x_align, y_align, x_offset, y_offset);
	}
};

enum class OskDialogState
{
	Unloaded,
	Open,
	Abort,
	Closed
};

class OskDialogBase
{
public:
	struct color
	{
		f32 r = 1.0f;
		f32 g = 1.0f;
		f32 b = 1.0f;
		f32 a = 1.0f;
	};

	struct osk_params
	{
		std::string title;
		std::u16string message;
		const char16_t* init_text = nullptr;
		u32 charlimit = 0;
		u32 prohibit_flags = 0;
		u32 panel_flag = 0;
		u32 support_language = 0;
		u32 first_view_panel = 0;
		osk_window_layout layout{};
		osk_window_layout input_layout{}; // Only used with separate windows
		osk_window_layout panel_layout{}; // Only used with separate windows
		u32 input_field_window_width = 0; // Only used with separate windows
		f32 input_field_background_transparency = 1.0f; // Only used with separate windows
		f32 initial_scale = 1.0f;
		color base_color{};
		bool dimmer_enabled = false;
		bool use_separate_windows = false;
		bool intercept_input = false;
	};

	virtual void Create(const osk_params& params) = 0;
	virtual void Clear(bool clear_all_data) = 0;
	virtual void Insert(const std::u16string& text) = 0;
	virtual void SetText(const std::u16string& text) = 0;

	// Closes the dialog.
	// Set status to CELL_OSKDIALOG_CLOSE_CONFIRM or CELL_OSKDIALOG_CLOSE_CANCEL for user input.
	// Set status to -1 if closed by the game or system.
	// Set status to -2 if terminated by the system.
	virtual void Close(s32 status) = 0;
	OskDialogBase() { osk_text_init(&osk_text); }
	virtual ~OskDialogBase() {};

	std::function<void(s32 status)> on_osk_close;
	std::function<void(CellOskDialogKeyMessage key_message)> on_osk_key_input_entered;

	atomic_t<OskDialogState> state{ OskDialogState::Unloaded };
	atomic_t<CellOskDialogContinuousMode> continuous_mode{ CELL_OSKDIALOG_CONTINUOUS_MODE_NONE };
	atomic_t<CellOskDialogInputDevice> input_device{ CELL_OSKDIALOG_INPUT_DEVICE_PAD }; // The current input device.
	atomic_t<bool> pad_input_enabled{ true };      // Determines if the OSK consumes the device's input.
	atomic_t<bool> mouse_input_enabled{ true };    // Determines if the OSK consumes the device's input.
	atomic_t<bool> keyboard_input_enabled{ true }; // Determines if the OSK consumes the device's input.
	atomic_t<bool> ignore_device_events{ false };  // Determines if the OSK ignores device events.

	atomic_t<CellOskDialogInputFieldResult> osk_input_result{ CellOskDialogInputFieldResult::CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK };

	// The text typed so far: set whole by the dialog (or the game's checks),
	// read in place by the game's calls, with no lock (osk_text.c)
	osk_text_t osk_text;
};

struct osk_info
{
	std::shared_ptr<OskDialogBase> dlg;

	// The string that's going to be served to the game: each set published
	// whole, read in place with no lock (osk_text.c)
	osk_text_t valid_text;

	static_assert(OSK_TEXT_CHARS == CELL_OSKDIALOG_STRING_SIZE);

	osk_info() { osk_text_init(&valid_text); }
	osk_info(const osk_info&) = delete;
	osk_info& operator=(const osk_info&) = delete;

	// What the game set up (cellOskDialogSet*), read as the dialog loads and
	// by the overlay: bools are 0 or 1, f32s their bits, callbacks their
	// guest addresses
	retro_atomic_int_t use_separate_windows{0};

	retro_atomic_int_t lock_ext_input_device{0};
	retro_atomic_int_t device_mask{0}; // OSK ignores input from the specified devices. 0 means all devices can influence the OSK
	retro_atomic_int_t input_field_window_width{0};
	retro_atomic_int_t input_field_background_transparency{0x3f800000}; // 1.0f
	osk_window_layout input_field_layout_info{};
	osk_window_layout input_panel_layout_info{};
	retro_atomic_int_t key_layout_options{CELL_OSKDIALOG_10KEY_PANEL};
	retro_atomic_int_t initial_key_layout{CELL_OSKDIALOG_INITIAL_PANEL_LAYOUT_SYSTEM}; // TODO: use
	retro_atomic_int_t initial_input_device{CELL_OSKDIALOG_INPUT_DEVICE_PAD}; // OSK at first only receives input from the initial device

	retro_atomic_int_t clipboard_enabled{0}; // For copy and paste
	retro_atomic_int_t half_byte_kana_enabled{0};
	retro_atomic_int_t supported_languages{0}; // Used to enable non-default languages in the OSK

	retro_atomic_int_t dimmer_enabled{1};

	// The colour, two f32s' bits a word (red and green, blue and alpha)
	retro_atomic_64_t base_color_rg{pack_f32s(0.2f, 0.2f)};
	retro_atomic_64_t base_color_ba{pack_f32s(0.2f, 1.0f)};

	static constexpr s64 pack_f32s(f32 hi, f32 lo) { return static_cast<s64>((u64{std::bit_cast<u32>(hi)} << 32) | std::bit_cast<u32>(lo)); }
	static f32 f32_hi(s64 word) { return std::bit_cast<f32>(static_cast<u32>(static_cast<u64>(word) >> 32)); }
	static f32 f32_lo(s64 word) { return std::bit_cast<f32>(static_cast<u32>(static_cast<u64>(word))); }

	void set_base_color(const OskDialogBase::color& c)
	{
		retro_atomic_store_release_64(&base_color_rg, pack_f32s(c.r, c.g));
		retro_atomic_store_release_64(&base_color_ba, pack_f32s(c.b, c.a));
	}

	OskDialogBase::color get_base_color() const
	{
		const s64 rg = retro_atomic_load_acquire_64(&base_color_rg);
		const s64 ba = retro_atomic_load_acquire_64(&base_color_ba);
		return OskDialogBase::color{f32_hi(rg), f32_lo(rg), f32_hi(ba), f32_lo(ba)};
	}

	retro_atomic_int_t pointer_enabled{0};
	// The pointer's x and y, set and read as one (x's bits high, y's low)
	retro_atomic_64_t pointer_pos{0};
	retro_atomic_int_t initial_scale{0x3f800000}; // 1.0f

	osk_window_layout layout = {};

	retro_atomic_int_t osk_continuous_mode{CELL_OSKDIALOG_CONTINUOUS_MODE_NONE};
	retro_atomic_int_t last_dialog_state{CELL_SYSUTIL_OSKDIALOG_UNLOADED}; // Used for continuous seperate window dialog

	retro_atomic_int_t osk_confirm_callback{0};
	retro_atomic_int_t osk_force_finish_callback{0};
	retro_atomic_int_t osk_hardware_keyboard_event_hook_callback{0};
	retro_atomic_int_t hook_event_mode{0};

	stx::init_mutex init;

	void reset();

	// Align horizontally
	static u32 get_aligned_x(u32 layout_mode);

	// Align vertically
	static u32 get_aligned_y(u32 layout_mode);
};
