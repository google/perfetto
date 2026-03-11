/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/profiling/perf/libunwindstack_backend.h"

#include <mutex>

#include <unwindstack/Elf.h>
#include <unwindstack/Regs.h>
#include <unwindstack/RegsArm.h>
#include <unwindstack/RegsArm64.h>
#include <unwindstack/RegsRiscv64.h>
#include <unwindstack/RegsX86.h>
#include <unwindstack/RegsX86_64.h>
#include <unwindstack/Unwinder.h>
#include <unwindstack/UserArm.h>
#include <unwindstack/UserArm64.h>
#include <unwindstack/UserRiscv64.h>
#include <unwindstack/UserX86.h>
#include <unwindstack/UserX86_64.h>

#include <unwindstack/MachineArm.h>
#include <unwindstack/MachineArm64.h>
#include <unwindstack/MachineRiscv64.h>
#include <unwindstack/MachineX86_64.h>

#include <linux/perf_event.h>

// kernel uapi headers
#include <uapi/asm-arm/asm/perf_regs.h>
#undef PERF_REG_EXTENDED_MASK
#include <uapi/asm-x86/asm/perf_regs.h>
#undef PERF_REG_EXTENDED_MASK
#define perf_event_arm_regs perf_event_arm64_regs
#include <uapi/asm-arm64/asm/perf_regs.h>
#undef PERF_REG_EXTENDED_MASK
#undef perf_event_arm_regs
#include <uapi/asm-riscv/asm/perf_regs.h>
#undef PERF_REG_EXTENDED_MASK

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_splitter.h"
#include "src/profiling/perf/frame_pointer_unwinder.h"

namespace perfetto {
namespace profiling {

namespace {

constexpr size_t constexpr_max(size_t x, size_t y) {
  return x > y ? x : y;
}

template <typename T>
const char* ReadValue(T* value_out, const char* ptr) {
  memcpy(value_out, reinterpret_cast<const void*>(ptr), sizeof(T));
  return ptr + sizeof(T);
}

struct RawRegisterData {
  static constexpr uint64_t kMaxSize =
      constexpr_max(constexpr_max(PERF_REG_ARM_MAX, PERF_REG_ARM64_MAX),
                    constexpr_max(PERF_REG_X86_64_MAX, PERF_REG_RISCV_MAX));
  uint64_t regs[kMaxSize] = {};
};

uint64_t PerfUserRegsMaskForArch(unwindstack::ArchEnum arch) {
  switch (static_cast<uint8_t>(arch)) {
    case unwindstack::ARCH_ARM64:
      return (1ULL << PERF_REG_ARM64_MAX) - 1;
    case unwindstack::ARCH_ARM:
      return (1ULL << PERF_REG_ARM_MAX) - 1;
    case unwindstack::ARCH_X86_64:
      return (((1ULL << PERF_REG_X86_64_MAX) - 1) & ~(1ULL << PERF_REG_X86_DS) &
              ~(1ULL << PERF_REG_X86_ES) & ~(1ULL << PERF_REG_X86_FS) &
              ~(1ULL << PERF_REG_X86_GS));
    case unwindstack::ARCH_X86:
      return ((1ULL << PERF_REG_X86_32_MAX) - 1) & ~(1ULL << PERF_REG_X86_DS) &
             ~(1ULL << PERF_REG_X86_ES) & ~(1ULL << PERF_REG_X86_FS) &
             ~(1ULL << PERF_REG_X86_GS);
    case unwindstack::ARCH_RISCV64:
      return (1ULL << PERF_REG_RISCV_MAX) - 1;
    default:
      PERFETTO_FATAL("Unsupported architecture");
  }
}

unwindstack::ArchEnum ArchForAbi(unwindstack::ArchEnum arch, uint64_t abi) {
  if (arch == unwindstack::ARCH_ARM64 && abi == PERF_SAMPLE_REGS_ABI_32) {
    return unwindstack::ARCH_ARM;
  }
  if (arch == unwindstack::ARCH_X86_64 && abi == PERF_SAMPLE_REGS_ABI_32) {
    return unwindstack::ARCH_X86;
  }
  return arch;
}

std::unique_ptr<unwindstack::Regs> RawToLibUnwindstackRegs(
    const RawRegisterData& raw_regs,
    unwindstack::ArchEnum arch) {
  if (arch == unwindstack::ARCH_ARM64) {
    unwindstack::arm64_user_regs arm64_user_regs = {};
    memcpy(&arm64_user_regs.regs[0], &raw_regs.regs[0],
           sizeof(uint64_t) * (PERF_REG_ARM64_PC + 1));
    return std::unique_ptr<unwindstack::Regs>(
        unwindstack::RegsArm64::Read(&arm64_user_regs));
  }

  if (arch == unwindstack::ARCH_ARM) {
    unwindstack::arm_user_regs arm_user_regs = {};
    for (size_t i = 0; i < unwindstack::ARM_REG_LAST; i++) {
      arm_user_regs.regs[i] = static_cast<uint32_t>(raw_regs.regs[i]);
    }
    return std::unique_ptr<unwindstack::Regs>(
        unwindstack::RegsArm::Read(&arm_user_regs));
  }

  if (arch == unwindstack::ARCH_X86_64) {
    unwindstack::x86_64_user_regs x86_64_user_regs = {};
    x86_64_user_regs.rax = raw_regs.regs[PERF_REG_X86_AX];
    x86_64_user_regs.rbx = raw_regs.regs[PERF_REG_X86_BX];
    x86_64_user_regs.rcx = raw_regs.regs[PERF_REG_X86_CX];
    x86_64_user_regs.rdx = raw_regs.regs[PERF_REG_X86_DX];
    x86_64_user_regs.r8 = raw_regs.regs[PERF_REG_X86_R8];
    x86_64_user_regs.r9 = raw_regs.regs[PERF_REG_X86_R9];
    x86_64_user_regs.r10 = raw_regs.regs[PERF_REG_X86_R10];
    x86_64_user_regs.r11 = raw_regs.regs[PERF_REG_X86_R11];
    x86_64_user_regs.r12 = raw_regs.regs[PERF_REG_X86_R12];
    x86_64_user_regs.r13 = raw_regs.regs[PERF_REG_X86_R13];
    x86_64_user_regs.r14 = raw_regs.regs[PERF_REG_X86_R14];
    x86_64_user_regs.r15 = raw_regs.regs[PERF_REG_X86_R15];
    x86_64_user_regs.rdi = raw_regs.regs[PERF_REG_X86_DI];
    x86_64_user_regs.rsi = raw_regs.regs[PERF_REG_X86_SI];
    x86_64_user_regs.rbp = raw_regs.regs[PERF_REG_X86_BP];
    x86_64_user_regs.rsp = raw_regs.regs[PERF_REG_X86_SP];
    x86_64_user_regs.rip = raw_regs.regs[PERF_REG_X86_IP];
    return std::unique_ptr<unwindstack::Regs>(
        unwindstack::RegsX86_64::Read(&x86_64_user_regs));
  }

  if (arch == unwindstack::ARCH_X86) {
    unwindstack::x86_user_regs x86_user_regs = {};
    x86_user_regs.eax = static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_AX]);
    x86_user_regs.ebx = static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_BX]);
    x86_user_regs.ecx = static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_CX]);
    x86_user_regs.edx = static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_DX]);
    x86_user_regs.ebp = static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_BP]);
    x86_user_regs.edi = static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_DI]);
    x86_user_regs.esi = static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_SI]);
    x86_user_regs.esp = static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_SP]);
    x86_user_regs.eip = static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_IP]);
    return std::unique_ptr<unwindstack::Regs>(
        unwindstack::RegsX86::Read(&x86_user_regs));
  }

  if (arch == unwindstack::ARCH_RISCV64) {
    return std::unique_ptr<unwindstack::Regs>(
        unwindstack::RegsRiscv64::Read(&raw_regs.regs[0]));
  }

  PERFETTO_FATAL("Unsupported architecture");
}

}  // namespace

// LibunwindstackProcessState

LibunwindstackProcessState::LibunwindstackProcessState(base::ScopedFile maps_fd,
                                                       base::ScopedFile mem_fd)
    : metadata_(std::move(maps_fd), std::move(mem_fd)) {}

LibunwindstackProcessState::~LibunwindstackProcessState() = default;

// LibunwindstackBackend

LibunwindstackBackend::LibunwindstackBackend() {
  ResetCache();
}

LibunwindstackBackend::~LibunwindstackBackend() = default;

std::unique_ptr<ProcessUnwindState> LibunwindstackBackend::CreateProcessState(
    base::ScopedFile maps_fd,
    base::ScopedFile mem_fd) {
  return std::make_unique<LibunwindstackProcessState>(std::move(maps_fd),
                                                      std::move(mem_fd));
}

UnwindBackend::UnwindResult LibunwindstackBackend::Unwind(
    ProcessUnwindState* state,
    const RegisterData& regs,
    uint64_t stack_base,
    const char* stack,
    size_t stack_size,
    size_t max_frames) {
  auto* lus_state = static_cast<LibunwindstackProcessState*>(state);
  UnwindingMetadata& metadata = lus_state->metadata();

  auto lus_regs = ToUnwindstackRegs(regs);
  if (!lus_regs) {
    UnwindResult result;
    result.error_code = UnwindErrorCode::kBadArch;
    return result;
  }

  auto overlay_memory = std::make_shared<StackOverlayMemory>(
      metadata.fd_mem, stack_base, reinterpret_cast<const uint8_t*>(stack),
      stack_size);

  unwindstack::Unwinder unwinder(max_frames, &metadata.fd_maps, lus_regs.get(),
                                 overlay_memory);
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  unwinder.SetJitDebug(metadata.GetJitDebug(lus_regs->Arch()));
  unwinder.SetDexFiles(metadata.GetDexFiles(lus_regs->Arch()));
#endif
  unwinder.Unwind(nullptr, nullptr);

  UnwindResult result;
  result.error_code = ConvertErrorCode(unwinder.LastErrorCode());
  result.warnings = unwinder.warnings();
  for (auto& frame : unwinder.ConsumeFrames()) {
    std::string build_id = metadata.GetBuildId(frame);
    result.frames.emplace_back(ConvertFrame(frame, build_id));
  }
  return result;
}

UnwindBackend::UnwindResult LibunwindstackBackend::UnwindFramePointers(
    ProcessUnwindState* state,
    const RegisterData& regs,
    uint64_t stack_base,
    const char* stack,
    size_t stack_size,
    size_t max_frames) {
  auto* lus_state = static_cast<LibunwindstackProcessState*>(state);
  UnwindingMetadata& metadata = lus_state->metadata();

  auto lus_regs = ToUnwindstackRegs(regs);
  if (!lus_regs) {
    UnwindResult result;
    result.error_code = UnwindErrorCode::kBadArch;
    return result;
  }

  auto overlay_memory = std::make_shared<StackOverlayMemory>(
      metadata.fd_mem, stack_base, reinterpret_cast<const uint8_t*>(stack),
      stack_size);

  FramePointerUnwinder fp_unwinder(max_frames, &metadata.fd_maps,
                                   lus_regs.get(), overlay_memory, stack_size);
  fp_unwinder.Unwind();

  UnwindResult result;
  result.error_code = ConvertErrorCode(fp_unwinder.LastErrorCode());
  result.warnings = fp_unwinder.warnings();
  for (auto& frame : fp_unwinder.ConsumeFrames()) {
    std::string build_id = metadata.GetBuildId(frame);
    result.frames.emplace_back(ConvertFrame(frame, build_id));
  }
  return result;
}

void LibunwindstackBackend::ReparseMaps(ProcessUnwindState* state) {
  auto* lus_state = static_cast<LibunwindstackProcessState*>(state);
  lus_state->metadata().ReparseMaps();
}

void LibunwindstackBackend::ResetCache() {
  static std::mutex* lock = new std::mutex{};
  std::lock_guard<std::mutex> guard{*lock};
  unwindstack::Elf::SetCachingEnabled(false);
  unwindstack::Elf::SetCachingEnabled(true);
}

uint64_t LibunwindstackBackend::PerfUserRegsMask() {
  return PerfUserRegsMaskForArch(unwindstack::Regs::CurrentArch());
}

std::optional<RegisterData> LibunwindstackBackend::ParsePerfRegs(
    const char** data) {
  unwindstack::ArchEnum requested_arch = unwindstack::Regs::CurrentArch();

  const char* parse_pos = *data;
  uint64_t sampled_abi;
  parse_pos = ReadValue(&sampled_abi, parse_pos);

  if (sampled_abi == PERF_SAMPLE_REGS_ABI_NONE) {
    *data = parse_pos;
    return std::nullopt;
  }

  RawRegisterData raw_regs{};
  uint64_t regs_mask = PerfUserRegsMaskForArch(requested_arch);
  for (size_t i = 0; regs_mask && (i < RawRegisterData::kMaxSize); i++) {
    if (regs_mask & (1ULL << i)) {
      parse_pos = ReadValue(&raw_regs.regs[i], parse_pos);
    }
  }

  // arm64 kernel sampling a 32-bit arm process: fixup PC register.
  if (requested_arch == unwindstack::ARCH_ARM64 &&
      sampled_abi == PERF_SAMPLE_REGS_ABI_32) {
    raw_regs.regs[PERF_REG_ARM_PC] = raw_regs.regs[PERF_REG_ARM64_PC];
  }

  *data = parse_pos;

  // Build RegisterData from the raw registers.
  unwindstack::ArchEnum sampled_arch = ArchForAbi(requested_arch, sampled_abi);
  auto lus_regs = RawToLibUnwindstackRegs(raw_regs, sampled_arch);

  RegisterData result;
  result.sp = lus_regs->sp();
  result.abi = sampled_abi;

  // Store all raw register values.
  uint64_t mask = PerfUserRegsMaskForArch(requested_arch);
  for (size_t i = 0; i < RawRegisterData::kMaxSize; i++) {
    if (mask & (1ULL << i)) {
      result.regs.push_back(raw_regs.regs[i]);
    }
  }

  return result;
}

std::optional<RegisterData> LibunwindstackBackend::ParseClientRegs(
    ArchEnum arch,
    void* raw_data) {
  // Create libunwindstack Regs from raw data based on arch.
  std::unique_ptr<unwindstack::Regs> regs;
  switch (arch) {
    case kArchArm:
      regs.reset(new unwindstack::RegsArm());
      break;
    case kArchArm64:
      regs.reset(new unwindstack::RegsArm64());
      break;
    case kArchX86:
      regs.reset(new unwindstack::RegsX86());
      break;
    case kArchX86_64:
      regs.reset(new unwindstack::RegsX86_64());
      break;
    case kArchRiscv64:
      regs.reset(new unwindstack::RegsRiscv64());
      break;
    case kArchUnknown:
    case kArchMips:
    case kArchMips64:
      return std::nullopt;
  }

  size_t reg_bytes = regs->Is32Bit() ? sizeof(uint32_t) * regs->total_regs()
                                     : sizeof(uint64_t) * regs->total_regs();
  memcpy(regs->RawData(), raw_data, reg_bytes);

  RegisterData result;
  result.sp = regs->sp();
  result.abi = regs->Is32Bit() ? 1 : 2;  // PERF_SAMPLE_REGS_ABI_32/64
  // Mark as client regs so ToUnwindstackRegs knows the register layout.
  result.arch = arch;

  // Store register values in AsmGetRegs/libunwindstack native order.
  // For 32-bit architectures we widen to uint64_t.
  uint16_t nregs = regs->total_regs();
  result.regs.resize(nregs);
  if (regs->Is32Bit()) {
    auto* raw32 = static_cast<uint32_t*>(raw_data);
    for (uint16_t i = 0; i < nregs; i++)
      result.regs[i] = raw32[i];
  } else {
    auto* raw64 = static_cast<uint64_t*>(raw_data);
    for (uint16_t i = 0; i < nregs; i++)
      result.regs[i] = raw64[i];
  }

  return result;
}

UnwindFrame LibunwindstackBackend::ConvertFrame(
    const unwindstack::FrameData& frame,
    const std::string& build_id) {
  UnwindFrame out;
  out.rel_pc = frame.rel_pc;
  out.pc = frame.pc;
  out.sp = frame.sp;
  out.function_name = frame.function_name;
  out.function_offset = frame.function_offset;
  out.build_id = build_id;
  if (frame.map_info != nullptr) {
    out.map_name = frame.map_info->GetFullName();
    out.map_start = frame.map_info->start();
    out.map_end = frame.map_info->end();
    out.map_exact_offset = frame.map_info->offset();
    out.map_elf_start_offset = frame.map_info->elf_start_offset();
    out.map_load_bias = frame.map_info->GetLoadBias();
  }
  return out;
}

UnwindErrorCode LibunwindstackBackend::ConvertErrorCode(
    unwindstack::ErrorCode code) {
  switch (code) {
    case unwindstack::ERROR_NONE:
      return UnwindErrorCode::kNone;
    case unwindstack::ERROR_MEMORY_INVALID:
      return UnwindErrorCode::kMemoryInvalid;
    case unwindstack::ERROR_UNWIND_INFO:
      return UnwindErrorCode::kUnwindInfo;
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
    case unwindstack::ERROR_THREAD_TIMEOUT:
      return UnwindErrorCode::kThreadTimeout;
    case unwindstack::ERROR_THREAD_DOES_NOT_EXIST:
      return UnwindErrorCode::kThreadDoesNotExist;
    case unwindstack::ERROR_BAD_ARCH:
      return UnwindErrorCode::kBadArch;
    case unwindstack::ERROR_MAPS_PARSE:
      return UnwindErrorCode::kMapsParse;
    case unwindstack::ERROR_INVALID_PARAMETER:
      return UnwindErrorCode::kInvalidParameter;
    case unwindstack::ERROR_PTRACE_CALL:
      return UnwindErrorCode::kPtraceCall;
  }
  return UnwindErrorCode::kUnsupported;
}

std::unique_ptr<unwindstack::Regs> LibunwindstackBackend::ToUnwindstackRegs(
    const RegisterData& regs) {
  // Client registers (from ParseClientRegs) are in AsmGetRegs/libunwindstack
  // native order. Reconstruct the Regs object directly via memcpy into
  // RawData(), which is the same format AsmGetRegs produces.
  if (regs.arch != kArchUnknown) {
    std::unique_ptr<unwindstack::Regs> lus_regs;
    switch (regs.arch) {
      case kArchArm:
        lus_regs.reset(new unwindstack::RegsArm());
        break;
      case kArchArm64:
        lus_regs.reset(new unwindstack::RegsArm64());
        break;
      case kArchX86:
        lus_regs.reset(new unwindstack::RegsX86());
        break;
      case kArchX86_64:
        lus_regs.reset(new unwindstack::RegsX86_64());
        break;
      case kArchRiscv64:
        lus_regs.reset(new unwindstack::RegsRiscv64());
        break;
      case kArchUnknown:
      case kArchMips:
      case kArchMips64:
        return nullptr;
    }
    size_t nregs =
        std::min(static_cast<size_t>(lus_regs->total_regs()), regs.regs.size());
    if (lus_regs->Is32Bit()) {
      auto* out = static_cast<uint32_t*>(lus_regs->RawData());
      for (size_t i = 0; i < nregs; i++)
        out[i] = static_cast<uint32_t>(regs.regs[i]);
    } else {
      auto* out = static_cast<uint64_t*>(lus_regs->RawData());
      for (size_t i = 0; i < nregs; i++)
        out[i] = regs.regs[i];
    }
    return lus_regs;
  }

  // Perf registers: packed in perf mask order. Reconstruct the raw register
  // array indexed by perf register number, then convert to libunwindstack.
  unwindstack::ArchEnum requested_arch = unwindstack::Regs::CurrentArch();
  unwindstack::ArchEnum sampled_arch = ArchForAbi(requested_arch, regs.abi);

  RawRegisterData raw{};
  uint64_t mask = PerfUserRegsMaskForArch(requested_arch);
  size_t reg_idx = 0;
  for (size_t i = 0;
       i < RawRegisterData::kMaxSize && reg_idx < regs.regs.size(); i++) {
    if (mask & (1ULL << i)) {
      raw.regs[i] = regs.regs[reg_idx++];
    }
  }

  // arm64 kernel sampling a 32-bit arm process: fixup PC register.
  if (requested_arch == unwindstack::ARCH_ARM64 &&
      regs.abi == PERF_SAMPLE_REGS_ABI_32) {
    raw.regs[PERF_REG_ARM_PC] = raw.regs[PERF_REG_ARM64_PC];
  }

  return RawToLibUnwindstackRegs(raw, sampled_arch);
}

}  // namespace profiling
}  // namespace perfetto
