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

#include "src/profiling/perf/libunwind_backend.h"

#include <asm/perf_regs.h>
#include <elf.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include <libunwind.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto {
namespace profiling {
namespace {

// Context passed to libunwind accessor callbacks.
struct UnwindContext {
  const LibunwindProcessState* process_state;
  const RegisterData* regs;
  uint64_t stack_base;
  const char* stack;
  size_t stack_size;
};

int AccessMem(unw_addr_space_t /*as*/,
              unw_word_t addr,
              unw_word_t* val,
              int write,
              void* arg) {
  if (write)
    return -UNW_EINVAL;

  auto* ctx = static_cast<UnwindContext*>(arg);

  // Try the sampled stack overlay first.
  uint64_t base = ctx->stack_base;
  if (addr >= base && addr + sizeof(*val) <= base + ctx->stack_size) {
    memcpy(val, ctx->stack + (addr - base), sizeof(*val));
    return 0;
  }

  // Fall back to /proc/pid/mem.
  ssize_t rd = pread64(ctx->process_state->mem_fd(), val, sizeof(*val),
                       static_cast<off64_t>(addr));
  if (rd == static_cast<ssize_t>(sizeof(*val)))
    return 0;

  return -UNW_EINVAL;
}

int AccessReg(unw_addr_space_t /*as*/,
              unw_regnum_t reg,
              unw_word_t* val,
              int write,
              void* arg) {
  if (write)
    return -UNW_EINVAL;

  auto* ctx = static_cast<UnwindContext*>(arg);
  const auto& regs = *ctx->regs;

  // Client registers (from ParseClientRegs) are stored in AsmGetRegs order,
  // which matches libunwind's register numbering directly. Use the register
  // number as the direct index.
  if (regs.arch != kArchUnknown) {
    if (reg < 0 || static_cast<size_t>(reg) >= regs.regs.size())
      return -UNW_EBADREG;
    *val = regs.regs[static_cast<size_t>(reg)];
    return 0;
  }

  // Perf registers: need to map libunwind register numbers to perf indices.
#if defined(__x86_64__)
  static constexpr int kPerfRegMap[] = {
      PERF_REG_X86_AX,   // UNW_X86_64_RAX = 0
      PERF_REG_X86_DX,   // UNW_X86_64_RDX = 1
      PERF_REG_X86_CX,   // UNW_X86_64_RCX = 2
      PERF_REG_X86_BX,   // UNW_X86_64_RBX = 3
      PERF_REG_X86_SI,   // UNW_X86_64_RSI = 4
      PERF_REG_X86_DI,   // UNW_X86_64_RDI = 5
      PERF_REG_X86_BP,   // UNW_X86_64_RBP = 6
      PERF_REG_X86_SP,   // UNW_X86_64_RSP = 7
      PERF_REG_X86_R8,   // UNW_X86_64_R8  = 8
      PERF_REG_X86_R9,   // UNW_X86_64_R9  = 9
      PERF_REG_X86_R10,  // UNW_X86_64_R10 = 10
      PERF_REG_X86_R11,  // UNW_X86_64_R11 = 11
      PERF_REG_X86_R12,  // UNW_X86_64_R12 = 12
      PERF_REG_X86_R13,  // UNW_X86_64_R13 = 13
      PERF_REG_X86_R14,  // UNW_X86_64_R14 = 14
      PERF_REG_X86_R15,  // UNW_X86_64_R15 = 15
      PERF_REG_X86_IP,   // UNW_X86_64_RIP = 16
  };
  if (reg < 0 || reg >= static_cast<int>(std::size(kPerfRegMap)))
    return -UNW_EBADREG;

  int perf_idx = kPerfRegMap[reg];
  if (static_cast<size_t>(perf_idx) >= regs.regs.size())
    return -UNW_EBADREG;

  *val = regs.regs[static_cast<size_t>(perf_idx)];
  return 0;

#elif defined(__aarch64__)
  // libunwind aarch64: UNW_AARCH64_X0..X30 = 0..30, UNW_AARCH64_SP = 31,
  // UNW_AARCH64_PC = 32
  // perf: PERF_REG_ARM64_X0..X30 = 0..30, SP = 31, PC = 32
  // They match directly.
  if (reg < 0 || static_cast<size_t>(reg) >= regs.regs.size())
    return -UNW_EBADREG;

  *val = regs.regs[static_cast<size_t>(reg)];
  return 0;

#else
  (void)reg;
  (void)val;
  (void)regs;
  return -UNW_EBADREG;
#endif
}

int AccessFpreg(unw_addr_space_t /*as*/,
                unw_regnum_t /*reg*/,
                unw_fpreg_t* /*val*/,
                int /*write*/,
                void* /*arg*/) {
  return -UNW_EBADREG;
}

int GetProcName(unw_addr_space_t /*as*/,
                unw_word_t /*addr*/,
                char* /*buf*/,
                size_t /*buf_len*/,
                unw_word_t* /*offp*/,
                void* /*arg*/) {
  // Symbol resolution is done separately via ELF parsing.
  return -UNW_ENOINFO;
}

// Finds the .eh_frame_hdr virtual address for the ELF mapped at the given
// map entry in the target process. Reads the ELF program headers from the
// file on disk to find PT_GNU_EH_FRAME, then computes the runtime virtual
// address using the load bias.
struct EhFrameInfo {
  unw_word_t eh_frame_hdr_vaddr = 0;
  unw_word_t eh_frame_hdr_size = 0;
  unw_word_t map_start = 0;
  unw_word_t map_end = 0;
  bool found = false;
};

EhFrameInfo FindEhFrameHdr(const LibunwindProcessState::MapEntry& map) {
  EhFrameInfo result;
  if (map.name.empty() || map.name[0] != '/')
    return result;

  base::ScopedFile fd(base::OpenFile(map.name, O_RDONLY));
  if (!fd)
    return result;

  // Read ELF header.
  Elf64_Ehdr ehdr;
  if (pread(*fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr))
    return result;
  if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0)
    return result;

  bool is_64 = ehdr.e_ident[EI_CLASS] == ELFCLASS64;

  // Find the load bias: the difference between the actual load address and
  // the ELF's first PT_LOAD p_vaddr.
  unw_word_t load_bias = 0;
  unw_word_t eh_frame_hdr_vaddr = 0;
  unw_word_t eh_frame_hdr_size = 0;
  bool found_eh_frame = false;

  if (is_64) {
    // First pass: find load bias from first PT_LOAD.
    unw_word_t first_load_vaddr = 0;
    bool found_load = false;
    for (uint16_t i = 0; i < ehdr.e_phnum; ++i) {
      Elf64_Phdr phdr;
      off_t off = static_cast<off_t>(ehdr.e_phoff +
                                     static_cast<uint64_t>(i) * sizeof(phdr));
      if (pread(*fd, &phdr, sizeof(phdr), off) != sizeof(phdr))
        continue;
      if (phdr.p_type == PT_LOAD && !found_load) {
        first_load_vaddr = phdr.p_vaddr;
        found_load = true;
      }
      if (phdr.p_type == PT_GNU_EH_FRAME) {
        eh_frame_hdr_vaddr = phdr.p_vaddr;
        eh_frame_hdr_size = phdr.p_memsz;
        found_eh_frame = true;
      }
    }
    if (found_load) {
      // The load bias for a mapping at file offset `map.offset` is:
      // map.start - (first_load_vaddr + map.offset)
      // But for the first mapping (offset matching first PT_LOAD), it's:
      // map.start - first_load_vaddr
      load_bias = map.start - map.offset - first_load_vaddr;
    }
  } else {
    Elf32_Ehdr ehdr32;
    if (pread(*fd, &ehdr32, sizeof(ehdr32), 0) != sizeof(ehdr32))
      return result;
    uint32_t first_load_vaddr = 0;
    bool found_load = false;
    for (uint16_t i = 0; i < ehdr32.e_phnum; ++i) {
      Elf32_Phdr phdr;
      off_t off = static_cast<off_t>(ehdr32.e_phoff +
                                     static_cast<uint32_t>(i) * sizeof(phdr));
      if (pread(*fd, &phdr, sizeof(phdr), off) != sizeof(phdr))
        continue;
      if (phdr.p_type == PT_LOAD && !found_load) {
        first_load_vaddr = phdr.p_vaddr;
        found_load = true;
      }
      if (phdr.p_type == PT_GNU_EH_FRAME) {
        eh_frame_hdr_vaddr = phdr.p_vaddr;
        eh_frame_hdr_size = phdr.p_memsz;
        found_eh_frame = true;
      }
    }
    if (found_load)
      load_bias = map.start - map.offset - first_load_vaddr;
  }

  if (!found_eh_frame)
    return result;

  result.eh_frame_hdr_vaddr = load_bias + eh_frame_hdr_vaddr;
  result.eh_frame_hdr_size = eh_frame_hdr_size;
  result.map_start = map.start;
  result.map_end = map.end;
  result.found = true;
  return result;
}

int FindProcInfo(unw_addr_space_t as,
                 unw_word_t ip,
                 unw_proc_info_t* pip,
                 int need_unwind_info,
                 void* arg) {
  auto* ctx = static_cast<UnwindContext*>(arg);

  // Find the mapping containing the IP.
  const LibunwindProcessState::MapEntry* map = nullptr;
  for (const auto& m : ctx->process_state->maps()) {
    if (ip >= m.start && ip < m.end) {
      map = &m;
      break;
    }
  }
  if (!map)
    return -UNW_ENOINFO;

  // Find the base mapping for this binary (offset 0, same name) to compute
  // the correct load bias. With modern kernels, there can be alignment gaps
  // between mappings, so the code mapping's (start - offset) may differ from
  // the actual base address.
  const LibunwindProcessState::MapEntry* base_map = map;
  if (!map->name.empty()) {
    for (const auto& m : ctx->process_state->maps()) {
      if (m.name == map->name && m.offset == 0) {
        base_map = &m;
        break;
      }
    }
  }

  // Find .eh_frame_hdr for this ELF using the base mapping for load bias.
  EhFrameInfo ehi = FindEhFrameHdr(*base_map);
  if (!ehi.found)
    return -UNW_ENOINFO;

  return unw_get_proc_info_in_range(
      static_cast<unw_word_t>(map->start), static_cast<unw_word_t>(map->end),
      ehi.eh_frame_hdr_vaddr, ehi.eh_frame_hdr_size,
      0,  // exidx_frame_table (ARM only)
      0,  // exidx_frame_table_len (ARM only)
      as, ip, pip, need_unwind_info, arg);
}

void PutUnwindInfo(unw_addr_space_t /*as*/,
                   unw_proc_info_t* /*pip*/,
                   void* /*arg*/) {}

int Resume(unw_addr_space_t /*as*/, unw_cursor_t* /*cp*/, void* /*arg*/) {
  return -UNW_EINVAL;
}

int GetDynInfoListAddr(unw_addr_space_t /*as*/,
                       unw_word_t* /*dilap*/,
                       void* /*arg*/) {
  return -UNW_ENOINFO;
}

unw_accessors_t MakeAccessors() {
  unw_accessors_t acc = {};
  acc.find_proc_info = FindProcInfo;
  acc.put_unwind_info = PutUnwindInfo;
  acc.get_dyn_info_list_addr = GetDynInfoListAddr;
  acc.access_mem = AccessMem;
  acc.access_reg = AccessReg;
  acc.access_fpreg = AccessFpreg;
  acc.resume = Resume;
  acc.get_proc_name = GetProcName;
  return acc;
}

#if defined(__x86_64__)
constexpr uint64_t kPerfUserRegsMask =
    (1ULL << PERF_REG_X86_AX) | (1ULL << PERF_REG_X86_BX) |
    (1ULL << PERF_REG_X86_CX) | (1ULL << PERF_REG_X86_DX) |
    (1ULL << PERF_REG_X86_SI) | (1ULL << PERF_REG_X86_DI) |
    (1ULL << PERF_REG_X86_BP) | (1ULL << PERF_REG_X86_SP) |
    (1ULL << PERF_REG_X86_IP) | (1ULL << PERF_REG_X86_FLAGS) |
    (1ULL << PERF_REG_X86_CS) | (1ULL << PERF_REG_X86_SS) |
    (1ULL << PERF_REG_X86_R8) | (1ULL << PERF_REG_X86_R9) |
    (1ULL << PERF_REG_X86_R10) | (1ULL << PERF_REG_X86_R11) |
    (1ULL << PERF_REG_X86_R12) | (1ULL << PERF_REG_X86_R13) |
    (1ULL << PERF_REG_X86_R14) | (1ULL << PERF_REG_X86_R15);
constexpr size_t kPerfUserRegsCount = 20;
#elif defined(__aarch64__)
// PERF_REG_ARM64_X0..X30, SP, PC, PSTATE = 34 registers
constexpr uint64_t kPerfUserRegsMask = (1ULL << 34) - 1;
constexpr size_t kPerfUserRegsCount = 34;
#else
constexpr uint64_t kPerfUserRegsMask = 0;
constexpr size_t kPerfUserRegsCount = 0;
#endif

}  // namespace

// ---------------------------------------------------------------------------
// LibunwindProcessState
// ---------------------------------------------------------------------------

LibunwindProcessState::LibunwindProcessState(base::ScopedFile maps_fd,
                                             base::ScopedFile mem_fd)
    : maps_fd_(std::move(maps_fd)), mem_fd_(std::move(mem_fd)) {
  ParseMaps();
}

LibunwindProcessState::~LibunwindProcessState() = default;

void LibunwindProcessState::ReparseMaps() {
  maps_.clear();
  ParseMaps();
}

void LibunwindProcessState::ParseMaps() {
  if (!maps_fd_)
    return;

  // Seek to beginning and read the whole file.
  lseek(*maps_fd_, 0, SEEK_SET);
  std::string contents;
  char buf[4096];
  for (;;) {
    ssize_t rd = read(*maps_fd_, buf, sizeof(buf));
    if (rd <= 0)
      break;
    contents.append(buf, static_cast<size_t>(rd));
  }

  // Parse /proc/pid/maps format:
  // start-end perms offset dev inode pathname
  base::StringSplitter lines(std::move(contents), '\n');
  while (lines.Next()) {
    const char* line = lines.cur_token();
    MapEntry entry;

    // Parse start-end.
    char* end_ptr;
    entry.start = strtoull(line, &end_ptr, 16);
    if (*end_ptr != '-')
      continue;
    entry.end = strtoull(end_ptr + 1, &end_ptr, 16);

    // Skip perms field.
    while (*end_ptr == ' ')
      end_ptr++;
    while (*end_ptr && *end_ptr != ' ')
      end_ptr++;

    // Parse offset.
    while (*end_ptr == ' ')
      end_ptr++;
    entry.offset = strtoull(end_ptr, &end_ptr, 16);

    // Skip dev.
    while (*end_ptr == ' ')
      end_ptr++;
    while (*end_ptr && *end_ptr != ' ')
      end_ptr++;

    // Skip inode.
    while (*end_ptr == ' ')
      end_ptr++;
    while (*end_ptr && *end_ptr != ' ')
      end_ptr++;

    // Parse pathname.
    while (*end_ptr == ' ')
      end_ptr++;
    if (*end_ptr)
      entry.name = end_ptr;

    maps_.push_back(std::move(entry));
  }
}

// ---------------------------------------------------------------------------
// LibunwindBackend
// ---------------------------------------------------------------------------

LibunwindBackend::LibunwindBackend() = default;
LibunwindBackend::~LibunwindBackend() = default;

std::unique_ptr<ProcessUnwindState> LibunwindBackend::CreateProcessState(
    base::ScopedFile maps_fd,
    base::ScopedFile mem_fd) {
  return std::make_unique<LibunwindProcessState>(std::move(maps_fd),
                                                 std::move(mem_fd));
}

const LibunwindProcessState::MapEntry* LibunwindBackend::FindMap(
    const LibunwindProcessState* state,
    uint64_t pc) {
  for (const auto& m : state->maps()) {
    if (pc >= m.start && pc < m.end)
      return &m;
  }
  return nullptr;
}

std::string LibunwindBackend::ReadBuildId(const std::string& path) {
  base::ScopedFile fd(base::OpenFile(path, O_RDONLY));
  if (!fd)
    return "";

  // Read ELF header.
  Elf64_Ehdr ehdr;
  if (pread(*fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr))
    return "";
  if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0)
    return "";

  bool is_64 = ehdr.e_ident[EI_CLASS] == ELFCLASS64;
  if (!is_64) {
    // Re-read as 32-bit.
    Elf32_Ehdr ehdr32;
    if (pread(*fd, &ehdr32, sizeof(ehdr32), 0) != sizeof(ehdr32))
      return "";
    // Search section headers for .note.gnu.build-id.
    for (uint16_t i = 0; i < ehdr32.e_shnum; ++i) {
      Elf32_Shdr shdr;
      off_t off = static_cast<off_t>(ehdr32.e_shoff +
                                     static_cast<uint32_t>(i) * sizeof(shdr));
      if (pread(*fd, &shdr, sizeof(shdr), off) != sizeof(shdr))
        continue;
      if (shdr.sh_type != SHT_NOTE)
        continue;

      // Read note contents.
      std::vector<char> note(shdr.sh_size);
      if (pread(*fd, note.data(), note.size(),
                static_cast<off_t>(shdr.sh_offset)) !=
          static_cast<ssize_t>(note.size()))
        continue;

      // Parse note looking for NT_GNU_BUILD_ID.
      size_t pos = 0;
      while (pos + sizeof(Elf32_Nhdr) <= note.size()) {
        auto* nhdr = reinterpret_cast<Elf32_Nhdr*>(note.data() + pos);
        size_t name_off = pos + sizeof(Elf32_Nhdr);
        size_t name_sz = (nhdr->n_namesz + 3) & ~3u;
        size_t desc_off = name_off + name_sz;
        size_t desc_sz = (nhdr->n_descsz + 3) & ~3u;

        if (desc_off + nhdr->n_descsz > note.size())
          break;

        if (nhdr->n_type == NT_GNU_BUILD_ID && nhdr->n_namesz == 4 &&
            memcmp(note.data() + name_off, "GNU", 4) == 0) {
          std::string build_id;
          for (uint32_t j = 0; j < nhdr->n_descsz; ++j) {
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x",
                     static_cast<uint8_t>(note[desc_off + j]));
            build_id += hex;
          }
          return build_id;
        }

        pos = desc_off + desc_sz;
      }
    }
    return "";
  }

  // 64-bit path.
  for (uint16_t i = 0; i < ehdr.e_shnum; ++i) {
    Elf64_Shdr shdr;
    off_t off = static_cast<off_t>(ehdr.e_shoff +
                                   static_cast<uint64_t>(i) * sizeof(shdr));
    if (pread(*fd, &shdr, sizeof(shdr), off) != sizeof(shdr))
      continue;
    if (shdr.sh_type != SHT_NOTE)
      continue;

    std::vector<char> note(shdr.sh_size);
    if (pread(*fd, note.data(), note.size(),
              static_cast<off_t>(shdr.sh_offset)) !=
        static_cast<ssize_t>(note.size()))
      continue;

    size_t pos = 0;
    while (pos + sizeof(Elf64_Nhdr) <= note.size()) {
      auto* nhdr = reinterpret_cast<Elf64_Nhdr*>(note.data() + pos);
      size_t name_off = pos + sizeof(Elf64_Nhdr);
      size_t name_sz = (nhdr->n_namesz + 3) & ~3u;
      size_t desc_off = name_off + name_sz;
      size_t desc_sz = (nhdr->n_descsz + 3) & ~3u;

      if (desc_off + nhdr->n_descsz > note.size())
        break;

      if (nhdr->n_type == NT_GNU_BUILD_ID && nhdr->n_namesz == 4 &&
          memcmp(note.data() + name_off, "GNU", 4) == 0) {
        std::string build_id;
        for (uint32_t j = 0; j < nhdr->n_descsz; ++j) {
          char hex[3];
          snprintf(hex, sizeof(hex), "%02x",
                   static_cast<uint8_t>(note[desc_off + j]));
          build_id += hex;
        }
        return build_id;
      }

      pos = desc_off + desc_sz;
    }
  }
  return "";
}

uint64_t LibunwindBackend::GetLoadBias(const std::string& path) {
  base::ScopedFile fd(base::OpenFile(path, O_RDONLY));
  if (!fd)
    return 0;

  Elf64_Ehdr ehdr;
  if (pread(*fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr))
    return 0;
  if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0)
    return 0;

  bool is_64 = ehdr.e_ident[EI_CLASS] == ELFCLASS64;
  if (is_64) {
    for (uint16_t i = 0; i < ehdr.e_phnum; ++i) {
      Elf64_Phdr phdr;
      off_t off = static_cast<off_t>(ehdr.e_phoff +
                                     static_cast<uint64_t>(i) * sizeof(phdr));
      if (pread(*fd, &phdr, sizeof(phdr), off) != sizeof(phdr))
        continue;
      if (phdr.p_type == PT_LOAD && (phdr.p_flags & PF_X)) {
        return phdr.p_vaddr - phdr.p_offset;
      }
    }
  } else {
    Elf32_Ehdr ehdr32;
    if (pread(*fd, &ehdr32, sizeof(ehdr32), 0) != sizeof(ehdr32))
      return 0;
    for (uint16_t i = 0; i < ehdr32.e_phnum; ++i) {
      Elf32_Phdr phdr;
      off_t off = static_cast<off_t>(ehdr32.e_phoff +
                                     static_cast<uint32_t>(i) * sizeof(phdr));
      if (pread(*fd, &phdr, sizeof(phdr), off) != sizeof(phdr))
        continue;
      if (phdr.p_type == PT_LOAD && (phdr.p_flags & PF_X)) {
        return phdr.p_vaddr - phdr.p_offset;
      }
    }
  }
  return 0;
}

UnwindBackend::UnwindResult LibunwindBackend::Unwind(ProcessUnwindState* state,
                                                     const RegisterData& regs,
                                                     uint64_t stack_base,
                                                     const char* stack,
                                                     size_t stack_size,
                                                     size_t max_frames) {
  UnwindResult result;

  auto* process_state = static_cast<LibunwindProcessState*>(state);
  if (!process_state) {
    result.error_code = UnwindErrorCode::kInvalidMap;
    return result;
  }

  static unw_accessors_t accessors = MakeAccessors();
  unw_addr_space_t as = unw_create_addr_space(&accessors, 0);
  if (!as) {
    result.error_code = UnwindErrorCode::kSystemCall;
    return result;
  }

  UnwindContext ctx;
  ctx.process_state = process_state;
  ctx.regs = &regs;
  ctx.stack_base = stack_base;
  ctx.stack = stack;
  ctx.stack_size = stack_size;

  unw_cursor_t cursor;
  int ret = unw_init_remote(&cursor, as, &ctx);
  if (ret < 0) {
    unw_destroy_addr_space(as);
    result.error_code = UnwindErrorCode::kSystemCall;
    return result;
  }

  for (size_t i = 0; i < max_frames; ++i) {
    unw_word_t pc = 0;
    unw_word_t sp = 0;
    unw_get_reg(&cursor, UNW_REG_IP, &pc);
    unw_get_reg(&cursor, UNW_REG_SP, &sp);

    UnwindFrame frame;
    frame.pc = pc;
    frame.sp = sp;

    // Try to get function name.
    char proc_name[256];
    unw_word_t proc_offset = 0;
    if (unw_get_proc_name(&cursor, proc_name, sizeof(proc_name),
                          &proc_offset) == 0) {
      frame.function_name = proc_name;
      frame.function_offset = proc_offset;
    }

    // Look up mapping info.
    const auto* map = FindMap(process_state, pc);
    if (map) {
      frame.map_name = map->name;
      frame.map_start = map->start;
      frame.map_end = map->end;
      frame.map_exact_offset = map->offset;
      frame.rel_pc = pc - map->start + map->offset;

      if (!map->name.empty() && map->name[0] == '/') {
        frame.map_load_bias = GetLoadBias(map->name);
        frame.build_id = ReadBuildId(map->name);
      }
    } else {
      frame.rel_pc = pc;
    }

    result.frames.push_back(std::move(frame));

    ret = unw_step(&cursor);
    if (ret <= 0)
      break;
  }

  if (ret == 0) {
    // Normal end of stack.
    result.error_code = UnwindErrorCode::kNone;
  } else if (ret < 0) {
    // Error during stepping.
    result.error_code = UnwindErrorCode::kUnwindInfo;
  } else {
    // Hit max_frames.
    result.error_code = UnwindErrorCode::kMaxFramesExceeded;
  }

  unw_destroy_addr_space(as);
  return result;
}

UnwindBackend::UnwindResult LibunwindBackend::UnwindFramePointers(
    ProcessUnwindState* /*state*/,
    const RegisterData& /*regs*/,
    uint64_t /*stack_base*/,
    const char* /*stack*/,
    size_t /*stack_size*/,
    size_t /*max_frames*/) {
  // Frame pointer unwinding not yet supported with libunwind backend.
  UnwindResult result;
  result.error_code = UnwindErrorCode::kUnsupported;
  return result;
}

void LibunwindBackend::ReparseMaps(ProcessUnwindState* state) {
  auto* process_state = static_cast<LibunwindProcessState*>(state);
  if (process_state)
    process_state->ReparseMaps();
}

void LibunwindBackend::ResetCache() {
  // No global cache to reset for libunwind.
}

uint64_t LibunwindBackend::PerfUserRegsMask() {
  return kPerfUserRegsMask;
}

std::optional<RegisterData> LibunwindBackend::ParsePerfRegs(const char** data) {
  // Layout: [abi (u64)][regs...] where count of regs = popcount(mask).
  // The abi tells us whether the sample is from a 32-bit or 64-bit process.
  uint64_t abi;
  memcpy(&abi, *data, sizeof(abi));
  *data += sizeof(abi);

  // abi == 0 means the sample is from a kernel thread (no userspace regs).
  if (abi == 0)
    return std::nullopt;

  RegisterData result;
  result.abi = abi;
  result.regs.resize(kPerfUserRegsCount);

  memcpy(result.regs.data(), *data, kPerfUserRegsCount * sizeof(uint64_t));
  *data += kPerfUserRegsCount * sizeof(uint64_t);

  // Extract SP from the appropriate register.
#if defined(__x86_64__)
  result.sp = result.regs[PERF_REG_X86_SP];
#elif defined(__aarch64__)
  result.sp = result.regs[31];  // PERF_REG_ARM64_SP
#endif

  return result;
}

std::optional<RegisterData> LibunwindBackend::ParseClientRegs(ArchEnum arch,
                                                              void* raw_data) {
  RegisterData result;

  switch (arch) {
#if defined(__x86_64__)
    case kArchX86_64: {
      // The client AsmGetRegs stores 17 uint64_t registers in
      // AsmGetRegs order: rax, rdx, rcx, rbx, rsi, rdi, rbp, rsp,
      // r8..r15, rip. This matches libunwind's UNW_X86_64_* numbering.
      constexpr size_t kNumRegs = 17;
      auto* regs_ptr = static_cast<uint64_t*>(raw_data);
      result.regs.resize(kNumRegs);
      memcpy(result.regs.data(), regs_ptr, kNumRegs * sizeof(uint64_t));
      result.sp = regs_ptr[7];  // RSP is at index 7
      result.abi = 2;           // PERF_SAMPLE_REGS_ABI_64
      result.arch = arch;
      return result;
    }
#endif
#if defined(__aarch64__)
    case kArchArm64: {
      // 33 registers: x0..x30, sp (31), pc (32).
      // This matches libunwind's UNW_AARCH64_* numbering.
      constexpr size_t kNumRegs = 33;
      auto* regs_ptr = static_cast<uint64_t*>(raw_data);
      result.regs.resize(kNumRegs);
      memcpy(result.regs.data(), regs_ptr, kNumRegs * sizeof(uint64_t));
      result.sp = regs_ptr[31];  // SP is at index 31
      result.abi = 2;            // PERF_SAMPLE_REGS_ABI_64
      result.arch = arch;
      return result;
    }
#endif
    case kArchUnknown:
    case kArchArm:
    case kArchX86:
    case kArchMips:
    case kArchMips64:
    case kArchRiscv64:
#if defined(__x86_64__)
    case kArchArm64:
#elif defined(__aarch64__)
    case kArchX86_64:
#else
    case kArchArm64:
    case kArchX86_64:
#endif
      PERFETTO_ELOG("ParseClientRegs: unsupported arch %u",
                    static_cast<unsigned>(arch));
      return std::nullopt;
  }
}

}  // namespace profiling
}  // namespace perfetto
