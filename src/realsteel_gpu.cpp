#include "realsteel_gpu.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_BOOL(realsteel_native_gpu, true, "Real Steel",
                    "Answer the game's GPU with the built-in command-stream "
                    "service and the native renderer (false: the SDK's GPU "
                    "emulation plugin, development only)");

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace realsteel::gpu {
namespace {

#ifdef _WIN32
// Bring-up diagnostics: host call stacks of every thread, as module+offset
// (resolve against out/build/.../realsteel.map; recompiled guest functions
// are named sub_XXXXXXXX there). Used once when the guest stops submitting.
void DumpAllThreadStacks() {
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  std::vector<DWORD> tids;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids.push_back(te.th32ThreadID);
  }
  CloseHandle(snap);
  for (DWORD tid : tids) {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                           FALSE, tid);
    if (th == nullptr) continue;
    uint64_t frames[40];
    HMODULE mods[40];
    uint32_t count = 0;
    if (SuspendThread(th) != DWORD(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(th, &c)) {
        while (count < 40 && c.Rip != 0) {
          frames[count++] = c.Rip;
          DWORD64 image_base = 0;
          PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
          if (fn == nullptr) {
            // Leaf: return address is at the stack top.
            if (IsBadReadPtr(reinterpret_cast<void*>(c.Rsp), 8)) break;
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
          }
          void* handler_data = nullptr;
          DWORD64 establisher = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data,
                           &establisher, nullptr);
        }
      }
      ResumeThread(th);
    }
    CloseHandle(th);
    // Resolve modules after resuming (the loader lock may be held by the
    // suspended thread).
    std::string line;
    for (uint32_t i = 0; i < count; ++i) {
      mods[i] = nullptr;
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(frames[i]), &mods[i]);
      char name[MAX_PATH] = "?";
      if (mods[i] != nullptr) {
        GetModuleFileNameA(mods[i], name, sizeof(name));
      }
      const char* base = std::strrchr(name, 92);  // backslash
      char b[160];
      std::snprintf(b, sizeof(b), " %s+%llX", base ? base + 1 : name,
                    static_cast<unsigned long long>(
                        frames[i] - reinterpret_cast<uint64_t>(mods[i])));
      line += b;
    }
    REXLOG_INFO("[rs-gpu] stack tid {}:{}", tid, line);
  }
}
#else
void DumpAllThreadStacks() {}
#endif
}  // namespace

void SampleStacks(int count) {
  std::thread([count] {
    REXLOG_INFO("[rs-gpu] sampling stacks x{}", count);
    for (int i = 0; i < count; ++i) {
      REXLOG_INFO("[rs-gpu] sample {}", i);
      DumpAllThreadStacks();
      // 7 ms against a 16.7 ms frame: the samples walk through the frame.
      std::this_thread::sleep_for(std::chrono::milliseconds(7));
    }
    REXLOG_INFO("[rs-gpu] sampling done");
  }).detach();
}

namespace {

using rex::system::XHostThread;
using rex::system::XThread;
using rex::X_STATUS;

constexpr uint32_t kRegisterCount = 0x5003;

// Register indices (rex/graphics/register_table.inc).
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegScratchUmsk = 0x01DC;
constexpr uint32_t kRegScratchAddr = 0x01DD;
constexpr uint32_t kRegScratchReg0 = 0x0578;
constexpr uint32_t kRegCoherStatusHost = 0x0A31;
constexpr uint32_t kRegVizQueryStatus0 = 0x0C44;
constexpr uint32_t kRegVgtEventInitiator = 0x21F9;
constexpr uint32_t kRegRbSampleCountAddr = 0x2325;

// PM4 type-3 opcodes (rex/graphics/xenos.h).
enum : uint32_t {
  kPm4MeInit = 0x48,
  kPm4Nop = 0x10,
  kPm4IndirectBuffer = 0x3F,
  kPm4IndirectBufferPfd = 0x37,
  kPm4WaitRegMem = 0x3C,
  kPm4RegRmw = 0x21,
  kPm4RegToMem = 0x3E,
  kPm4MemWrite = 0x3D,
  kPm4CondWrite = 0x45,
  kPm4EventWrite = 0x46,
  kPm4EventWriteShd = 0x58,
  kPm4EventWriteExt = 0x5A,
  kPm4EventWriteZpd = 0x5B,
  kPm4VizQuery = 0x23,
  kPm4SetConstant = 0x2D,
  kPm4SetConstant2 = 0x55,
  kPm4SetShaderConstants = 0x56,
  kPm4Interrupt = 0x54,
  kPm4SetBinMaskLo = 0x60,
  kPm4SetBinMaskHi = 0x61,
  kPm4SetBinSelectLo = 0x62,
  kPm4SetBinSelectHi = 0x63,
  kPm4SetBinMask = 0x50,
  kPm4SetBinSelect = 0x51,
};

inline uint32_t LoadBE(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
         uint32_t(p[3]);
}
inline void StoreBE(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
// Xenos "GPU swap": how a value written by the GPU lands in memory, selected
// by the low two bits of the destination address.
inline uint32_t GpuSwap(uint32_t v, uint32_t endian) {
  switch (endian & 3) {
    default:
    case 0:
      return v;
    case 1:  // 8 in 16
      return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    case 2:  // 8 in 32
      return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    case 3:  // 16 in 32
      return (v >> 16) | (v << 16);
  }
}

class GpuService final : public rex::system::IGraphicsSystem {
 public:
  ~GpuService() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext*) override {
    return X_STATUS_SUCCESS;  // the native renderer owns its own surface
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = function_dispatcher;
    kernel_state_ = kernel_state;
    memory_ = kernel_state->memory();
    regs_.assign(kRegisterCount, 0);

    // GPU register block (same range the emulated GPU maps).
    memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
                                   &GpuService::ReadThunk, &GpuService::WriteThunk);

    running_.store(true);
    threads_left_.store(2);
    worker_ = rex::system::object_ref<XHostThread>(
        new XHostThread(kernel_state, 128 * 1024, 0, [this]() {
          WorkerMain();
          threads_left_.fetch_sub(1);
          return 0;
        }));
    worker_->set_name("RS GPU Commands");
    worker_->Create();
    vsync_ = rex::system::object_ref<XHostThread>(
        new XHostThread(kernel_state, 128 * 1024, 0, [this]() {
          VsyncMain();
          threads_left_.fetch_sub(1);
          return 0;
        }));
    vsync_->set_name("RS GPU VSync");
    vsync_->Create();
    REXLOG_INFO("[rs-gpu] command-stream service up (no GPU emulation plugin)");
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return false; }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    interrupt_callback_ = callback;
    interrupt_callback_data_ = user_data;
    REXLOG_INFO("[rs-gpu] interrupt callback {:#010x} data {:#010x}", callback,
                user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ring_ptr_ = ptr;
    ring_bytes_ = uint32_t(1) << (size_log2 + 3);
    read_index_ = 0;
    write_index_ = 0;
    REXLOG_INFO("[rs-gpu] ring buffer at {:#010x}, {} bytes", ptr, ring_bytes_);
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    std::lock_guard<std::mutex> lock(mutex_);
    writeback_ptr_ = ptr;
    (void)block_size_log2;
    REXLOG_INFO("[rs-gpu] read-pointer writeback at {:#010x}", ptr);
  }

  void Shutdown() override {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    // Wait (bounded) until both host threads have left their loops: the
    // object is destroyed right after this, and a worker still waking up in
    // cv_.wait_for would lock a destroyed mutex (2026-10-13: an Android start
    // without the title update ended at once and aborted in exactly that).
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (threads_left_.load() > 0 && std::chrono::steady_clock::now() < give_up) {
      cv_.notify_all();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

 private:
  static uint32_t ReadThunk(void*, void* self, uint32_t addr) {
    return static_cast<GpuService*>(self)->ReadRegister(addr);
  }
  static void WriteThunk(void*, void* self, uint32_t addr, uint32_t value) {
    static_cast<GpuService*>(self)->WriteRegisterMmio(addr, value);
  }

  // Diagnostics: the first accesses per register, and a status line every
  // two seconds (rex_latest.log, "[rs-gpu]").
  void NoteMmio(bool write, uint32_t r, uint32_t value) {
    std::lock_guard<std::mutex> lock(diag_mutex_);
    uint32_t& n = mmio_seen_[(uint32_t(write) << 16) | (r & 0xFFFF)];
    if (n++ < 3 && mmio_logged_ < 200) {
      ++mmio_logged_;
      REXLOG_INFO("[rs-gpu] mmio {} reg {:#06x} value {:#010x}",
                  write ? "write" : "read ", r, value);
    }
  }

  uint32_t ReadRegister(uint32_t addr) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    const uint32_t result = ReadRegisterValue(r);
    NoteMmio(false, r, result);
    return result;
  }

  uint32_t ReadRegisterValue(uint32_t r) {
    switch (r) {
      case 0x0F00: return 0x08100748;  // RB_EDRAM_TIMING
      case 0x0F01: return 0x0000200E;  // RB_BC_CONTROL
      case 0x194C: return 0x000002D0;  // D1MODE_V_COUNTER
      case 0x1951: return 1;           // interrupt status: vblank
      case 0x1961: return (1280u << 16) | 720u;  // D1MODE_VIEWPORT_SIZE
      default: break;
    }
    return r < kRegisterCount ? regs_[r] : 0;
  }

  void WriteRegisterMmio(uint32_t addr, uint32_t value) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    NoteMmio(true, r, value);
    if (r == kRegCpRbWptr) wptr_writes_.fetch_add(1);
    if (r == kRegCpRbWptr) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        write_index_ = value;
      }
      cv_.notify_one();
      return;
    }
    if (r < kRegisterCount) regs_[r] = value;
  }

  uint8_t* Phys(uint32_t addr) { return memory_->TranslatePhysical<uint8_t*>(addr); }

  // Register write from the command stream.
  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) return;
    regs_[index] = value;
    if (index >= kRegScratchReg0 && index < kRegScratchReg0 + 8) {
      const uint32_t n = index - kRegScratchReg0;
      if ((regs_[kRegScratchUmsk] >> n) & 1) {
        StoreBE(Phys(regs_[kRegScratchAddr] + n * 4), value);
      }
    } else if (index == kRegCoherStatusHost) {
      regs_[index] |= 0x80000000u;
    }
  }

  void DispatchInterrupt(uint32_t source, uint32_t cpu) {
    if (interrupt_callback_ == 0) return;
    XThread* thread = XThread::GetCurrentThread();
    if (thread == nullptr) return;
    if (cpu == 0xFFFFFFFFu) cpu = 2;
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source, interrupt_callback_data_};
    interrupts_begun_[source & 1].fetch_add(1);
    dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args, 2);
    interrupts_done_[source & 1].fetch_add(1);
  }

  // Sequential reader over a word range (ring segment or indirect buffer).
  struct Reader {
    const uint8_t* base;
    uint32_t words;   // total words in the buffer
    uint32_t pos;     // current word
    uint32_t end;     // one past the last word to execute
    bool ring;        // wraps at |words|
    uint32_t Left() const { return ring ? (end + words - pos) % words : end - pos; }
    uint32_t Read() {
      const uint32_t v = LoadBE(base + size_t(pos) * 4);
      pos = ring ? (pos + 1) % words : pos + 1;
      return v;
    }
    void Skip(uint32_t n) { pos = ring ? (pos + n) % words : pos + n; }
  };

  void Execute(Reader& r, int depth) {
    while (r.Left() != 0) {
      const uint32_t packet_pos = r.pos;  // restored if the packet is incomplete
      const uint32_t packet = r.Read();
      if (trace_packets_ < 400 && packet != 0) {
        ++trace_packets_;
        // Header plus up to 7 following words (may run past a short packet).
        uint32_t w[7] = {};
        Reader peek = r;
        uint32_t n = 0;
        for (; n < 7 && peek.Left() != 0; ++n) w[n] = peek.Read();
        REXLOG_INFO("[rs-gpu] pkt d{} @{} {:08X}: {:08X} {:08X} {:08X} {:08X} "
                    "{:08X} {:08X} {:08X}",
                    depth, packet_pos, packet, w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
      }
      const uint32_t type = packet >> 30;
      if (packet == 0 || type == 2) continue;  // filler / NOP
      if (type == 0) {
        const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
        if (r.Left() < count) { r.pos = packet_pos; return; }  // wait for more
        const uint32_t base_index = packet & 0x7FFF;
        const bool one_reg = ((packet >> 15) & 1) != 0;
        for (uint32_t m = 0; m < count; ++m) {
          WriteRegister(one_reg ? base_index : base_index + m, r.Read());
        }
        continue;
      }
      if (type == 1) {
        if (r.Left() < 2) { r.pos = packet_pos; return; }
        const uint32_t a = r.Read();
        const uint32_t b = r.Read();
        WriteRegister(packet & 0x7FF, a);
        WriteRegister((packet >> 11) & 0x7FF, b);
        continue;
      }
      // Type 3.
      const uint32_t opcode = (packet >> 8) & 0x7F;
      ++opcode_counts_[opcode];
      const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      if (r.Left() < count) { r.pos = packet_pos; return; }
      const uint32_t next = r.ring ? (r.pos + count) % r.words : r.pos + count;
      // Bit 0: predicated on the bin mask/select pair.
      if ((packet & 1) != 0 && (bin_mask_ & bin_select_) == 0) {
        r.pos = next;
        continue;
      }
      switch (opcode) {
        case kPm4IndirectBuffer:
        case kPm4IndirectBufferPfd: {
          const uint32_t list_ptr = r.Read();
          const uint32_t list_words = r.Read() & 0xFFFFF;
          if (depth < 8 && list_words != 0) {
            Reader sub{Phys(list_ptr), list_words, 0, list_words, false};
            Execute(sub, depth + 1);
          }
          break;
        }
        case kPm4Interrupt: {
          const uint32_t cpu_mask = r.Read();
          for (uint32_t n = 0; n < 6; ++n) {
            if (cpu_mask & (1u << n)) DispatchInterrupt(1, n);
          }
          break;
        }
        case kPm4RegRmw: {
          const uint32_t info = r.Read();
          const uint32_t and_mask = r.Read();
          const uint32_t or_mask = r.Read();
          uint32_t value = regs_[info & 0x1FFF];
          value |= ((info >> 31) & 1) ? regs_[or_mask & 0x1FFF] : or_mask;
          value &= ((info >> 30) & 1) ? regs_[and_mask & 0x1FFF] : and_mask;
          WriteRegister(info & 0x1FFF, value);
          break;
        }
        case kPm4RegToMem: {
          const uint32_t reg = r.Read();
          const uint32_t addr = r.Read();
          const uint32_t value = reg < kRegisterCount ? regs_[reg] : 0;
          StoreBE(Phys(addr & ~3u), value);
          break;
        }
        case kPm4MemWrite: {
          uint32_t addr = r.Read();
          const uint32_t endian = addr & 3;
          addr &= ~3u;
          for (uint32_t i = 1; i < count; ++i) {
            const uint32_t v = GpuSwap(r.Read(), endian);
            std::memcpy(Phys(addr), &v, 4);
            addr += 4;
          }
          break;
        }
        case kPm4CondWrite: {
          const uint32_t wait_info = r.Read();
          const uint32_t poll = r.Read();
          const uint32_t ref = r.Read();
          const uint32_t mask = r.Read();
          const uint32_t write_addr = r.Read();
          const uint32_t write_data = r.Read();
          uint32_t value;
          if (wait_info & 0x10) {
            value = LoadBE(Phys(poll & ~3u));
          } else {
            value = poll < kRegisterCount ? regs_[poll] : 0;
          }
          value &= mask;
          bool matched = false;
          switch (wait_info & 7) {
            case 0: matched = false; break;
            case 1: matched = value < ref; break;
            case 2: matched = value <= ref; break;
            case 3: matched = value == ref; break;
            case 4: matched = value != ref; break;
            case 5: matched = value >= ref; break;
            case 6: matched = value > ref; break;
            case 7: matched = true; break;
          }
          if (matched) {
            if (wait_info & 0x100) {
              const uint32_t v = GpuSwap(write_data, write_addr & 3);
              std::memcpy(Phys(write_addr & ~3u), &v, 4);
            } else {
              WriteRegister(write_addr, write_data);
            }
          }
          break;
        }
        case kPm4EventWrite: {
          WriteRegister(kRegVgtEventInitiator, r.Read() & 0x3F);
          break;
        }
        case kPm4EventWriteShd: {
          const uint32_t initiator = r.Read();
          uint32_t addr = r.Read();
          const uint32_t value = r.Read();
          WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
          // The literal value, always. (xenia substitutes its vblank
          // counter when bit 31 is set; this title's D3D writes its segment
          // fences here - a guest address such as 0xB19F00E8 with the wrap
          // count in the low two bits - and the allocator sub_822575E8 spins
          // until the fence at [dev+11024]+4 catches up. With the counter
          // substituted the very first frame never got its segment:
          // 2026-10-02, stacks of the stalled render thread.)
          const uint32_t data = value;
          const uint32_t v = GpuSwap(data, addr & 3);
          addr &= ~3u;
          std::memcpy(Phys(addr), &v, 4);
          break;
        }
        case kPm4EventWriteExt: {
          const uint32_t initiator = r.Read();
          const uint32_t addr = r.Read() & ~3u;
          WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
          // Screen extents of the frame, in units of 8 pixels (min/max x, y,
          // then z).
          const uint16_t extents[6] = {0, 1280 >> 3, 0, 720 >> 3, 0, 1};
          uint8_t* p = Phys(addr);
          for (int i = 0; i < 6; ++i) {
            p[i * 2] = uint8_t(extents[i] >> 8);
            p[i * 2 + 1] = uint8_t(extents[i]);
          }
          break;
        }
        case kPm4EventWriteZpd: {
          // Occlusion query result: report "visible" (as the emulated GPU's
          // default does).
          const uint32_t initiator = r.Read();
          WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
          uint8_t* counts = Phys(regs_[kRegRbSampleCountAddr]);
          const uint32_t kFinished = 0xFFFFFEEDu;
          const bool end_pass = LoadBE(counts + 16) == kFinished &&
                                LoadBE(counts + 20) == kFinished;
          const bool end_fail = LoadBE(counts + 8) == kFinished &&
                                LoadBE(counts + 12) == kFinished;
          std::memset(counts, 0, 32);
          if (end_pass || end_fail) StoreBE(counts, 1000);
          break;
        }
        case kPm4VizQuery: {
          const uint32_t dword0 = r.Read();
          const uint32_t id = dword0 & 0x3F;
          if (dword0 & 0x40) {  // end: report visible
            regs_[kRegVizQueryStatus0 + id / 32] |= 1u << (id % 32);
          }
          break;
        }
        case kPm4SetConstant: {
          const uint32_t offset_type = r.Read();
          uint32_t index = offset_type & 0x7FF;
          switch ((offset_type >> 16) & 0xFF) {
            case 0: index += 0x4000; break;  // ALU
            case 1: index += 0x4800; break;  // FETCH
            case 2: index += 0x4900; break;  // BOOL
            case 3: index += 0x4908; break;  // LOOP
            case 4: index += 0x2000; break;  // REGISTERS
            default: index = kRegisterCount; break;
          }
          for (uint32_t n = 0; n + 1 < count; ++n) WriteRegister(index + n, r.Read());
          break;
        }
        case kPm4SetConstant2:
        case kPm4SetShaderConstants: {
          const uint32_t index = r.Read() & 0xFFFF;
          for (uint32_t n = 0; n + 1 < count; ++n) WriteRegister(index + n, r.Read());
          break;
        }
        case kPm4SetBinMaskLo: bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | r.Read(); break;
        case kPm4SetBinMaskHi: bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); break;
        case kPm4SetBinSelectLo: bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | r.Read(); break;
        case kPm4SetBinSelectHi: bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); break;
        case kPm4SetBinMask: {
          const uint64_t hi = r.Read();
          bin_mask_ = (hi << 32) | r.Read();
          break;
        }
        case kPm4SetBinSelect: {
          const uint64_t hi = r.Read();
          bin_select_ = (hi << 32) | r.Read();
          break;
        }
        default:
          // Draws, shader/constant loads, swaps, waits, state: nothing the
          // guest can observe.
          break;
      }
      r.pos = next;
    }
  }

  void WorkerMain() {
    while (running_.load()) {
      uint32_t write_index = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
          return !running_.load() || (ring_ptr_ != 0 && write_index_ != read_index_);
        });
        if (!running_.load()) return;
        if (ring_ptr_ == 0 || write_index_ == read_index_) continue;
        write_index = write_index_;
      }
      const uint32_t words = ring_bytes_ / 4;
      Reader r{Phys(ring_ptr_), words, read_index_ % words, write_index % words, true};
      Execute(r, 0);
      uint32_t writeback = 0;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        read_index_ = r.pos;
        writeback = writeback_ptr_;
      }
      if (writeback != 0) StoreBE(Phys(writeback), r.pos);
      if (r.pos != write_index % words) {
        // A packet is still being written; give the guest a moment.
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    }
  }

  void VsyncMain() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::microseconds(16667);
    auto next = clock::now() + period;
    while (running_.load()) {
      std::this_thread::sleep_until(next);
      next += period;
      const auto now = clock::now();
      if (next < now) next = now + period;  // fell behind: do not burst
      counter_.fetch_add(1);
      // Stall watchdog: three seconds after the last command submission,
      // once, dump where every thread is.
      if (!stacks_dumped_ && wptr_writes_.load() != 0) {
        if (wptr_writes_.load() != watchdog_last_writes_) {
          watchdog_last_writes_ = wptr_writes_.load();
          watchdog_ticks_ = 0;
        } else if (++watchdog_ticks_ == 180) {
          stacks_dumped_ = true;
          REXLOG_INFO("[rs-gpu] no command submission for 3 s - thread stacks:");
          DumpAllThreadStacks();
        }
      }
      if (counter_.load() % 120 == 0 && status_lines_ < 60) {
        ++status_lines_;
        std::string ops;
        for (uint32_t op = 0; op < 128; ++op) {
          if (opcode_counts_[op] != 0) {
            char b[32];
            std::snprintf(b, sizeof(b), " %02X:%u", op, opcode_counts_[op]);
            ops += b;
          }
        }
        REXLOG_INFO("[rs-gpu] status: wptr writes {} write_index {} read_index {} "
                    "vblank irq {}/{} cmd irq {}/{} type3:{}",
                    wptr_writes_.load(), write_index_, read_index_,
                    interrupts_done_[0].load(), interrupts_begun_[0].load(),
                    interrupts_done_[1].load(), interrupts_begun_[1].load(), ops);
      }
      DispatchInterrupt(0, 2);
    }
  }

  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  std::vector<uint32_t> regs_;
  uint64_t bin_mask_ = ~0ull;
  uint64_t bin_select_ = ~0ull;

  std::mutex mutex_;
  std::condition_variable cv_;
  uint32_t ring_ptr_ = 0;
  uint32_t ring_bytes_ = 0;
  uint32_t read_index_ = 0;
  uint32_t write_index_ = 0;
  uint32_t writeback_ptr_ = 0;

  std::mutex diag_mutex_;
  std::unordered_map<uint32_t, uint32_t> mmio_seen_;
  uint32_t mmio_logged_ = 0;
  uint32_t status_lines_ = 0;
  uint32_t trace_packets_ = 0;
  bool stacks_dumped_ = false;
  uint32_t watchdog_last_writes_ = 0;
  uint32_t watchdog_ticks_ = 0;
  uint32_t opcode_counts_[128] = {};
  std::atomic<uint32_t> wptr_writes_{0};
  std::atomic<uint32_t> interrupts_begun_[2] = {};
  std::atomic<uint32_t> interrupts_done_[2] = {};
  std::atomic<bool> running_{false};
  std::atomic<int> threads_left_{0};  // host threads still inside their loops
  std::atomic<uint32_t> counter_{0};
  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;
  rex::system::object_ref<XHostThread> worker_;
  rex::system::object_ref<XHostThread> vsync_;
};

}  // namespace

bool Enabled() { return REXCVAR_GET(realsteel_native_gpu); }

std::unique_ptr<rex::system::IGraphicsSystem> Create() {
  return std::make_unique<GpuService>();
}

}  // namespace realsteel::gpu
