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

#include "src/profiling/unwind/libunwind/libunwind_context.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/unwind/libunwind/elf_helper.h"
#include "src/profiling/unwind/libunwind/process_map.h"
#include "src/profiling/unwind/perf_regs.h"
#include "src/profiling/unwind/unwind_context.h"
#include "src/profiling/unwind/unwind_types.h"

#define dwarf_find_unwind_table UNW_OBJ(dwarf_find_unwind_table)
#define dwarf_search_unwind_table UNW_OBJ(dwarf_search_unwind_table)

extern "C" {
int dwarf_find_unwind_table(void* edi,
                            unw_addr_space_t as,
                            const char* path,
                            unw_word_t segbase,
                            unw_word_t mapoff,
                            unw_word_t ip);
int dwarf_search_unwind_table(unw_addr_space_t as,
                              unw_word_t ip,
                              unw_dyn_info_t* di,
                              unw_proc_info_t* pi,
                              int need_unwind_info,
                              void* arg);
}

namespace perfetto {
namespace profiling {

namespace {

struct CursorState {
  LibunwindContext* context = nullptr;
  const UnwindInputSample* sample = nullptr;
  int mem_fd = -1;
};

int FindProcInfo(unw_addr_space_t as,
                 unw_word_t ip,
                 unw_proc_info_t* pi,
                 int need_unwind_info,
                 void* arg) {
  auto* state = static_cast<CursorState*>(arg);
  return state->context->FindProcInfo(as, ip, pi, need_unwind_info, arg);
}

void PutUnwindInfo(unw_addr_space_t /*as*/,
                   unw_proc_info_t* pi,
                   void* /*arg*/) {
  if (pi->unwind_info) {
    free(pi->unwind_info);
    pi->unwind_info = nullptr;
  }
}

int GetDynInfoListAddr(unw_addr_space_t /*as*/,
                       unw_word_t* /*dil_addr*/,
                       void* /*arg*/) {
  return -UNW_ENOINFO;
}

int GetProcName(unw_addr_space_t /*as*/,
                unw_word_t /*ip*/,
                char* /*buf*/,
                size_t /*buf_len*/,
                unw_word_t* /*offp*/,
                void* /*arg*/) {
  return -UNW_ENOINFO;
}

int AccessMem(unw_addr_space_t /*as*/,
              unw_word_t addr,
              unw_word_t* valp,
              int write,
              void* arg) {
  if (write)
    return -UNW_EINVAL;
  const auto* state = static_cast<const CursorState*>(arg);
  const auto* s = state->sample;
  // 1. Read from captured stack snapshot if within [sp, sp + stack_size)
  if (s->stack_data && s->stack_size > 0 && s->regs) {
    uint64_t sp = s->regs->sp;
    if (addr >= sp && addr + sizeof(unw_word_t) <= sp + s->stack_size) {
      memcpy(valp, s->stack_data + (addr - sp), sizeof(unw_word_t));
      return 0;
    }
  }
  // 2. Read from cached ELF mmap (mimicking Linux perf's `access_dso_mem`)
  if (state->context && state->context->AccessDsoMem(addr, valp)) {
    return 0;
  }
  // 3. Fall back to /proc/<pid>/mem
  if (state->mem_fd >= 0 &&
      pread64(state->mem_fd, valp, sizeof(unw_word_t),
              static_cast<off64_t>(addr)) == sizeof(unw_word_t)) {
    return 0;
  }
  return -UNW_EUNSPEC;
}

int AccessReg(unw_addr_space_t /*as*/,
              unw_regnum_t regnum,
              unw_word_t* valp,
              int write,
              void* arg) {
  if (write)
    return -UNW_EINVAL;
  auto* state = static_cast<CursorState*>(arg);
  const CpuRegisters& r = *state->sample->regs;
  switch (regnum) {
    case UNW_X86_64_RAX:
      *valp = r.raw_data[PERF_REG_X86_AX];
      return 0;
    case UNW_X86_64_RDX:
      *valp = r.raw_data[PERF_REG_X86_DX];
      return 0;
    case UNW_X86_64_RCX:
      *valp = r.raw_data[PERF_REG_X86_CX];
      return 0;
    case UNW_X86_64_RBX:
      *valp = r.raw_data[PERF_REG_X86_BX];
      return 0;
    case UNW_X86_64_RSI:
      *valp = r.raw_data[PERF_REG_X86_SI];
      return 0;
    case UNW_X86_64_RDI:
      *valp = r.raw_data[PERF_REG_X86_DI];
      return 0;
    case UNW_X86_64_RBP:
      *valp = r.raw_data[PERF_REG_X86_BP];
      return 0;
    case UNW_X86_64_RSP:
      *valp = r.sp;
      return 0;
    case UNW_X86_64_R8:
      *valp = r.raw_data[PERF_REG_X86_R8];
      return 0;
    case UNW_X86_64_R9:
      *valp = r.raw_data[PERF_REG_X86_R9];
      return 0;
    case UNW_X86_64_R10:
      *valp = r.raw_data[PERF_REG_X86_R10];
      return 0;
    case UNW_X86_64_R11:
      *valp = r.raw_data[PERF_REG_X86_R11];
      return 0;
    case UNW_X86_64_R12:
      *valp = r.raw_data[PERF_REG_X86_R12];
      return 0;
    case UNW_X86_64_R13:
      *valp = r.raw_data[PERF_REG_X86_R13];
      return 0;
    case UNW_X86_64_R14:
      *valp = r.raw_data[PERF_REG_X86_R14];
      return 0;
    case UNW_X86_64_R15:
      *valp = r.raw_data[PERF_REG_X86_R15];
      return 0;
    case UNW_X86_64_RIP:
      *valp = r.pc;
      return 0;
    default:
      return -UNW_EBADREG;
  }
}

unw_accessors_t* GetAccessors() {
  static unw_accessors_t accessors = [] {
    unw_accessors_t a{};
    a.find_proc_info = &FindProcInfo;
    a.put_unwind_info = &PutUnwindInfo;
    a.get_dyn_info_list_addr = &GetDynInfoListAddr;
    a.access_mem = &AccessMem;
    a.access_reg = &AccessReg;
    a.get_proc_name = &GetProcName;
    return a;
  }();
  return &accessors;
}

}  // namespace

// Implemention of the unwinding interface

LibunwindContext::LibunwindContext(base::ScopedFile maps_fd,
                                   base::ScopedFile mem_fd)
    : maps_fd_(std::move(maps_fd)), mem_fd_(std::move(mem_fd)) {
  ParseMaps();
  addr_space_ = unw_create_addr_space(GetAccessors(), 0);
  // Enable DWARF frame-state caching across unwinds (defaults to
  // UNW_CACHE_NONE).
  // TODO(safayat): set cache size limit
  unw_set_caching_policy(addr_space_, UNW_CACHE_PER_THREAD);
}

LibunwindContext::~LibunwindContext() {
  for (auto it = dso_mmaps_.GetIterator(); it; ++it) {
    if (it.value().edi.di_debug.format != -1 &&
        it.value().edi.di_debug.u.ti.table_data) {
      free(it.value().edi.di_debug.u.ti.table_data);
    }
    if (it.value().data)
      munmap(it.value().data, it.value().size);
  }
  if (addr_space_)
    unw_destroy_addr_space(addr_space_);
}

void LibunwindContext::ReparseMaps() {
  // TODO(safayat): consider moving to common base class
  reparses_count_++;
  ResetMaps();
  ParseMaps();
}

std::unique_ptr<UnwindContext> UnwindContext::Create(base::ScopedFile maps_fd,
                                                     base::ScopedFile mem_fd) {
  return std::make_unique<LibunwindContext>(std::move(maps_fd),
                                            std::move(mem_fd));
}

void LibunwindContext::ResetMaps() {
  maps_.clear();
  if (addr_space_)
    unw_flush_cache(addr_space_, 0, 0);
}

void LibunwindContext::ParseMaps() {
  if (!maps_fd_ || lseek(*maps_fd_, 0, SEEK_SET) == -1) {
    return;
  }

  std::string content;
  if (!base::ReadFileDescriptor(*maps_fd_, &content))
    return;

  procinfo::ReadMapFileContent(&content[0], [&](const procinfo::MapInfo& info) {
    MapInfo map;
    map.map_start = info.start;
    map.map_end = info.end;
    map.map_offset = info.pgoff;
    map.map_start_offset = info.pgoff;
    map.map_name = info.name;

    if ((info.flags & PROT_EXEC) && !maps_.empty() &&
        maps_.back().map_name == info.name) {
      map.map_start_offset = maps_.back().map_start_offset;
    }
    maps_.push_back(std::move(map));
  });
}

void UnwindContext::ResetAndEnableGlobalCache() {}

// Mirroring `Maps::Find` from `libunwindstack/Maps.cpp` (and `maps__find` in
// Linux `perf`).
MapInfo* LibunwindContext::FindMap(uint64_t pc) {
  auto it = std::upper_bound(
      maps_.begin(), maps_.end(), pc,
      [](uint64_t val, const MapInfo& m) { return val < m.map_start; });
  if (it != maps_.begin()) {
    --it;
    if (pc >= it->map_start && pc < it->map_end) {
      return &(*it);
    }
  }
  return nullptr;
}

LibunwindContext::MappedFile* LibunwindContext::GetOrMapDso(
    const std::string& path) {
  if (path.empty() || path[0] != '/')
    return nullptr;

  MappedFile* found = dso_mmaps_.Find(path);
  if (!found) {
    MappedFile mapped;
    base::ScopedFile fd(base::OpenFile(path, O_RDONLY | O_CLOEXEC));
    struct stat st{};
    if (fd && fstat(*fd, &st) == 0 && st.st_size > 0) {
      size_t size = static_cast<size_t>(st.st_size);
      void* ptr = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, *fd, 0);
      if (ptr != MAP_FAILED) {
        mapped.data = ptr;
        mapped.size = size;
        mapped.load_bias = GetElfLoadBias(ptr, size);
      }
    }
    found = dso_mmaps_.Insert(path, std::move(mapped)).first;
  }
  return found->data ? found : nullptr;
}

// Uses `libunwind`'s `dwarf_find_unwind_table` (to parse `.eh_frame_hdr`,
// `.debug_frame`, and `.gnu_debugdata` once per mapped DSO) and
// `dwarf_search_unwind_table` (to binary-search the table), avoiding
// `_UPT_find_proc_info` which only caches 1 ELF at a time and re-reads
// `/proc/<pid>/maps` on every DSO transition or missing FDE.
int LibunwindContext::FindProcInfo(unw_addr_space_t as,
                                   unw_word_t ip,
                                   unw_proc_info_t* pi,
                                   int need_unwind_info,
                                   void* arg) {
  MapInfo* map = FindMap(ip);
  if (!map)
    return -UNW_ENOINFO;

  MappedFile* mapped = GetOrMapDso(map->map_name);
  if (!mapped)
    return -UNW_ENOINFO;

  if (!mapped->unwind_table_parsed || mapped->table_segbase != map->map_start ||
      mapped->table_mapoff != map->map_offset) {
    if (mapped->edi.di_debug.format != -1 &&
        mapped->edi.di_debug.u.ti.table_data) {
      free(mapped->edi.di_debug.u.ti.table_data);
    }
    mapped->edi = {};
    mapped->edi.image = mapped->data;
    mapped->edi.size = mapped->size;
    mapped->edi.di_cache.format = -1;
    mapped->edi.di_debug.format = -1;
    dwarf_find_unwind_table(&mapped->edi, as, map->map_name.c_str(),
                            map->map_start, map->map_offset, ip);
    mapped->table_segbase = map->map_start;
    mapped->table_mapoff = map->map_offset;
    mapped->unwind_table_parsed = true;
  }

  if (mapped->edi.di_cache.format != -1 &&
      ip >= mapped->edi.di_cache.start_ip && ip < mapped->edi.di_cache.end_ip) {
    int ret = dwarf_search_unwind_table(as, ip, &mapped->edi.di_cache, pi,
                                        need_unwind_info, arg);
    if (ret != -UNW_ENOINFO)
      return ret;
  }
  if (mapped->edi.di_debug.format != -1 &&
      ip >= mapped->edi.di_debug.start_ip && ip < mapped->edi.di_debug.end_ip) {
    return dwarf_search_unwind_table(as, ip, &mapped->edi.di_debug, pi,
                                     need_unwind_info, arg);
  }
  return -UNW_ENOINFO;
}

// Mimics `access_dso_mem` in Linux `perf`
// (`tools/perf/util/unwind-libunwind.c`).
//
// `libunwind`'s `unw_step()` calls `unw_is_signal_frame()` on every frame,
// which invokes `AccessMem(ip)` and `AccessMem(ip + 8)` (2 reads in `.text` per
// frame) to check for the `rt_sigreturn` syscall instruction sequence, and
// `dwarf_search_unwind_table` reads `.eh_frame_hdr` and `.eh_frame` entries via
// `AccessMem`. Serving non-stack reads directly from a cached `mmap` of the
// backing ELF binary eliminates `pread64(mem_fd)` syscalls.
bool LibunwindContext::AccessDsoMem(uint64_t addr, unw_word_t* valp) {
  const MapInfo* map = FindMap(addr);
  if (!map)
    return false;

  const MappedFile* mapped = GetOrMapDso(map->map_name);
  if (!mapped)
    return false;

  uint64_t file_offset = addr - map->map_start + map->map_offset;
  if (file_offset + sizeof(unw_word_t) > mapped->size)
    return false;

  memcpy(valp, static_cast<const uint8_t*>(mapped->data) + file_offset,
         sizeof(unw_word_t));
  return true;
}

FrameData LibunwindContext::BuildFrameFromPc(uint64_t pc, bool resolve_names) {
  FrameData frame{};
  frame.pc = pc;
  frame.rel_pc = pc;

  MapInfo* map_info = FindMap(pc);
  if (map_info == nullptr) {
    return frame;
  }

  MappedFile* mapped = GetOrMapDso(map_info->map_name);
  if (mapped) {
    map_info->map_load_bias = mapped->load_bias;
  }

  frame.rel_pc =
      pc - map_info->map_start + map_info->map_offset + map_info->map_load_bias;
  frame.map_info = *map_info;
  frame.symbol_status =
      resolve_names ? SymbolStatus::kFailure : SymbolStatus::kNotAttempted;

  // Resolve function name and offset by binary-searching the cached, sorted
  // `.symtab`/`.dynsym` table of the mapped DSO (mimicking
  // `libunwindstack::Symbols` and Linux `perf`'s `dso__load_sym`), avoiding
  // `_UPT_get_proc_name` which re-opens `/proc/<pid>/maps`, `mmap`s the ELF,
  // and linearly scans the symbol table on every frame.
  if (resolve_names && mapped) {
    const auto& symbols = mapped->GetSymbols();
    auto it = std::upper_bound(
        symbols.begin(), symbols.end(), frame.rel_pc,
        [](uint64_t val, const ElfSymbol& s) { return val < s.addr; });
    if (it != symbols.begin()) {
      --it;
      if (frame.rel_pc >= it->addr && frame.rel_pc < it->addr + it->size) {
        frame.function_name = it->name;
        frame.function_offset = frame.rel_pc - it->addr;
        frame.symbol_status = SymbolStatus::kResolved;
      }
    }
  }

  return frame;
}

UnwindResult LibunwindContext::Unwind(const UnwindInputSample& sample,
                                      const UnwindOptions& options) {
  UnwindResult result;
  if (!sample.regs || sample.regs->arch != CpuArch::kX86_64 || !addr_space_) {
    result.error_code = UnwindErrorCode::kBadArch;
    return result;
  }

  for (int attempt = 0; attempt < options.max_attempts; ++attempt) {
    if (attempt > 0) {
      constexpr base::TimeMillis kMapsReparseInterval{500};
      if (last_maps_reparse_time_ + kMapsReparseInterval >
          base::GetWallTimeMs()) {
        break;
      }
      ReparseMaps();
      last_maps_reparse_time_ = base::GetWallTimeMs();
    }

    result.frames.clear();
    result.error_code = UnwindErrorCode::kNone;

    CursorState state{this, &sample, *mem_fd_};
    unw_cursor_t cursor;
    if (unw_init_remote(&cursor, addr_space_, &state) < 0) {
      result.error_code = UnwindErrorCode::kInvalidUnwindInfo;
      return result;
    }

    bool skipping_initial_maps = !options.skip_initial_maps.empty();
    bool is_first_frame = true;
    bool prev_was_signal_frame = false;
    unw_word_t prev_ip = 0;
    unw_word_t prev_sp = 0;

    while (result.frames.size() < options.max_frames) {
      unw_word_t ip = 0;
      unw_word_t sp = 0;

      if (unw_get_reg(&cursor, UNW_REG_IP, &ip) < 0 || ip == 0)
        break;
      unw_get_reg(&cursor, UNW_REG_SP, &sp);

      if (!is_first_frame && ip == prev_ip && sp == prev_sp) {
        result.error_code = UnwindErrorCode::kRepeatedFrame;
        break;
      }
      prev_ip = ip;
      prev_sp = sp;

      // Subtract 1 from caller return addresses (non-first, non-signal frames)
      // so the PC points inside the `call` instruction rather than the
      // following instruction (matching `libunwindstack` and Linux `perf`).
      const uint64_t frame_pc =
          (!is_first_frame && !prev_was_signal_frame && ip > 0) ? ip - 1 : ip;

      FrameData frame = BuildFrameFromPc(frame_pc, options.resolve_names);
      frame.sp = sp;
      if (!frame.map_info) {
        result.error_code = UnwindErrorCode::kInvalidMap;
        if (!skipping_initial_maps) {
          result.frames.push_back(std::move(frame));
        }
        break;
      }

      bool ignore_frame = false;
      if (skipping_initial_maps) {
        std::string basename = base::Basename(frame.map_info->map_name);
        ignore_frame = std::find(options.skip_initial_maps.begin(),
                                 options.skip_initial_maps.end(),
                                 basename) != options.skip_initial_maps.end();
        if (!ignore_frame) {
          skipping_initial_maps = false;
        }
      }
      if (!ignore_frame) {
        result.frames.push_back(std::move(frame));
      }

      prev_was_signal_frame = unw_is_signal_frame(&cursor) > 0;
      is_first_frame = false;

      int step = unw_step(&cursor);
      if (step <= 0) {
        break;
      }
      if (result.frames.size() >= options.max_frames) {
        result.error_code = UnwindErrorCode::kMaxFramesExceeded;
        break;
      }
    }

    if (result.error_code != UnwindErrorCode::kInvalidMap) {
      break;
    }
  }
  return result;
}

}  // namespace profiling
}  // namespace perfetto
