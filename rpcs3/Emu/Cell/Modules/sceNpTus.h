#pragma once

#include "cellRtc.h"
#include "sceNp.h"


// Constants for TUS functions and structures
enum
{
	SCE_NP_TUS_DATA_INFO_MAX_SIZE = 384,
	SCE_NP_TUS_MAX_CTX_NUM = 32,
	SCE_NP_TUS_MAX_SLOT_NUM_PER_TRANS = 64,
	SCE_NP_TUS_MAX_USER_NUM_PER_TRANS = 101,
	SCE_NP_TUS_MAX_SELECTED_FRIENDS_NUM = 100,
};

enum
{
	SCE_NP_TUS_OPETYPE_EQUAL = 1,
	SCE_NP_TUS_OPETYPE_NOT_EQUAL,
	SCE_NP_TUS_OPETYPE_GREATER_THAN,
	SCE_NP_TUS_OPETYPE_GREATER_OR_EQUAL,
	SCE_NP_TUS_OPETYPE_LESS_THAN,
	SCE_NP_TUS_OPETYPE_LESS_OR_EQUAL
};

enum
{
	SCE_NP_TUS_VARIABLE_SORTTYPE_DESCENDING_DATE = 1,
	SCE_NP_TUS_VARIABLE_SORTTYPE_ASCENDING_DATE,
	SCE_NP_TUS_VARIABLE_SORTTYPE_DESCENDING_VALUE,
	SCE_NP_TUS_VARIABLE_SORTTYPE_ASCENDING_VALUE
};

enum
{
	SCE_NP_TUS_DATASTATUS_SORTTYPE_DESCENDING_DATE = 1,
	SCE_NP_TUS_DATASTATUS_SORTTYPE_ASCENDING_DATE
};

enum SceNpTssStatusCodeType
{
	SCE_NP_TSS_STATUS_TYPE_OK,
	SCE_NP_TSS_STATUS_TYPE_PARTIAL,
	SCE_NP_TSS_STATUS_TYPE_NOT_MODIFIED
};

enum SceNpTssIfType
{
	SCE_NP_TSS_IFTYPE_IF_MODIFIED_SINCE,
	SCE_NP_TSS_IFTYPE_IF_RANGE
};

using SceNpTssSlotId = s32;
using SceNpTusSlotId = s32;
using SceNpTusVirtualUserId = SceNpOnlineId;

// Structure for representing a TUS variable
struct SceNpTusVariable
{
	SceNpId ownerId;
	be_t<s32> hasData;
	CellRtcTick lastChangedDate;
	u8 pad[4];
	SceNpId lastChangedAuthorId;
	be_t<s64> variable;
	be_t<s64> oldVariable;
	u8 reserved[16];
};

// Structure for representing the accessory information of a TUS data
struct SceNpTusDataInfo
{
	be_t<u32> infoSize;
	u8 pad[4];
	u8 data[SCE_NP_TUS_DATA_INFO_MAX_SIZE];
};

// Structure for respreseting the status of TUS data
struct SceNpTusDataStatus
{
	SceNpId ownerId;
	be_t<s32> hasData;
	CellRtcTick lastChangedDate;
	SceNpId lastChangedAuthorId;
	vm::bptr<void> data;
	be_t<u32> dataSize;
	u8 pad[4];
	SceNpTusDataInfo info;
};

struct SceNpTusAddAndGetVariableOptParam
{
	be_t<u32> size;
	vm::bptr<CellRtcTick> isLastChangedDate;
	vm::bptr<SceNpId> isLastChangedAuthorId;
};

struct SceNpTusTryAndSetVariableOptParam
{
	be_t<u32> size;
	vm::bptr<CellRtcTick> isLastChangedDate;
	vm::bptr<SceNpId> isLastChangedAuthorId;
	vm::bptr<s64> compareValue;
};

struct SceNpTusSetDataOptParam
{
	be_t<u32> size;
	vm::bptr<CellRtcTick> isLastChangedDate;
	vm::bptr<SceNpId> isLastChangedAuthorId;
};

struct SceNpTssDataStatus
{
	CellRtcTick lastModified;
	be_t<s32> statusCodeType;
	be_t<u32> contentLength;
};

struct SceNpTssIfModifiedSinceParam
{
	be_t<s32> ifType;
	u8 padding[4];
	CellRtcTick lastModified;
};

struct SceNpTssGetDataOptParam
{
	be_t<u32> size;
	vm::bptr<u64> offset;
	vm::bptr<u64> lastByte;
	vm::bptr<SceNpTssIfModifiedSinceParam> ifParam;
};
