// realsteel - mod support (2026-10-05).
//
// Step 1: see what the game loads. --realsteel_trace_files=true logs every
// file the game opens through its own file layer (sub_821F9D98: path, mode,
// flags), which is how add-on packages' archives are located.

#include "generated/default_final/realsteel_init.h"

#include <atomic>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(realsteel_trace_files, false, "Real Steel",
                    "Log every file the game opens");

namespace {

std::string GuestString(uint8_t* base, uint32_t address, size_t limit = 255) {
  std::string out;
  if (address == 0) return out;
  for (size_t i = 0; i < limit; ++i) {
    const char c = static_cast<char>(base[address + i]);
    if (c == 0) break;
    out.push_back(c);
  }
  return out;
}

}  // namespace

// The game's "open file by path": r3 = path, r4 = mode, r5 = flags; returns
// a file object or 0.
extern "C" REX_FUNC(sub_821F9D98) {
  const uint32_t path = ctx.r3.u32;
  __imp__sub_821F9D98(ctx, base);
  if (REXCVAR_GET(realsteel_trace_files)) {
    REXLOG_INFO("[mods] open '{}' -> {:#x}", GuestString(base, path), ctx.r3.u32);
  }
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>

// Diagnostics for bad mod data: when the game dereferences a null-ish guest
// pointer, log the host call stack once as module+offset (resolve with
// tools/pac/mapsym.py; recompiled guest functions are named sub_XXXXXXXX).
namespace {

LONG CALLBACK NullGuestPointerLogger(EXCEPTION_POINTERS* info) {
  static std::atomic<bool> logged{false};
  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const uint64_t address = rec->ExceptionInformation[1];
  // guest addresses only, and not the GPU register window the runtime
  // handles itself; the first three are logged
  const uint64_t guest = address & 0xFFFFFFFFull;
  if ((address >> 32) != 1 || (guest >= 0x7FC00000ull && guest < 0x80000000ull)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  static std::atomic<int> count{0};
  if (count.fetch_add(1) >= 3) return EXCEPTION_CONTINUE_SEARCH;
  (void)logged;
  CONTEXT c = *info->ContextRecord;
  std::string line;
  for (int count = 0; count < 24 && c.Rip != 0; ++count) {
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(c.Rip), &mod);
    char name[MAX_PATH] = "?";
    if (mod != nullptr) GetModuleFileNameA(mod, name, sizeof(name));
    const char* file = std::strrchr(name, 92);  // backslash
    char b[160];
    std::snprintf(b, sizeof(b), " %s+%llX", file ? file + 1 : name,
                  static_cast<unsigned long long>(c.Rip - reinterpret_cast<uint64_t>(mod)));
    line += b;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
    if (fn == nullptr) {
      if (IsBadReadPtr(reinterpret_cast<void*>(c.Rsp), 8)) break;
      c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
      c.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &establisher,
                     nullptr);
  }
  REXLOG_INFO("[mods] guest access fault at {:#x}, stack:{}", guest, line);
  return EXCEPTION_CONTINUE_SEARCH;
}

const void* const g_null_logger = AddVectoredExceptionHandler(1, NullGuestPointerLogger);

}  // namespace
#endif

// --realsteel_trace_files also traces the add-on entitlement pass
// (sub_821356A0 walks a package's catalog and adds its parts to the
// inventory): which package each catalog entry resolves to, and every part
// the pass adds.
extern "C" REX_FUNC(sub_8212F570) {
  const uint32_t key = ctx.r4.u32;
  __imp__sub_8212F570(ctx, base);
  if (REXCVAR_GET(realsteel_trace_files)) {
    static std::atomic<uint32_t> logs{0};
    if (logs.fetch_add(1) < 400) {
      REXLOG_INFO("[mods] entitlement: package number {:#x} -> object {:#x}", key, ctx.r3.u32);
    }
  }
}

extern "C" REX_FUNC(sub_82118468) {
  if (REXCVAR_GET(realsteel_trace_files)) {
    REXLOG_INFO("[mods] entitlement: add part slot {} id {:#x}", ctx.r4.u32, ctx.r5.u32);
  }
  __imp__sub_82118468(ctx, base);
}

// "Is part (slot, id) in the inventory?" - traced for add-on part ids only.
extern "C" REX_FUNC(sub_82116DB0) {
  const uint32_t slot = ctx.r4.u32, id = ctx.r5.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82116DB0(ctx, base);
  if (REXCVAR_GET(realsteel_trace_files) && id >= 0xBBF && id < 0x2000) {
    static std::atomic<uint32_t> logs{0};
    if (logs.fetch_add(1) < 300) {
      REXLOG_INFO("[mods] owned? slot {} id {:#x} -> {} (from {:#x})", slot, id, ctx.r3.u32, caller);
    }
  }
}

// Part stat record lookup (slot, id) in the current group - traced for add-on
// part ids, with the caller, to find who lists parts.
extern "C" REX_FUNC(sub_82092598) {
  const uint32_t slot = ctx.r4.u32, id = ctx.r5.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82092598(ctx, base);
  if (REXCVAR_GET(realsteel_trace_files) && id >= 0xBBF && id < 0x2000) {
    static std::atomic<uint32_t> logs{0};
    if (logs.fetch_add(1) < 400) {
      REXLOG_INFO("[mods] stats slot {} id {:#x} -> {:#x} (from {:#x})", slot, id, ctx.r3.u32, caller);
    }
  }
}

// The parts list builder (garage inventory -> list for one slot). Traced:
// the add-on parts present in the garage inventory (1000 entries of u16
// slot, u16 id, u32 state at save object + 96).
extern "C" REX_FUNC(sub_823AB418) {
  if (REXCVAR_GET(realsteel_trace_files)) {
    static std::atomic<uint32_t> logs{0};
    if (logs.fetch_add(1) < 6) {
      const uint32_t holder = REX_LOAD_U32(0x824B0000u + 1564);
      const uint32_t entries = holder + 28 + 96;
      std::string line;
      uint32_t used = 0;
      for (uint32_t i = 0; i < 1000; ++i) {
        const uint32_t slot = REX_LOAD_U16(entries + i * 8);
        const uint32_t id = REX_LOAD_U16(entries + i * 8 + 2);
        if (slot == 0xFFFF || id == 0xFFFF) continue;
        ++used;
        if (id >= 0xBBE) {
          char b[48];
          std::snprintf(b, sizeof(b), " %u:%X/%08X", slot, id, REX_LOAD_U32(entries + i * 8 + 4));
          line += b;
        }
      }
      REXLOG_INFO("[mods] list for slot {}: garage has {} entries; add-on ones:{}", ctx.r6.u32, used, line);
    }
  }
  __imp__sub_823AB418(ctx, base);
}

// Part record lookup by id in one merged slot table (u16 count, ..., records
// at [table + 8], 60 bytes each, id at +12) - traced for add-on ids.
extern "C" REX_FUNC(sub_82092750) {
  const uint32_t table = ctx.r3.u32, id = ctx.r4.u32;
  __imp__sub_82092750(ctx, base);
  if (REXCVAR_GET(realsteel_trace_files) && id >= 0xBBF && id < 0x2000) {
    static std::atomic<uint32_t> logs{0};
    if (logs.fetch_add(1) < 60) {
      const uint32_t count = REX_LOAD_U16(table), records = REX_LOAD_U32(table + 8);
      std::string tail;
      for (uint32_t i = count > 5 ? count - 5 : 0; i < count && records != 0; ++i) {
        char b[16];
        std::snprintf(b, sizeof(b), " %X", REX_LOAD_U16(records + i * 60 + 12));
        tail += b;
      }
      const uint32_t found = ctx.r3.u32;
      REXLOG_INFO("[mods] record id {:#x}: table of {} (last ids{}) -> {:#x}, id there {:#x}, hidden {}",
                  id, count, tail, found, found ? REX_LOAD_U16(found + 12) : 0,
                  found ? base[found + 43] : 0);
    }
  }
}

// Archive entry lookup by path ("/GROUP/NAME", r4) - with the file trace on,
// the menu layouts the game pulls in are logged, which tells which screen
// it is entering.
extern "C" REX_FUNC(sub_821F5F38) {
  const uint32_t path = ctx.r4.u32;
  __imp__sub_821F5F38(ctx, base);
  if (REXCVAR_GET(realsteel_trace_files) && path != 0 && base[path] == '/') {
    const std::string text = GuestString(base, path, 32);
    if (text.rfind("/MENU/", 0) == 0) REXLOG_INFO("[mods] menu entry {} -> {:#x}", text, ctx.r3.u32);
  }
}
