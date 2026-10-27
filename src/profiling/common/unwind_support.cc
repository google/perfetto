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

#include "src/profiling/common/unwind_support.h"

#include <cinttypes>

#include <procinfo/process_map.h>
#include <unwindstack/MachineArm.h>
#include <unwindstack/MachineArm64.h>
#include <unwindstack/MachineRiscv64.h>
#include <unwindstack/MachineX86.h>
#include <unwindstack/MachineX86_64.h>
#include <unwindstack/Maps.h>
#include <unwindstack/Memory.h>
#include <unwindstack/RegsArm.h>
#include <unwindstack/RegsArm64.h>
#include <unwindstack/RegsRiscv64.h>
#include <unwindstack/RegsX86.h>
#include <unwindstack/RegsX86_64.h>

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

// This guarantees that kMaxRegisterDataSize in wire protocol stays the same.
static_assert(static_cast<uint16_t>(ARM_REG_MAX) ==
                      static_cast<uint16_t>(unwindstack::ARM_REG_LAST) &&
                  static_cast<uint16_t>(ARM64_REG_MAX) ==
                      static_cast<uint16_t>(unwindstack::ARM64_REG_LAST) &&
                  static_cast<uint16_t>(X86_REG_MAX) ==
                      static_cast<uint16_t>(unwindstack::X86_REG_LAST) &&
                  static_cast<uint16_t>(X86_64_REG_MAX) ==
                      static_cast<uint16_t>(unwindstack::X86_64_REG_LAST) &&
                  static_cast<uint16_t>(RISCV64_REG_MAX) ==
                      static_cast<uint16_t>(unwindstack::RISCV64_REG_COUNT),
              "Register counts must match libunwindstack");

// unwindstack::Regs registers are 32-bit (4 bytes) or 64-bit (8 bytes)
// depending on the target architecture, while CpuRegisters::regs uniformly
// stores 64-bit values. For 64-bit architectures, raw memcpy can be used
// directly; for 32-bit architectures, registers must be downcast to uint32_t
// in a loop.
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
        (*regs)[i] = static_cast<uint32_t>(cpu_regs[i]);
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
        (*regs)[i] = static_cast<uint32_t>(cpu_regs[i]);
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
      // VLENB is not included in perf sampled registers, but libunwindstack
      // requires it to compute DWARF frame offsets for functions using RISC-V
      // vector extensions.
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

std::string UnwindingMetadata::GetBuildId(const unwindstack::FrameData& frame) {
  if (frame.map_info != nullptr && !frame.map_info->name().empty() &&
      !(frame.map_info->flags() & unwindstack::MAPS_FLAGS_DEVICE_MAP)) {
    return frame.map_info->GetBuildID();
  }

  return "";
}

std::string StringifyLibUnwindstackError(unwindstack::ErrorCode e) {
  switch (e) {
    case unwindstack::ERROR_NONE:
      return "NONE";
    case unwindstack::ERROR_MEMORY_INVALID:
      return "MEMORY_INVALID";
    case unwindstack::ERROR_UNWIND_INFO:
      return "UNWIND_INFO";
    case unwindstack::ERROR_UNSUPPORTED:
      return "UNSUPPORTED";
    case unwindstack::ERROR_INVALID_MAP:
      return "INVALID_MAP";
    case unwindstack::ERROR_MAX_FRAMES_EXCEEDED:
      return "MAX_FRAME_EXCEEDED";
    case unwindstack::ERROR_REPEATED_FRAME:
      return "REPEATED_FRAME";
    case unwindstack::ERROR_INVALID_ELF:
      return "INVALID_ELF";
    case unwindstack::ERROR_SYSTEM_CALL:
      return "SYSTEM_CALL";
    case unwindstack::ERROR_THREAD_DOES_NOT_EXIST:
      return "THREAD_DOES_NOT_EXIST";
    case unwindstack::ERROR_THREAD_TIMEOUT:
      return "THREAD_TIMEOUT";
    case unwindstack::ERROR_BAD_ARCH:
      return "BAD_ARCH";
    case unwindstack::ERROR_MAPS_PARSE:
      return "MAPS_PARSE";
    case unwindstack::ERROR_INVALID_PARAMETER:
      return "INVALID_PARAMETER";
    case unwindstack::ERROR_PTRACE_CALL:
      return "PTRACE_CALL";
  }
}

}  // namespace profiling
}  // namespace perfetto
