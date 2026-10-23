/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <procinfo/process_map.h>
#include <unistd.h>
#include <unwindstack/Elf.h>
#include <unwindstack/MachineArm.h>
#include <unwindstack/MachineArm64.h>
#include <unwindstack/MachineRiscv64.h>
#include <unwindstack/MachineX86.h>
#include <unwindstack/MachineX86_64.h>
#include <unwindstack/Maps.h>
#include <unwindstack/Regs.h>
#include <unwindstack/RegsArm.h>
#include <unwindstack/RegsArm64.h>
#include <unwindstack/RegsRiscv64.h>
#include <unwindstack/RegsX86.h>
#include <unwindstack/RegsX86_64.h>
#include <unwindstack/Unwinder.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"

namespace perfetto {
namespace profiling {

static_assert(static_cast<uint8_t>(CpuArch::UNKNOWN) ==
                      static_cast<uint8_t>(unwindstack::ARCH_UNKNOWN) &&
                  static_cast<uint8_t>(CpuArch::ARM) ==
                      static_cast<uint8_t>(unwindstack::ARCH_ARM) &&
                  static_cast<uint8_t>(CpuArch::ARM64) ==
                      static_cast<uint8_t>(unwindstack::ARCH_ARM64) &&
                  static_cast<uint8_t>(CpuArch::X86) ==
                      static_cast<uint8_t>(unwindstack::ARCH_X86) &&
                  static_cast<uint8_t>(CpuArch::X86_64) ==
                      static_cast<uint8_t>(unwindstack::ARCH_X86_64) &&
                  static_cast<uint8_t>(CpuArch::RISCV64) ==
                      static_cast<uint8_t>(unwindstack::ARCH_RISCV64),
              "CpuArch and unwindstack::ArchEnum must match");

std::unique_ptr<unwindstack::Regs> ToLibUnwindstackRegs(
    const CpuRegisters& cpu_regs) {
  switch (cpu_regs.arch) {
    case CpuArch::ARM64: {
      static_assert(static_cast<int>(ARM64_REG_MAX) ==
                        static_cast<int>(unwindstack::ARM64_REG_LAST),
                    "arm64 register layout mismatch");
      auto regs = std::make_unique<unwindstack::RegsArm64>();
      memcpy(regs->RawData(), cpu_regs.regs, sizeof(uint64_t) * ARM64_REG_MAX);
      return regs;
    }
    case CpuArch::ARM: {
      static_assert(static_cast<int>(ARM_REG_MAX) ==
                        static_cast<int>(unwindstack::ARM_REG_LAST),
                    "arm register layout mismatch");
      auto regs = std::make_unique<unwindstack::RegsArm>();
      for (size_t i = 0; i < ARM_REG_MAX; ++i) {
        (*regs)[i] = static_cast<uint32_t>(cpu_regs.regs[i]);
      }
      return regs;
    }
    case CpuArch::X86_64: {
      static_assert(static_cast<int>(X86_64_REG_MAX) ==
                        static_cast<int>(unwindstack::X86_64_REG_LAST),
                    "x86_64 register layout mismatch");
      auto regs = std::make_unique<unwindstack::RegsX86_64>();
      memcpy(regs->RawData(), cpu_regs.regs, sizeof(uint64_t) * X86_64_REG_MAX);
      return regs;
    }
    case CpuArch::X86: {
      static_assert(static_cast<int>(X86_REG_MAX) ==
                        static_cast<int>(unwindstack::X86_REG_LAST),
                    "x86 register layout mismatch");
      auto regs = std::make_unique<unwindstack::RegsX86>();
      for (size_t i = 0; i < X86_REG_MAX; ++i) {
        (*regs)[i] = static_cast<uint32_t>(cpu_regs.regs[i]);
      }
      return regs;
    }
    case CpuArch::RISCV64: {
      static_assert(static_cast<int>(RISCV64_REG_MAX) ==
                        static_cast<int>(unwindstack::RISCV64_REG_COUNT),
                    "riscv64 register layout mismatch");
      auto regs = std::make_unique<unwindstack::RegsRiscv64>();
      memcpy(regs->RawData(), cpu_regs.regs,
             sizeof(uint64_t) * RISCV64_REG_MAX);
      if ((*regs)[unwindstack::RISCV64_REG_VLENB] == 0) {
        (*regs)[unwindstack::RISCV64_REG_VLENB] =
            unwindstack::RegsRiscv64::GetVlenbFromLocal();
      }
      return regs;
    }
    case CpuArch::UNKNOWN:
      return nullptr;
  }
  return nullptr;
}

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
      content.data(), [&](const android::procinfo::MapInfo& mapinfo) {
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

namespace {

class LibunwindstackMapping : public UnwoundMapping {
 public:
  explicit LibunwindstackMapping(std::shared_ptr<unwindstack::MapInfo> map_info)
      : map_info_(std::move(map_info)) {}

  std::string map_name() const override { return map_info_->GetFullName(); }
  uint64_t map_start() const override { return map_info_->start(); }
  uint64_t map_end() const override { return map_info_->end(); }
  uint64_t map_offset() const override { return map_info_->offset(); }
  uint64_t map_start_offset() const override {
    return map_info_->elf_start_offset();
  }
  uint64_t map_load_bias() const override { return map_info_->GetLoadBias(); }
  std::string build_id() const override {
    if (!map_info_->name().empty() &&
        !(map_info_->flags() & unwindstack::MAPS_FLAGS_DEVICE_MAP)) {
      return map_info_->GetBuildID();
    }
    return "";
  }

 private:
  std::shared_ptr<unwindstack::MapInfo> map_info_;
};

inline UnwindErrorCode FromLibunwindStackErrorCode(unwindstack::ErrorCode e) {
  switch (e) {
    case unwindstack::ERROR_NONE:
      return UnwindErrorCode::kNone;
    case unwindstack::ERROR_MEMORY_INVALID:
      return UnwindErrorCode::kMemoryInvalid;
    case unwindstack::ERROR_UNWIND_INFO:
      return UnwindErrorCode::kInvalidUnwindInfo;
    case unwindstack::ERROR_UNSUPPORTED:
      return UnwindErrorCode::kUnsupported;
    case unwindstack::ERROR_INVALID_MAP:
      return UnwindErrorCode::kInvalidMap;
    case unwindstack::ERROR_MAX_FRAMES_EXCEEDED:
      return UnwindErrorCode::kMaxFramesExceeded;
    case unwindstack::ERROR_REPEATED_FRAME:
      return UnwindErrorCode::kRepeatedFrame;
    case unwindstack::ERROR_INVALID_ELF:
      return UnwindErrorCode::kInvalidElf;
    case unwindstack::ERROR_SYSTEM_CALL:
      return UnwindErrorCode::kSystemCall;
    case unwindstack::ERROR_THREAD_DOES_NOT_EXIST:
      return UnwindErrorCode::kThreadDoesNotExist;
    case unwindstack::ERROR_THREAD_TIMEOUT:
      return UnwindErrorCode::kThreadTimeout;
    case unwindstack::ERROR_BAD_ARCH:
      return UnwindErrorCode::kBadArch;
    case unwindstack::ERROR_MAPS_PARSE:
      return UnwindErrorCode::kMapParseError;
    case unwindstack::ERROR_INVALID_PARAMETER:
      return UnwindErrorCode::kInvalidParam;
    case unwindstack::ERROR_PTRACE_CALL:
      return UnwindErrorCode::kPtraceCall;
  }
  return UnwindErrorCode::kUnknownError;
}

}  // namespace

FrameData LibunwindstackContext::BuildFrameFromPc(uint64_t pc,
                                                  bool resolve_names) {
  FrameData frame{};
  frame.pc = pc;
  frame.rel_pc = pc;

  std::shared_ptr<unwindstack::MapInfo> map_info = metadata_.fd_maps.Find(pc);
  if (map_info == nullptr) {
    return frame;
  }

  const auto arch = unwindstack::Regs::CurrentArch();

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
      frame.function_offset = 0;
    }
  }

  frame.map_info = std::make_shared<LibunwindstackMapping>(std::move(map_info));

  return frame;
}

UnwindResult LibunwindstackContext::Unwind(const UnwindInputSample& sample,
                                           const UnwindOptions& options) {
  std::unique_ptr<unwindstack::Regs> regs(ToLibUnwindstackRegs(*sample.regs));
  if (regs == nullptr) {
    PERFETTO_DLOG("Unable to construct unwindstack::Regs");
    return {UnwindErrorCode::kInvalidParam, {}};
  }
  std::shared_ptr<unwindstack::Memory> mems =
      std::make_shared<StackOverlayMemory>(metadata_.fd_mem, sample.regs->sp(),
                                           sample.stack_data,
                                           sample.stack_size);

  unwindstack::Unwinder unwinder(options.max_frames, &metadata_.fd_maps,
                                 regs.get(), mems);
  unwinder.SetResolveNames(options.resolve_names);
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  unwinder.SetJitDebug(metadata_.GetJitDebug(regs->Arch()));
  unwinder.SetDexFiles(metadata_.GetDexFiles(regs->Arch()));
#endif
  unwinder.Unwind(&options.skip_initial_maps,
                  /*map_suffixes_to_ignore=*/nullptr);
  UnwindErrorCode error_code =
      FromLibunwindStackErrorCode(unwinder.LastErrorCode());
  if (error_code == UnwindErrorCode::kNone &&
      (unwinder.warnings() & unwindstack::WARNING_DEX_PC_NOT_IN_MAP) != 0) {
    error_code = UnwindErrorCode::kDexPcNotInMap;
  }

  std::vector<unwindstack::FrameData> raw_frames = unwinder.ConsumeFrames();
  std::vector<FrameData> frames;
  frames.reserve(raw_frames.size());
  const unwindstack::MapInfo* prev_frame_map_info = nullptr;
  std::shared_ptr<const UnwoundMapping> prev_mapping;
  for (unwindstack::FrameData& raw_frame : raw_frames) {
    FrameData& frame = frames.emplace_back();
    frame.pc = raw_frame.pc;
    frame.rel_pc = raw_frame.rel_pc;
    frame.sp = raw_frame.sp;
    if (!raw_frame.function_name.empty()) {
      frame.function_name = std::string(raw_frame.function_name);
    }
    frame.function_offset = raw_frame.function_offset;
    if (raw_frame.map_info != nullptr) {
      // Consecutive frames in a callstack usually belong to the same binary or
      // DSO. Reuse the wrapper when `MapInfo*` matches the previous frame to
      // avoid redundant `make_shared` heap allocations per frame.
      if (raw_frame.map_info.get() != prev_frame_map_info) {
        prev_frame_map_info = raw_frame.map_info.get();
        prev_mapping = std::make_shared<LibunwindstackMapping>(
            std::move(raw_frame.map_info));
      }
      frame.map_info = prev_mapping;
    }
  }

  return {error_code, std::move(frames)};
}

}  // namespace profiling
}  // namespace perfetto
