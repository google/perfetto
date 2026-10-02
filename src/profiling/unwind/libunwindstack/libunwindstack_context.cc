/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/profiling/unwind/libunwindstack/libunwindstack_context.h"

#include <cinttypes>
#include <memory>

#include <procinfo/process_map.h>
#include <unwindstack/Arch.h>
#include <unwindstack/Elf.h>
#include <unwindstack/MachineArm.h>
#include <unwindstack/MachineArm64.h>
#include <unwindstack/MachineRiscv64.h>
#include <unwindstack/MachineX86.h>
#include <unwindstack/MachineX86_64.h>
#include <unwindstack/Regs.h>
#include <unwindstack/RegsArm.h>
#include <unwindstack/RegsArm64.h>
#include <unwindstack/RegsRiscv64.h>
#include <unwindstack/RegsX86.h>
#include <unwindstack/RegsX86_64.h>
#include <unwindstack/UserArm.h>
#include <unwindstack/UserArm64.h>
#include <unwindstack/UserRiscv64.h>
#include <unwindstack/UserX86.h>
#include <unwindstack/UserX86_64.h>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/unwind/perf_regs.h"
#include "src/profiling/unwind/unwind_context.h"
#include "src/profiling/unwind/unwind_types.h"

namespace perfetto {
namespace profiling {

StackOverlayMemory::StackOverlayMemory(std::shared_ptr<unwindstack::Memory> mem,
                                       uint64_t sp,
                                       const uint8_t* stack,
                                       size_t size)
    : mem_(std::move(mem)), sp_(sp), stack_end_(sp + size), stack_(stack) {}

size_t StackOverlayMemory::Read(uint64_t addr, void* dst, size_t size) {
  if (addr >= sp_ && addr + size <= stack_end_ && addr + size > sp_) {
    size_t offset = static_cast<size_t>(addr - sp_);
    memcpy(dst, stack_ + offset, size);
    return size;
  }

  return mem_->Read(addr, dst, size);
}

FDMemory::FDMemory(base::ScopedFile mem_fd) : mem_fd_(std::move(mem_fd)) {}

size_t FDMemory::Read(uint64_t addr, void* dst, size_t size) {
  ssize_t rd = pread64(*mem_fd_, dst, size, static_cast<off64_t>(addr));
  if (PERFETTO_UNLIKELY(rd == -1)) {
    PERFETTO_PLOG("Failed remote pread of %zu bytes at address %" PRIx64, size,
                  addr);
    return 0;
  }
  return static_cast<size_t>(rd);
}

FDMaps::FDMaps(base::ScopedFile fd) : fd_(std::move(fd)) {}

bool FDMaps::Parse() {
  // If the process has already exited, lseek or ReadFileDescriptor will
  // return false.
  if (lseek(*fd_, 0, SEEK_SET) == -1)
    return false;

  std::string content;
  if (!base::ReadFileDescriptor(*fd_, &content))
    return false;

  unwindstack::SharedString name("");
  std::shared_ptr<unwindstack::MapInfo> prev_map;
  return android::procinfo::ReadMapFileContent(
      &content[0], [&](const android::procinfo::MapInfo& mapinfo) {
        // Mark a device map in /dev/ and not in /dev/ashmem/ specially.
        auto flags = mapinfo.flags;
        if (strncmp(mapinfo.name.c_str(), "/dev/", 5) == 0 &&
            strncmp(mapinfo.name.c_str() + 5, "ashmem/", 7) != 0) {
          flags |= unwindstack::MAPS_FLAGS_DEVICE_MAP;
        }
        // Share the string if it matches for consecutive maps.
        if (name != mapinfo.name) {
          name = unwindstack::SharedString(mapinfo.name);
        }
        maps_.emplace_back(unwindstack::MapInfo::Create(
            prev_map, mapinfo.start, mapinfo.end, mapinfo.pgoff, flags, name));
        prev_map = maps_.back();
      });
}

void FDMaps::Reset() {
  maps_.clear();
}

UnwindingMetadata::UnwindingMetadata(base::ScopedFile maps_fd,
                                     base::ScopedFile mem_fd)
    : fd_maps(std::move(maps_fd)),
      fd_mem(std::make_shared<FDMemory>(std::move(mem_fd))) {
  if (!fd_maps.Parse())
    PERFETTO_DLOG("Failed initial maps parse");
}

void UnwindingMetadata::ReparseMaps() {
  reparses++;
  fd_maps.Reset();
  fd_maps.Parse();
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  jit_debug.reset();
  dex_files.reset();
#endif
}

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
unwindstack::JitDebug* UnwindingMetadata::GetJitDebug(
    unwindstack::ArchEnum arch) {
  if (jit_debug.get() == nullptr) {
    std::vector<std::string> search_libs{"libart.so", "libartd.so"};
    jit_debug = unwindstack::CreateJitDebug(arch, fd_mem, search_libs);
  }
  return jit_debug.get();
}

unwindstack::DexFiles* UnwindingMetadata::GetDexFiles(
    unwindstack::ArchEnum arch) {
  if (dex_files.get() == nullptr) {
    std::vector<std::string> search_libs{"libart.so", "libartd.so"};
    dex_files = unwindstack::CreateDexFiles(arch, fd_mem, search_libs);
  }
  return dex_files.get();
}
#endif

// Implemention of the unwinding interface

LibunwindstackContext::LibunwindstackContext(base::ScopedFile maps_fd,
                                             base::ScopedFile mem_fd)
    : metadata_(std::move(maps_fd), std::move(mem_fd)) {}

LibunwindstackContext::~LibunwindstackContext() = default;

void LibunwindstackContext::ReparseMaps() {
  metadata_.ReparseMaps();
}

void LibunwindstackContext::ResetMaps() {
  metadata_.fd_maps.Reset();
}

std::unique_ptr<UnwindContext> UnwindContext::Create(base::ScopedFile maps_fd,
                                                     base::ScopedFile mem_fd) {
  return std::make_unique<LibunwindstackContext>(std::move(maps_fd),
                                                 std::move(mem_fd));
}
// Libunwindstack uses an unsynchronized variable for setting/checking whether
// the cache is enabled. Therefore unwinding and cache toggling should stay on
// the same thread, but we might be moving unwinding across threads if we're
// recreating |Unwinder| instances (during a reconnect to traced). Therefore,
// use our own static lock to synchronize the cache toggling.
// TODO(rsavitski): consider fixing this in libunwindstack itself.
void UnwindContext::ResetAndEnableGlobalCache() {
  unwindstack::Elf::SetCachingEnabled(false);  // free any existing state
  unwindstack::Elf::SetCachingEnabled(true);   // reallocate a fresh cache
}

namespace {
inline MapInfo FromLibunwindStackMapInfo(unwindstack::MapInfo* map_info) {
  MapInfo mi;
  mi.map_name = map_info->GetFullName();
  mi.map_start = map_info->start();
  mi.map_end = map_info->end();
  mi.map_offset = map_info->offset();
  mi.map_start_offset = map_info->elf_start_offset();
  mi.map_load_bias = map_info->GetLoadBias();
  if (!map_info->name().empty() &&
      !(map_info->flags() & unwindstack::MAPS_FLAGS_DEVICE_MAP)) {
    mi.build_id = map_info->GetBuildID();
  }
  return mi;
}

// First converts the |RawRegisterData| array to libunwindstack's "user"
// register structs (which match the ptrace/coredump format, also available at
// <sys/user.h>), then constructs the relevant unwindstack::Regs subclass out
// of the latter.
std::unique_ptr<unwindstack::Regs> ToLibUnwindstackRegs(
    const CpuRegisters& raw_regs) {
  if (raw_regs.arch == CpuArch::kArm64) {
    static_assert(static_cast<int>(unwindstack::ARM64_REG_R0) ==
                          static_cast<int>(PERF_REG_ARM64_X0) &&
                      static_cast<int>(unwindstack::ARM64_REG_R0) == 0,
                  "register layout mismatch");
    static_assert(static_cast<int>(unwindstack::ARM64_REG_PC) ==
                      static_cast<int>(PERF_REG_ARM64_PC),
                  "register layout mismatch");
    // Both the perf_event register order and the "user" format are derived from
    // "struct pt_regs", so we can directly memcpy all of the registers.
    unwindstack::arm64_user_regs arm64_user_regs = {};
    std::memcpy(&arm64_user_regs.regs[0], &raw_regs.raw_data[0],
                sizeof(uint64_t) * (PERF_REG_ARM64_PC + 1));
    return std::unique_ptr<unwindstack::Regs>(
        unwindstack::RegsArm64::Read(&arm64_user_regs));
  }

  if (raw_regs.arch == CpuArch::kArm) {
    static_assert(static_cast<int>(unwindstack::ARM_REG_R0) ==
                          static_cast<int>(PERF_REG_ARM_R0) &&
                      static_cast<int>(unwindstack::ARM_REG_R0) == 0,
                  "register layout mismatch");
    static_assert(static_cast<int>(unwindstack::ARM_REG_LAST) ==
                      static_cast<int>(PERF_REG_ARM_MAX),
                  "register layout mismatch");
    // As with arm64, the layouts match, but we need to downcast to u32.
    unwindstack::arm_user_regs arm_user_regs = {};
    for (size_t i = 0; i < unwindstack::ARM_REG_LAST; i++) {
      arm_user_regs.regs[i] = static_cast<uint32_t>(raw_regs.raw_data[i]);
    }
    return std::unique_ptr<unwindstack::Regs>(
        unwindstack::RegsArm::Read(&arm_user_regs));
  }

  if (raw_regs.arch == CpuArch::kX86_64) {
    // We've sampled more registers than what libunwindstack will use. Don't
    // copy over cs/ss/flags.
    unwindstack::x86_64_user_regs x86_64_user_regs = {};
    x86_64_user_regs.rax = raw_regs.raw_data[PERF_REG_X86_AX];
    x86_64_user_regs.rbx = raw_regs.raw_data[PERF_REG_X86_BX];
    x86_64_user_regs.rcx = raw_regs.raw_data[PERF_REG_X86_CX];
    x86_64_user_regs.rdx = raw_regs.raw_data[PERF_REG_X86_DX];
    x86_64_user_regs.r8 = raw_regs.raw_data[PERF_REG_X86_R8];
    x86_64_user_regs.r9 = raw_regs.raw_data[PERF_REG_X86_R9];
    x86_64_user_regs.r10 = raw_regs.raw_data[PERF_REG_X86_R10];
    x86_64_user_regs.r11 = raw_regs.raw_data[PERF_REG_X86_R11];
    x86_64_user_regs.r12 = raw_regs.raw_data[PERF_REG_X86_R12];
    x86_64_user_regs.r13 = raw_regs.raw_data[PERF_REG_X86_R13];
    x86_64_user_regs.r14 = raw_regs.raw_data[PERF_REG_X86_R14];
    x86_64_user_regs.r15 = raw_regs.raw_data[PERF_REG_X86_R15];
    x86_64_user_regs.rdi = raw_regs.raw_data[PERF_REG_X86_DI];
    x86_64_user_regs.rsi = raw_regs.raw_data[PERF_REG_X86_SI];
    x86_64_user_regs.rbp = raw_regs.raw_data[PERF_REG_X86_BP];
    x86_64_user_regs.rsp = raw_regs.raw_data[PERF_REG_X86_SP];
    x86_64_user_regs.rip = raw_regs.raw_data[PERF_REG_X86_IP];
    return std::unique_ptr<unwindstack::Regs>(
        unwindstack::RegsX86_64::Read(&x86_64_user_regs));
  }

  if (raw_regs.arch == CpuArch::kX86) {
    // We've sampled more registers than what libunwindstack will use. Don't
    // copy over cs/ss/flags.
    unwindstack::x86_user_regs x86_user_regs = {};
    x86_user_regs.eax =
        static_cast<uint32_t>(raw_regs.raw_data[PERF_REG_X86_AX]);
    x86_user_regs.ebx =
        static_cast<uint32_t>(raw_regs.raw_data[PERF_REG_X86_BX]);
    x86_user_regs.ecx =
        static_cast<uint32_t>(raw_regs.raw_data[PERF_REG_X86_CX]);
    x86_user_regs.edx =
        static_cast<uint32_t>(raw_regs.raw_data[PERF_REG_X86_DX]);
    x86_user_regs.ebp =
        static_cast<uint32_t>(raw_regs.raw_data[PERF_REG_X86_BP]);
    x86_user_regs.edi =
        static_cast<uint32_t>(raw_regs.raw_data[PERF_REG_X86_DI]);
    x86_user_regs.esi =
        static_cast<uint32_t>(raw_regs.raw_data[PERF_REG_X86_SI]);
    x86_user_regs.esp =
        static_cast<uint32_t>(raw_regs.raw_data[PERF_REG_X86_SP]);
    x86_user_regs.eip =
        static_cast<uint32_t>(raw_regs.raw_data[PERF_REG_X86_IP]);
    return std::unique_ptr<unwindstack::Regs>(
        unwindstack::RegsX86::Read(&x86_user_regs));
  }

  if (raw_regs.arch == CpuArch::kRiscv64) {
    static_assert(static_cast<int>(unwindstack::RISCV64_REG_PC) ==
                          static_cast<int>(PERF_REG_RISCV_PC) &&
                      static_cast<int>(unwindstack::RISCV64_REG_PC) == 0,
                  "register layout mismatch");
    static_assert(static_cast<int>(unwindstack::RISCV64_REG_REAL_COUNT) ==
                      static_cast<int>(PERF_REG_RISCV_MAX),
                  "register layout mismatch");
    // Register layout matches, pass the raw data to the Read call.
    return std::unique_ptr<unwindstack::Regs>(
        unwindstack::RegsRiscv64::Read(&raw_regs.raw_data[0]));
  }

  return nullptr;
}

}  // namespace

std::optional<FrameData> LibunwindstackContext::BuildFrameFromPc(
    uint64_t pc,
    bool resolve_names) {
  std::shared_ptr<unwindstack::MapInfo> map_info = metadata_.fd_maps.Find(pc);

  if (map_info == nullptr) {
    return std::nullopt;
  }

  const auto arch = unwindstack::Regs::CurrentArch();

  FrameData frame;
  frame.pc = pc;
  frame.rel_pc = pc;
  frame.symbol_status =
      resolve_names ? SymbolStatus::kFailure : SymbolStatus::kNotAttempted;

  unwindstack::Elf* elf = map_info->GetElf(metadata_.fd_mem, arch);
  if (elf != nullptr) {
    uint64_t relative_pc = elf->GetRelPc(pc, map_info.get());
    uint64_t pc_adjustment =
        unwindstack::GetPcAdjustment(relative_pc, elf, arch);
    frame.rel_pc = relative_pc - pc_adjustment;
    frame.pc = pc - pc_adjustment;

    unwindstack::SharedString func_name;
    if (resolve_names && elf->GetFunctionName(frame.rel_pc, &func_name,
                                              &frame.function_offset)) {
      frame.function_name = std::string(func_name);
      frame.symbol_status = SymbolStatus::kResolved;
    } else {
      frame.function_name = "";
      frame.function_offset = 0;
    }
  }

  frame.map_info = FromLibunwindStackMapInfo(map_info.get());

  return frame;
}

UnwindResult LibunwindstackContext::Unwind(const UnwindInputSample& sample,
                                           const UnwindOptions& options) {
  std::unique_ptr<unwindstack::Regs> regs(ToLibUnwindstackRegs(*sample.regs));
  if (regs == nullptr) {
    PERFETTO_DLOG("Unable to construct unwindstack::Regs");
    return {UnwindErrorCode::kInvalidParam, UnwindWarning::kNone, {}};
  }
  std::shared_ptr<unwindstack::Memory> mems =
      std::make_shared<StackOverlayMemory>(metadata_.fd_mem, sample.regs->sp,
                                           sample.stack_data,
                                           sample.stack_size);

  unwindstack::Unwinder unwinder(options.max_frames, &metadata_.fd_maps,
                                 regs.get(), mems);
  unwinder.SetResolveNames(options.resolve_names);
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  unwinder.SetJitDebug(metadata_.GetJitDebug(regs->Arch()));
  unwinder.SetDexFiles(metadata_.GetDexFiles(regs->Arch()));
#endif
  // Suppress incorrect "variable may be uninitialized" error for if condition
  // after this loop. error_code = LastErrorCode gets run at least once.
  unwindstack::ErrorCode error_code = unwindstack::ERROR_NONE;
  for (int attempt = 0; attempt < options.max_attempts; ++attempt) {
    if (attempt > 0) {
      constexpr base::TimeMillis kMapsReparseInterval{500};
      if (metadata_.last_maps_reparse_time + kMapsReparseInterval >
          base::GetWallTimeMs()) {
        PERFETTO_DLOG("Skipping reparse due to rate limit.");
        break;
      }
      PERFETTO_DLOG("Reparsing maps");
      metadata_.ReparseMaps();
      metadata_.last_maps_reparse_time = base::GetWallTimeMs();
      // Regs got invalidated by libuwindstack's speculative jump.
      // Reset.
      regs = ToLibUnwindstackRegs(*sample.regs);
      unwinder.SetRegs(regs.get());
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
      unwinder.SetJitDebug(metadata_.GetJitDebug(regs->Arch()));
      unwinder.SetDexFiles(metadata_.GetDexFiles(regs->Arch()));
#endif
    }
    unwinder.Unwind(&options.skip_initial_maps,
                    /*map_suffixes_to_ignore=*/nullptr);
    error_code = unwinder.LastErrorCode();
    if (error_code != unwindstack::ERROR_INVALID_MAP &&
        (unwinder.warnings() & unwindstack::WARNING_DEX_PC_NOT_IN_MAP) == 0) {
      break;
    }
  }

  std::vector<unwindstack::FrameData> raw_frames = unwinder.ConsumeFrames();
  std::vector<FrameData> frames;
  frames.reserve(raw_frames.size());
  for (unwindstack::FrameData& raw_frame : raw_frames) {
    FrameData frame{};
    frame.pc = raw_frame.pc;
    frame.rel_pc = raw_frame.rel_pc;
    frame.sp = raw_frame.sp;
    frame.function_name = std::move(raw_frame.function_name);
    frame.function_offset = raw_frame.function_offset;
    if (raw_frame.map_info != nullptr) {
      frame.map_info = FromLibunwindStackMapInfo(raw_frame.map_info.get());
    }
    frames.emplace_back(std::move(frame));
  }

  return {static_cast<UnwindErrorCode>(error_code), unwinder.warnings(),
          std::move(frames)};
}

}  // namespace profiling
}  // namespace perfetto
