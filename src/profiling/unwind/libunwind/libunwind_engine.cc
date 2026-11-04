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

#include "src/profiling/unwind/libunwind/libunwind_engine.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/no_destructor.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/unwind/cpu_registers.h"
#include "src/profiling/unwind/libunwind/elf_helper.h"
#include "src/profiling/unwind/libunwind/process_map.h"
#include "src/profiling/unwind/unwind_engine.h"
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

// libunwind's UNW_X86_64_* register numbering follows the DWARF (System V
// ABI) ordering, as does X86_64Reg in cpu_registers.h. Pin that down so the
// `access_reg` accessor can index CpuRegisters directly.
// TODO(safayat): Add 32-bit x86 (__i386__ / UNW_X86_*) support if needed.
constexpr bool RegEq(int unw, int ours) {
  return unw == ours;
}
static_assert(RegEq(UNW_X86_64_RAX, X86_64_REG_RAX) &&
                  RegEq(UNW_X86_64_RDX, X86_64_REG_RDX) &&
                  RegEq(UNW_X86_64_RCX, X86_64_REG_RCX) &&
                  RegEq(UNW_X86_64_RBX, X86_64_REG_RBX) &&
                  RegEq(UNW_X86_64_RSI, X86_64_REG_RSI) &&
                  RegEq(UNW_X86_64_RDI, X86_64_REG_RDI) &&
                  RegEq(UNW_X86_64_RBP, X86_64_REG_RBP) &&
                  RegEq(UNW_X86_64_RSP, X86_64_REG_RSP) &&
                  RegEq(UNW_X86_64_R8, X86_64_REG_R8) &&
                  RegEq(UNW_X86_64_R15, X86_64_REG_R15) &&
                  RegEq(UNW_X86_64_RIP, X86_64_REG_RIP),
              "libunwind x86_64 register numbering must match X86_64Reg");

struct CursorState {
  LibunwindProcessState* state = nullptr;
  const UnwindInputSample* sample = nullptr;
  int mem_fd = -1;
};

int FindProcInfo(unw_addr_space_t as,
                 unw_word_t ip,
                 unw_proc_info_t* pi,
                 int need_unwind_info,
                 void* arg) {
  auto* cursor = static_cast<CursorState*>(arg);
  return cursor->state->FindProcInfo(as, ip, pi, need_unwind_info, arg);
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
  auto* cursor = static_cast<CursorState*>(arg);
  const auto* s = cursor->sample;
  // 1. Read from captured stack snapshot if within [sp, sp + stack_size)
  if (s->stack_data && s->stack_size > 0 && s->regs) {
    uint64_t sp = s->regs->sp();
    if (addr >= sp && addr + sizeof(unw_word_t) <= sp + s->stack_size) {
      memcpy(valp, s->stack_data + (addr - sp), sizeof(unw_word_t));
      return 0;
    }
  }
  // 2. Read from cached ELF mmap (mimicking Linux perf's `access_dso_mem`)
  if (cursor->state && cursor->state->AccessDsoMem(addr, valp)) {
    return 0;
  }
  // 3. Fall back to /proc/<pid>/mem
  if (cursor->mem_fd >= 0 &&
      pread64(cursor->mem_fd, valp, sizeof(unw_word_t),
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
  auto* cursor = static_cast<CursorState*>(arg);
  const CpuRegisters& r = *cursor->sample->regs;
  if (regnum < UNW_X86_64_RAX || regnum > UNW_X86_64_RIP)
    return -UNW_EBADREG;
  *valp = r[static_cast<size_t>(regnum)];
  return 0;
}

int AccessFpReg(unw_addr_space_t /*as*/,
                unw_regnum_t /*regnum*/,
                unw_fpreg_t* /*valp*/,
                int /*write*/,
                void* /*arg*/) {
  return -UNW_EBADREG;
}

int Resume(unw_addr_space_t /*as*/, unw_cursor_t* /*cu*/, void* /*arg*/) {
  return -UNW_EINVAL;
}

unw_accessors_t* GetAccessors() {
  static unw_accessors_t accessors = [] {
    unw_accessors_t a{};
    a.find_proc_info = &FindProcInfo;
    a.put_unwind_info = &PutUnwindInfo;
    a.get_dyn_info_list_addr = &GetDynInfoListAddr;
    a.access_mem = &AccessMem;
    a.access_reg = &AccessReg;
    a.access_fpreg = &AccessFpReg;
    a.resume = &Resume;
    a.get_proc_name = &GetProcName;
    return a;
  }();
  return &accessors;
}

// |state| is always created by ProcessUnwindState::Create() in this
// translation unit, so the downcast is a closed-world invariant.
LibunwindProcessState* StateOf(ProcessUnwindState* state) {
  PERFETTO_DCHECK(state != nullptr);
  return static_cast<LibunwindProcessState*>(state);
}

}  // namespace

LibunwindMapping::~LibunwindMapping() = default;

// ---------------------------------------------------------------------------
// LibunwindProcessState
// ---------------------------------------------------------------------------

LibunwindProcessState::LibunwindProcessState(base::ScopedFile maps_fd,
                                             base::ScopedFile mem_fd)
    : maps_fd_(std::move(maps_fd)), mem_fd_(std::move(mem_fd)) {
  ParseMaps();
  addr_space_ = unw_create_addr_space(GetAccessors(), 0);
  // Enable DWARF frame-state caching across unwinds (defaults to
  // UNW_CACHE_NONE).
  // TODO(safayat): set cache size limit
  unw_set_caching_policy(addr_space_, UNW_CACHE_PER_THREAD);
}

LibunwindProcessState::~LibunwindProcessState() {
  ResetMaps();
  if (addr_space_)
    unw_destroy_addr_space(addr_space_);
}

std::unique_ptr<ProcessUnwindState> ProcessUnwindState::Create(
    base::ScopedFile maps_fd,
    base::ScopedFile mem_fd) {
  return std::make_unique<LibunwindProcessState>(std::move(maps_fd),
                                                 std::move(mem_fd));
}

void LibunwindProcessState::ReparseMaps() {
  ResetMaps();
  ParseMaps();
}

// Drops the parsed maps, the mmap'd DSOs (and their parsed unwind tables /
// symbols) and flushes libunwind's DWARF cache for this address space.
// Outstanding FrameData objects keep their LibunwindMapping alive via
// shared_ptr.
void LibunwindProcessState::ResetMaps() {
  maps_.clear();
  for (auto it = dso_mmaps_.GetIterator(); it; ++it) {
    if (it.value().edi.di_debug.format != -1 &&
        it.value().edi.di_debug.u.ti.table_data) {
      free(it.value().edi.di_debug.u.ti.table_data);
    }
    if (it.value().data)
      munmap(it.value().data, it.value().size);
  }
  dso_mmaps_.Clear();
  if (addr_space_)
    unw_flush_cache(addr_space_, 0, 0);
}

void LibunwindProcessState::ParseMaps() {
  if (!maps_fd_ || lseek(*maps_fd_, 0, SEEK_SET) == -1) {
    return;
  }

  std::string content;
  if (!base::ReadFileDescriptor(*maps_fd_, &content))
    return;

  ParseProcMaps(content, [&](const ProcMapsEntry& entry) {
    auto map = std::make_shared<LibunwindMapping>();
    map->start = entry.start;
    map->end = entry.end;
    map->offset = entry.pgoff;
    map->start_offset = entry.pgoff;
    map->name = entry.name;

    // For split ELFs (r--p header map followed by r-xp text map of the
    // same file), the start offset is the one of the first mapping.
    if ((entry.flags & PROT_EXEC) && !maps_.empty() &&
        maps_.back()->name == entry.name) {
      map->start_offset = maps_.back()->start_offset;
    }
    maps_.push_back(std::move(map));
  });
}

// Mirroring `Maps::Find` from `libunwindstack/Maps.cpp` (and `maps__find` in
// Linux `perf`).
std::shared_ptr<LibunwindMapping> LibunwindProcessState::FindMap(uint64_t pc) {
  auto it = std::upper_bound(
      maps_.begin(), maps_.end(), pc,
      [](uint64_t val, const std::shared_ptr<LibunwindMapping>& m) {
        return val < m->start;
      });
  if (it != maps_.begin()) {
    --it;
    if (pc >= (*it)->start && pc < (*it)->end) {
      return *it;
    }
  }
  return nullptr;
}

LibunwindProcessState::MappedFile* LibunwindProcessState::GetOrMapDso(
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

const std::vector<ElfSymbol>* LibunwindProcessState::GetSymbols(
    LibunwindMapping* mapping) {
  MappedFile* mapped = GetOrMapDso(mapping->name);
  if (!mapped)
    return nullptr;
  mapping->load_bias = mapped->load_bias;
  if (!mapped->symbols_parsed) {
    mapped->symbols = ParseElfSymbols(mapped->data, mapped->size);
    mapped->symbols_parsed = true;
  }
  return &mapped->symbols;
}

// Uses `libunwind`'s `dwarf_find_unwind_table` (to parse `.eh_frame_hdr`,
// `.debug_frame`, and `.gnu_debugdata` once per mapped DSO) and
// `dwarf_search_unwind_table` (to binary-search the table), avoiding
// `_UPT_find_proc_info` which only caches 1 ELF at a time and re-reads
// `/proc/<pid>/maps` on every DSO transition or missing FDE.
int LibunwindProcessState::FindProcInfo(unw_addr_space_t as,
                                        unw_word_t ip,
                                        unw_proc_info_t* pi,
                                        int need_unwind_info,
                                        void* arg) {
  std::shared_ptr<LibunwindMapping> map = FindMap(ip);
  if (!map)
    return -UNW_ENOINFO;

  MappedFile* mapped = GetOrMapDso(map->name);
  if (!mapped)
    return -UNW_ENOINFO;

  if (!mapped->unwind_table_parsed || mapped->table_segbase != map->start ||
      mapped->table_mapoff != map->offset) {
    if (mapped->edi.di_debug.format != -1 &&
        mapped->edi.di_debug.u.ti.table_data) {
      free(mapped->edi.di_debug.u.ti.table_data);
    }
    mapped->edi = {};
    mapped->edi.image = mapped->data;
    mapped->edi.size = mapped->size;
    mapped->edi.di_cache.format = -1;
    mapped->edi.di_debug.format = -1;
    dwarf_find_unwind_table(&mapped->edi, as, map->name.c_str(), map->start,
                            map->offset, ip);
    mapped->table_segbase = map->start;
    mapped->table_mapoff = map->offset;
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
bool LibunwindProcessState::AccessDsoMem(uint64_t addr, unw_word_t* valp) {
  std::shared_ptr<LibunwindMapping> map = FindMap(addr);
  if (!map)
    return false;

  const MappedFile* mapped = GetOrMapDso(map->name);
  if (!mapped)
    return false;

  uint64_t file_offset = addr - map->start + map->offset;
  if (file_offset + sizeof(unw_word_t) > mapped->size)
    return false;

  memcpy(valp, static_cast<const uint8_t*>(mapped->data) + file_offset,
         sizeof(unw_word_t));
  return true;
}

// ---------------------------------------------------------------------------
// LibunwindEngine
// ---------------------------------------------------------------------------

LibunwindEngine::LibunwindEngine() = default;
LibunwindEngine::~LibunwindEngine() = default;

UnwindEngine* UnwindEngine::Get() {
  // NoDestructor: the engine has a virtual dtor, so a plain function-local
  // static would require an exit-time destructor (-Wexit-time-destructors).
  static base::NoDestructor<LibunwindEngine> engine;
  return &engine.ref();
}

FrameData LibunwindEngine::BuildFrameFromPc(ProcessUnwindState* raw_state,
                                            uint64_t pc,
                                            bool resolve_names) {
  LibunwindProcessState* state = StateOf(raw_state);
  FrameData frame{};
  frame.pc = pc;
  frame.rel_pc = pc;

  std::shared_ptr<LibunwindMapping> map_info = state->FindMap(pc);
  if (map_info == nullptr) {
    return frame;
  }

  // Also refreshes map_info->load_bias from the backing ELF.
  const std::vector<ElfSymbol>* symbols = state->GetSymbols(map_info.get());

  frame.rel_pc = pc - map_info->start + map_info->offset + map_info->load_bias;
  frame.symbol_status =
      resolve_names ? SymbolStatus::kFailure : SymbolStatus::kNotAttempted;

  // Resolve function name and offset by binary-searching the cached, sorted
  // `.symtab`/`.dynsym` table of the mapped DSO (mimicking
  // `libunwindstack::Symbols` and Linux `perf`'s `dso__load_sym`), avoiding
  // `_UPT_get_proc_name` which re-opens `/proc/<pid>/maps`, `mmap`s the ELF,
  // and linearly scans the symbol table on every frame.
  if (resolve_names && symbols) {
    auto it = std::upper_bound(
        symbols->begin(), symbols->end(), frame.rel_pc,
        [](uint64_t val, const ElfSymbol& s) { return val < s.addr; });
    if (it != symbols->begin()) {
      --it;
      if (frame.rel_pc >= it->addr && frame.rel_pc < it->addr + it->size) {
        frame.function_name = it->name;
        frame.function_offset = frame.rel_pc - it->addr;
        frame.symbol_status = SymbolStatus::kResolved;
      }
    }
  }

  frame.map_info = std::move(map_info);
  return frame;
}

UnwindResult LibunwindEngine::Unwind(ProcessUnwindState* raw_state,
                                     const UnwindInputSample& sample,
                                     const UnwindOptions& options) {
  LibunwindProcessState* state = StateOf(raw_state);
  UnwindResult result;
  if (!sample.regs || sample.regs->arch != CpuArch::X86_64 ||
      !state->addr_space()) {
    result.error_code = UnwindErrorCode::kBadArch;
    return result;
  }

  CursorState cursor_state{state, &sample, state->mem_fd()};
  unw_cursor_t cursor;
  if (unw_init_remote(&cursor, state->addr_space(), &cursor_state) < 0) {
    result.error_code = UnwindErrorCode::kInvalidUnwindInfo;
    return result;
  }

  bool skipping_initial_maps =
      options.skip_initial_maps && !options.skip_initial_maps->empty();
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

    FrameData frame = BuildFrameFromPc(state, frame_pc, options.resolve_names);
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
      std::string basename = base::Basename(frame.map_info->map_name());
      const auto& skip = *options.skip_initial_maps;
      ignore_frame =
          std::find(skip.begin(), skip.end(), basename) != skip.end();
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
  return result;
}

}  // namespace profiling
}  // namespace perfetto
