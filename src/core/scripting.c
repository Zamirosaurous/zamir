/* Copyright (c) 2013-2022 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/core/scripting.h>

#include <mgba/core/core.h>
#include <mgba/core/serialize.h>
#include <mgba/script/context.h>
#include <mgba-util/table.h>
#include <mgba-util/vfs.h>

mLOG_DEFINE_CATEGORY(SCRIPT, "Scripting", "script");

struct mScriptBridge {
	struct Table engines;
	struct mDebugger* debugger;
};

struct mScriptInfo {
	const char* name;
	struct VFile* vf;
	bool success;
};

struct mScriptSymbol {
	const char* name;
	int32_t* out;
	bool success;
};

static void _seDeinit(void* value) {
	struct mScriptEngine* se = value;
	se->deinit(se);
}

static void _seTryLoad(const char* key, void* value, void* user) {
	UNUSED(key);
	struct mScriptEngine* se = value;
	struct mScriptInfo* si = user;
	if (!si->success && se->isScript(se, si->name, si->vf)) {
		si->success = se->loadScript(se, si->name, si->vf);
	}
}

static void _seLookupSymbol(const char* key, void* value, void* user) {
	UNUSED(key);
	struct mScriptEngine* se = value;
	struct mScriptSymbol* si = user;
	if (!si->success) {
		si->success = se->lookupSymbol(se, si->name, si->out);
	}
}

static void _seRun(const char* key, void* value, void* user) {
	UNUSED(key);
	UNUSED(user);
	struct mScriptEngine* se = value;
	se->run(se);
}

#ifdef USE_DEBUGGERS
struct mScriptDebuggerEntry {
	enum mDebuggerEntryReason reason;
	struct mDebuggerEntryInfo* info;
};

static void _seDebuggerEnter(const char* key, void* value, void* user) {
	UNUSED(key);
	struct mScriptEngine* se = value;
	struct mScriptDebuggerEntry* entry = user;
	se->debuggerEntered(se, entry->reason, entry->info);
}
#endif

struct mScriptBridge* mScriptBridgeCreate(void) {
	struct mScriptBridge* sb = malloc(sizeof(*sb));
	HashTableInit(&sb->engines, 0, _seDeinit);
	sb->debugger = NULL;
	return sb;
}

void mScriptBridgeDestroy(struct mScriptBridge* sb) {
	HashTableDeinit(&sb->engines);
	free(sb);
}

void mScriptBridgeInstallEngine(struct mScriptBridge* sb, struct mScriptEngine* se) {
	if (!se->init(se, sb)) {
		return;
	}
	const char* name = se->name(se);
	HashTableInsert(&sb->engines, name, se);
}

#ifdef USE_DEBUGGERS
void mScriptBridgeSetDebugger(struct mScriptBridge* sb, struct mDebugger* debugger) {
	if (sb->debugger == debugger) {
		return;
	}
	if (sb->debugger) {
		sb->debugger->bridge = NULL;
	}
	sb->debugger = debugger;
	if (debugger) {
		debugger->bridge = sb;
	}
}

struct mDebugger* mScriptBridgeGetDebugger(struct mScriptBridge* sb) {
	return sb->debugger;
}

void mScriptBridgeDebuggerEntered(struct mScriptBridge* sb, enum mDebuggerEntryReason reason, struct mDebuggerEntryInfo* info) {
	struct mScriptDebuggerEntry entry = {
		.reason = reason,
		.info = info
	};
	HashTableEnumerate(&sb->engines, _seDebuggerEnter, &entry);
}
#endif

void mScriptBridgeRun(struct mScriptBridge* sb) {
	HashTableEnumerate(&sb->engines, _seRun, NULL);
}

bool mScriptBridgeLoadScript(struct mScriptBridge* sb, const char* name) {
	struct VFile* vf = VFileOpen(name, O_RDONLY);
	if (!vf) {
		return false;
	}
	struct mScriptInfo info = {
		.name = name,
		.vf = vf,
		.success = false
	};
	HashTableEnumerate(&sb->engines, _seTryLoad, &info);
	vf->close(vf);
	return info.success;
}

bool mScriptBridgeLookupSymbol(struct mScriptBridge* sb, const char* name, int32_t* out) {
	struct mScriptSymbol info = {
		.name = name,
		.out = out,
		.success = false
	};
	HashTableEnumerate(&sb->engines, _seLookupSymbol, &info);
	return info.success;
}

struct mScriptMemoryAdapter {
	struct mCore* core;
	struct mCoreMemoryBlock block;
};

struct mScriptCoreAdapter {
	struct mCore* core;
	struct mScriptContext* context;
	struct mScriptValue memory;
};

struct mScriptUILibrary {
	mScriptContextBufferFactory textBufferFactory;
	void* textBufferContext;
};

#define CALCULATE_SEGMENT_INFO \
	uint32_t segmentSize = adapter->block.end - adapter->block.start; \
	uint32_t segmentStart = adapter->block.segmentStart - adapter->block.start; \
	if (adapter->block.segmentStart) { \
		segmentSize -= segmentStart; \
	}

#define CALCULATE_SEGMENT_ADDRESS \
	uint32_t segmentAddress = address % segmentSize; \
	int segment = address / segmentSize; \
	segmentAddress += adapter->block.start; \
	if (adapter->block.segmentStart && segment) { \
		segmentAddress += segmentStart; \
	}

static uint32_t mScriptMemoryAdapterRead8(struct mScriptMemoryAdapter* adapter, uint32_t address) {
	CALCULATE_SEGMENT_INFO;
	CALCULATE_SEGMENT_ADDRESS;
	return adapter->core->rawRead8(adapter->core, segmentAddress, segment);
}

static uint32_t mScriptMemoryAdapterRead16(struct mScriptMemoryAdapter* adapter, uint32_t address) {
	CALCULATE_SEGMENT_INFO;
	CALCULATE_SEGMENT_ADDRESS;
	return adapter->core->rawRead16(adapter->core, segmentAddress, segment);
}

static uint32_t mScriptMemoryAdapterRead32(struct mScriptMemoryAdapter* adapter, uint32_t address) {
	CALCULATE_SEGMENT_INFO;
	CALCULATE_SEGMENT_ADDRESS;
	return adapter->core->rawRead32(adapter->core, segmentAddress, segment);
}

static struct mScriptValue* mScriptMemoryAdapterReadRange(struct mScriptMemoryAdapter* adapter, uint32_t address, uint32_t length) {
	CALCULATE_SEGMENT_INFO;
	struct mScriptValue* value = mScriptValueAlloc(mSCRIPT_TYPE_MS_LIST);
	struct mScriptList* list = value->value.opaque;
	uint32_t i;
	for (i = 0; i < length; ++i, ++address) {
		CALCULATE_SEGMENT_ADDRESS;
		*mScriptListAppend(list) = mSCRIPT_MAKE_U32(adapter->core->rawRead8(adapter->core, segmentAddress, segment));
	}
	return value;
}

static void mScriptMemoryAdapterWrite8(struct mScriptMemoryAdapter* adapter, uint32_t address, uint8_t value) {
	CALCULATE_SEGMENT_INFO;
	CALCULATE_SEGMENT_ADDRESS;
	adapter->core->rawWrite8(adapter->core, address, segmentAddress, value);
}

static void mScriptMemoryAdapterWrite16(struct mScriptMemoryAdapter* adapter, uint32_t address, uint16_t value) {
	CALCULATE_SEGMENT_INFO;
	CALCULATE_SEGMENT_ADDRESS;
	adapter->core->rawWrite16(adapter->core, address, segmentAddress, value);
}

static void mScriptMemoryAdapterWrite32(struct mScriptMemoryAdapter* adapter, uint32_t address, uint32_t value) {
	CALCULATE_SEGMENT_INFO;
	CALCULATE_SEGMENT_ADDRESS;
	adapter->core->rawWrite32(adapter->core, address, segmentAddress, value);
}

mSCRIPT_DECLARE_STRUCT(mScriptMemoryAdapter);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptMemoryAdapter, U32, read8, mScriptMemoryAdapterRead8, 1, U32, address);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptMemoryAdapter, U32, read16, mScriptMemoryAdapterRead16, 1, U32, address);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptMemoryAdapter, U32, read32, mScriptMemoryAdapterRead32, 1, U32, address);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptMemoryAdapter, WRAPPER, readRange, mScriptMemoryAdapterReadRange, 2, U32, address, U32, length);
mSCRIPT_DECLARE_STRUCT_VOID_METHOD(mScriptMemoryAdapter, write8, mScriptMemoryAdapterWrite8, 2, U32, address, U8, value);
mSCRIPT_DECLARE_STRUCT_VOID_METHOD(mScriptMemoryAdapter, write16, mScriptMemoryAdapterWrite16, 2, U32, address, U16, value);
mSCRIPT_DECLARE_STRUCT_VOID_METHOD(mScriptMemoryAdapter, write32, mScriptMemoryAdapterWrite32, 2, U32, address, U32, value);

mSCRIPT_DEFINE_STRUCT(mScriptMemoryAdapter)
	mSCRIPT_DEFINE_DOCSTRING("Read an 8-bit value from the given offset")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptMemoryAdapter, read8)
	mSCRIPT_DEFINE_DOCSTRING("Read a 16-bit value from the given offset")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptMemoryAdapter, read16)
	mSCRIPT_DEFINE_DOCSTRING("Read a 32-bit value from the given offset")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptMemoryAdapter, read32)
	mSCRIPT_DEFINE_DOCSTRING("Read byte range from the given offset")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptMemoryAdapter, readRange)
	mSCRIPT_DEFINE_DOCSTRING("Write an 8-bit value from the given offset")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptMemoryAdapter, write8)
	mSCRIPT_DEFINE_DOCSTRING("Write a 16-bit value from the given offset")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptMemoryAdapter, write16)
	mSCRIPT_DEFINE_DOCSTRING("Write a 32-bit value from the given offset")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptMemoryAdapter, write32)
mSCRIPT_DEFINE_END;

static struct mScriptValue* _mScriptCoreGetGameTitle(const struct mCore* core) {
	char title[32] = {0};
	core->getGameTitle(core, title);
	return mScriptStringCreateFromASCII(title);
}

static struct mScriptValue* _mScriptCoreGetGameCode(const struct mCore* core) {
	char code[16] = {0};
	core->getGameCode(core, code);
	return mScriptStringCreateFromASCII(code);
}

static struct mScriptValue* _mScriptCoreReadRange(struct mCore* core, uint32_t address, uint32_t length) {
	struct mScriptValue* value = mScriptValueAlloc(mSCRIPT_TYPE_MS_LIST);
	struct mScriptList* list = value->value.opaque;
	uint32_t i;
	for (i = 0; i < length; ++i, ++address) {
		*mScriptListAppend(list) = mSCRIPT_MAKE_U32(core->busRead8(core, address));
	}
	return value;
}

static struct mScriptValue* _mScriptCoreReadRegister(const struct mCore* core, const char* regName) {
	int32_t out;
	if (!core->readRegister(core, regName, &out)) {
		return NULL;
	}
	struct mScriptValue* value = mScriptValueAlloc(mSCRIPT_TYPE_MS_S32);
	value->value.s32 = out;
	return value;
}

static void _mScriptCoreWriteRegister(struct mCore* core, const char* regName, int32_t in) {
	core->writeRegister(core, regName, &in);
}

// Info functions
mSCRIPT_DECLARE_STRUCT_CD_METHOD(mCore, S32, platform, 0);
mSCRIPT_DECLARE_STRUCT_CD_METHOD(mCore, U32, frameCounter, 0);
mSCRIPT_DECLARE_STRUCT_CD_METHOD(mCore, S32, frameCycles, 0);
mSCRIPT_DECLARE_STRUCT_CD_METHOD(mCore, S32, frequency, 0);
mSCRIPT_DECLARE_STRUCT_C_METHOD(mCore, WRAPPER, getGameTitle, _mScriptCoreGetGameTitle, 0);
mSCRIPT_DECLARE_STRUCT_C_METHOD(mCore, WRAPPER, getGameCode, _mScriptCoreGetGameCode, 0);

// Run functions
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mCore, runFrame, 0);
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mCore, step, 0);

// Key functions
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mCore, setKeys, 1, U32, keys);
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mCore, addKeys, 1, U32, keys);
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mCore, clearKeys, 1, U32, keys);
mSCRIPT_DECLARE_STRUCT_D_METHOD(mCore, U32, getKeys, 0);

// Memory functions
mSCRIPT_DECLARE_STRUCT_D_METHOD(mCore, U32, busRead8, 1, U32, address);
mSCRIPT_DECLARE_STRUCT_D_METHOD(mCore, U32, busRead16, 1, U32, address);
mSCRIPT_DECLARE_STRUCT_D_METHOD(mCore, U32, busRead32, 1, U32, address);
mSCRIPT_DECLARE_STRUCT_METHOD(mCore, WRAPPER, readRange, _mScriptCoreReadRange, 2, U32, address, U32, length);
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mCore, busWrite8, 2, U32, address, U8, value);
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mCore, busWrite16, 2, U32, address, U16, value);
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mCore, busWrite32, 2, U32, address, U32, value);

// Register functions
mSCRIPT_DECLARE_STRUCT_METHOD(mCore, WRAPPER, readRegister, _mScriptCoreReadRegister, 1, CHARP, regName);
mSCRIPT_DECLARE_STRUCT_VOID_METHOD(mCore, writeRegister, _mScriptCoreWriteRegister, 2, CHARP, regName, S32, value);

// Savestate functions
mSCRIPT_DECLARE_STRUCT_METHOD_WITH_DEFAULTS(mCore, S32, saveStateSlot, mCoreSaveState, 2, S32, slot, S32, flags);
mSCRIPT_DECLARE_STRUCT_METHOD_WITH_DEFAULTS(mCore, S32, loadStateSlot, mCoreLoadState, 2, S32, slot, S32, flags);

// Miscellaneous functions
mSCRIPT_DECLARE_STRUCT_VOID_METHOD(mCore, screenshot, mCoreTakeScreenshot, 0);

mSCRIPT_DEFINE_STRUCT(mCore)
	mSCRIPT_DEFINE_DOCSTRING("Get which platform is being emulated")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, platform)
	mSCRIPT_DEFINE_DOCSTRING("Get the number of the current frame")
	mSCRIPT_DEFINE_STRUCT_METHOD_NAMED(mCore, currentFrame, frameCounter)
	mSCRIPT_DEFINE_DOCSTRING("Get the number of cycles per frame")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, frameCycles)
	mSCRIPT_DEFINE_DOCSTRING("Get the number of cycles per second")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, frequency)

	mSCRIPT_DEFINE_DOCSTRING("Get internal title of the game from the ROM header")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, getGameTitle)
	mSCRIPT_DEFINE_DOCSTRING("Get internal product code for the game from the ROM header")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, getGameCode)

	mSCRIPT_DEFINE_DOCSTRING("Run until the next frame")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, runFrame)
	mSCRIPT_DEFINE_DOCSTRING("Run a single instruction")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, step)

	mSCRIPT_DEFINE_DOCSTRING("Set the currently active keys")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, setKeys)
	mSCRIPT_DEFINE_DOCSTRING("Add keys to the currently active key list")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, addKeys)
	mSCRIPT_DEFINE_DOCSTRING("Remove keys from the currently active key list")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, clearKeys)
	mSCRIPT_DEFINE_DOCSTRING("Get the currently active keys")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, getKeys)

	mSCRIPT_DEFINE_DOCSTRING("Read an 8-bit value from the given bus address")
	mSCRIPT_DEFINE_STRUCT_METHOD_NAMED(mCore, read8, busRead8)
	mSCRIPT_DEFINE_DOCSTRING("Read a 16-bit value from the given bus address")
	mSCRIPT_DEFINE_STRUCT_METHOD_NAMED(mCore, read16, busRead16)
	mSCRIPT_DEFINE_DOCSTRING("Read a 32-bit value from the given bus address")
	mSCRIPT_DEFINE_STRUCT_METHOD_NAMED(mCore, read32, busRead32)
	mSCRIPT_DEFINE_DOCSTRING("Read byte range from the given offset")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, readRange)
	mSCRIPT_DEFINE_DOCSTRING("Write an 8-bit value from the given bus address")
	mSCRIPT_DEFINE_STRUCT_METHOD_NAMED(mCore, write8, busWrite8)
	mSCRIPT_DEFINE_DOCSTRING("Write a 16-bit value from the given bus address")
	mSCRIPT_DEFINE_STRUCT_METHOD_NAMED(mCore, write16, busWrite16)
	mSCRIPT_DEFINE_DOCSTRING("Write a 32-bit value from the given bus address")
	mSCRIPT_DEFINE_STRUCT_METHOD_NAMED(mCore, write32, busWrite32)

	mSCRIPT_DEFINE_DOCSTRING("Read the value of the register with the given name")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, readRegister)
	mSCRIPT_DEFINE_DOCSTRING("Write the value of the register with the given name")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, writeRegister)

	mSCRIPT_DEFINE_DOCSTRING("Save state to the slot number")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, saveStateSlot)
	mSCRIPT_DEFINE_DOCSTRING("Load state from the slot number")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, loadStateSlot)

	mSCRIPT_DEFINE_DOCSTRING("Save a screenshot")
	mSCRIPT_DEFINE_STRUCT_METHOD(mCore, screenshot)
mSCRIPT_DEFINE_END;

mSCRIPT_DEFINE_STRUCT_BINDING_DEFAULTS(mCore, saveStateSlot)
	mSCRIPT_NO_DEFAULT,
	mSCRIPT_MAKE_S32(SAVESTATE_ALL)
mSCRIPT_DEFINE_DEFAULTS_END;

mSCRIPT_DEFINE_STRUCT_BINDING_DEFAULTS(mCore, loadStateSlot)
	mSCRIPT_NO_DEFAULT,
	mSCRIPT_MAKE_S32(SAVESTATE_ALL & ~SAVESTATE_SAVEDATA)
mSCRIPT_DEFINE_DEFAULTS_END;

static void _clearMemoryMap(struct mScriptContext* context, struct mScriptCoreAdapter* adapter, bool clear) {
	struct TableIterator iter;
	if (mScriptTableIteratorStart(&adapter->memory, &iter)) {
		while (true) {
			struct mScriptValue* weakref = mScriptTableIteratorGetValue(&adapter->memory, &iter);
			if (weakref) {
				if (clear) {
					mScriptContextClearWeakref(context, weakref->value.s32);
				}
				mScriptValueDeref(weakref);
			}
			if (!mScriptTableIteratorNext(&adapter->memory, &iter)) {
				break;
			}
		}
	}
	mScriptTableClear(&adapter->memory);
}

static void _rebuildMemoryMap(struct mScriptContext* context, struct mScriptCoreAdapter* adapter) {
	_clearMemoryMap(context, adapter, true);

	const struct mCoreMemoryBlock* blocks;
	size_t nBlocks = adapter->core->listMemoryBlocks(adapter->core, &blocks);
	size_t i;
	for (i = 0; i < nBlocks; ++i) {
		struct mScriptMemoryAdapter* memadapter = calloc(1, sizeof(*memadapter));
		memadapter->core = adapter->core;
		memcpy(&memadapter->block, &blocks[i], sizeof(memadapter->block));
		struct mScriptValue* value = mScriptValueAlloc(mSCRIPT_TYPE_MS_S(mScriptMemoryAdapter));
		value->flags = mSCRIPT_VALUE_FLAG_FREE_BUFFER;
		value->value.opaque = memadapter;
		struct mScriptValue* key = mScriptStringCreateFromUTF8(blocks[i].internalName);
		mScriptTableInsert(&adapter->memory, key, mScriptContextMakeWeakref(context, value));
		mScriptValueDeref(key);
	}
}

static void _mScriptCoreAdapterDeinit(struct mScriptCoreAdapter* adapter) {
	_clearMemoryMap(adapter->context, adapter, false);
	adapter->memory.type->free(&adapter->memory);
}

static struct mScriptValue* _mScriptCoreAdapterGet(struct mScriptCoreAdapter* adapter, const char* name) {
	struct mScriptValue val;
	struct mScriptValue core = mSCRIPT_MAKE(S(mCore), adapter->core);
	if (!mScriptObjectGet(&core, name, &val)) {
		return NULL;
	}

	struct mScriptValue* ret = malloc(sizeof(*ret));
	memcpy(ret, &val, sizeof(*ret));
	ret->refs = 1;
	return ret;
}

mSCRIPT_DECLARE_STRUCT(mScriptCoreAdapter);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptCoreAdapter, WRAPPER, _get, _mScriptCoreAdapterGet, 1, CHARP, name);
mSCRIPT_DECLARE_STRUCT_VOID_METHOD(mScriptCoreAdapter, _deinit, _mScriptCoreAdapterDeinit, 0);

mSCRIPT_DEFINE_STRUCT(mScriptCoreAdapter)
mSCRIPT_DEFINE_STRUCT_MEMBER_NAMED(mScriptCoreAdapter, PS(mCore), _core, core)
mSCRIPT_DEFINE_STRUCT_MEMBER(mScriptCoreAdapter, TABLE, memory)
mSCRIPT_DEFINE_STRUCT_DEINIT(mScriptCoreAdapter)
mSCRIPT_DEFINE_STRUCT_DEFAULT_GET(mScriptCoreAdapter)
mSCRIPT_DEFINE_STRUCT_CAST_TO_MEMBER(mScriptCoreAdapter, S(mCore), _core)
mSCRIPT_DEFINE_STRUCT_CAST_TO_MEMBER(mScriptCoreAdapter, CS(mCore), _core)
mSCRIPT_DEFINE_END;

void mScriptContextAttachCore(struct mScriptContext* context, struct mCore* core) {
	struct mScriptValue* coreValue = mScriptValueAlloc(mSCRIPT_TYPE_MS_S(mScriptCoreAdapter));
	struct mScriptCoreAdapter* adapter = calloc(1, sizeof(*adapter));
	adapter->core = core;
	adapter->context = context;

	adapter->memory.refs = mSCRIPT_VALUE_UNREF;
	adapter->memory.flags = 0;
	adapter->memory.type = mSCRIPT_TYPE_MS_TABLE;
	adapter->memory.type->alloc(&adapter->memory);

	_rebuildMemoryMap(context, adapter);

	coreValue->value.opaque = adapter;
	coreValue->flags = mSCRIPT_VALUE_FLAG_FREE_BUFFER;
	mScriptContextSetGlobal(context, "emu", coreValue);
}

void mScriptContextDetachCore(struct mScriptContext* context) {
	struct mScriptValue* value = HashTableLookup(&context->rootScope, "emu");
	if (!value) {
		return;
	}
	value = mScriptContextAccessWeakref(context, value);
	if (!value) {
		return;
	}
	_clearMemoryMap(context, value->value.opaque, true);
	mScriptContextRemoveGlobal(context, "emu");
}

void mScriptLog(struct mLogger* logger, struct mScriptString* msg) {
	mLogExplicit(logger, _mLOG_CAT_SCRIPT, mLOG_INFO, "%s", msg->buffer);
}

void mScriptWarn(struct mLogger* logger, struct mScriptString* msg) {
	mLogExplicit(logger, _mLOG_CAT_SCRIPT, mLOG_WARN, "%s", msg->buffer);
}

void mScriptError(struct mLogger* logger, struct mScriptString* msg) {
	mLogExplicit(logger, _mLOG_CAT_SCRIPT, mLOG_ERROR, "%s", msg->buffer);
}

mSCRIPT_DECLARE_STRUCT_VOID_METHOD(mLogger, log, mScriptLog, 1, STR, msg);
mSCRIPT_DECLARE_STRUCT_VOID_METHOD(mLogger, warn, mScriptWarn, 1, STR, msg);
mSCRIPT_DECLARE_STRUCT_VOID_METHOD(mLogger, error, mScriptError, 1, STR, msg);

mSCRIPT_DEFINE_STRUCT(mLogger)
mSCRIPT_DEFINE_STRUCT_METHOD(mLogger, log)
mSCRIPT_DEFINE_STRUCT_METHOD(mLogger, warn)
mSCRIPT_DEFINE_STRUCT_METHOD(mLogger, error)
mSCRIPT_DEFINE_END;

void mScriptContextAttachLogger(struct mScriptContext* context, struct mLogger* logger) {
	struct mScriptValue* value = mScriptValueAlloc(mSCRIPT_TYPE_MS_S(mLogger));
	value->value.opaque = logger;
	mScriptContextSetGlobal(context, "console", value);
}

void mScriptContextDetachLogger(struct mScriptContext* context) {
	mScriptContextRemoveGlobal(context, "console");
}

mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mScriptTextBuffer, deinit, 0);
mSCRIPT_DECLARE_STRUCT_CD_METHOD(mScriptTextBuffer, U32, getX, 0);
mSCRIPT_DECLARE_STRUCT_CD_METHOD(mScriptTextBuffer, U32, getY, 0);
mSCRIPT_DECLARE_STRUCT_CD_METHOD(mScriptTextBuffer, U32, cols, 0);
mSCRIPT_DECLARE_STRUCT_CD_METHOD(mScriptTextBuffer, U32, rows, 0);
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mScriptTextBuffer, print, 1, CHARP, text);
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mScriptTextBuffer, clear, 0);
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mScriptTextBuffer, setSize, 2, U32, cols, U32, rows);
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mScriptTextBuffer, moveCursor, 2, U32, x, U32, y);
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mScriptTextBuffer, advance, 1, S32, adv);
mSCRIPT_DECLARE_STRUCT_VOID_D_METHOD(mScriptTextBuffer, setName, 1, CHARP, name);

mSCRIPT_DEFINE_STRUCT(mScriptTextBuffer)
	mSCRIPT_DEFINE_STRUCT_DEINIT_NAMED(mScriptTextBuffer, deinit)
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptTextBuffer, getX)
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptTextBuffer, getY)
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptTextBuffer, cols)
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptTextBuffer, rows)
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptTextBuffer, print)
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptTextBuffer, clear)
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptTextBuffer, setSize)
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptTextBuffer, moveCursor)
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptTextBuffer, advance)
	mSCRIPT_DEFINE_DOCSTRING("Set the user-visible name of this buffer")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptTextBuffer, setName)
mSCRIPT_DEFINE_END;

struct mScriptTextBuffer* _mScriptUICreateBuffer(struct mScriptUILibrary* lib, const char* name) {
	struct mScriptTextBuffer* buffer = lib->textBufferFactory(lib->textBufferContext);
	buffer->init(buffer, name);
	return buffer;
}

mSCRIPT_DECLARE_STRUCT(mScriptUILibrary);
mSCRIPT_DECLARE_STRUCT_METHOD_WITH_DEFAULTS(mScriptUILibrary, S(mScriptTextBuffer), createBuffer, _mScriptUICreateBuffer, 1, CHARP, name);

mSCRIPT_DEFINE_STRUCT(mScriptUILibrary)
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptUILibrary, createBuffer)
mSCRIPT_DEFINE_END;

mSCRIPT_DEFINE_STRUCT_BINDING_DEFAULTS(mScriptUILibrary, createBuffer)
	mSCRIPT_MAKE_CHARP(NULL)
mSCRIPT_DEFINE_DEFAULTS_END;

void mScriptContextSetTextBufferFactory(struct mScriptContext* context, mScriptContextBufferFactory factory, void* cbContext) {
	struct mScriptValue* value = mScriptContextEnsureGlobal(context, "ui", mSCRIPT_TYPE_MS_S(mScriptUILibrary));
	struct mScriptUILibrary* uiLib = value->value.opaque;
	if (!uiLib) {
		uiLib = calloc(1, sizeof(*uiLib));
		value->value.opaque = uiLib;
		value->flags = mSCRIPT_VALUE_FLAG_FREE_BUFFER;
	}
	uiLib->textBufferFactory = factory;
	uiLib->textBufferContext = cbContext;
}
