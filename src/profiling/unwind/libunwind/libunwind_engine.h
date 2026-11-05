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

#ifndef SRC_PROFILING_UNWIND_LIBUNWIND_LIBUNWIND_ENGINE_H_
#define SRC_PROFILING_UNWIND_LIBUNWIND_LIBUNWIND_ENGINE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <libunwind.h>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/unwind/libunwind/elf_helper.h"
#include "src/profiling/unwind/unwind_engine.h"
#include "src/profiling/unwind/unwind_types.h"

namespace perfetto {
namespace profiling {

// A single /proc/<pid>/maps entry. Shared between the per-process map list
// and the FrameData objects handed out to callers, so that frames stay valid
// after ResetMaps()/ReparseMaps() drops the list.
struct LibunwindMapping : public UnwoundMapping {
  ~LibunwindMapping() override;

  std::string map_name() const override { return name; }
  uint64_t map_start() const override { return start; }
  uint64_t map_end() const override { return end; }
  uint64_t map_offset() const override { return offset; }
  uint64_t map_start_offset() const override { return start_offset; }
  uint64_t map_load_bias() const override { return load_bias; }
  // TODO(safayat): read .note.gnu.build-id from the mmap'd ELF.
  std::string build_id() const override { return ""; }

  std::string name;
  uint64_t start = 0;
  uint64_t end = 0;
  uint64_t offset = 0;
  uint64_t start_offset = 0;
  // Populated lazily from the backing ELF on first lookup.
  uint64_t load_bias = 0;
};

// Per-process state for the libunwind backend: parsed /proc/<pid>/maps, the
// /proc/<pid>/mem fd, the remote unw_addr_space_t (with its per-thread DWARF
// cache) and the cache of mmap'd DSOs.
class LibunwindProcessState : public ProcessUnwindState {
 public:
  LibunwindProcessState(base::ScopedFile maps_fd, base::ScopedFile mem_fd);
  ~LibunwindProcessState() override;

  void ReparseMaps() override;
  void ResetMaps() override;

  // libunwind `find_proc_info` accessor.
  int FindProcInfo(unw_addr_space_t as,
                   unw_word_t ip,
                   unw_proc_info_t* pi,
                   int need_unwind_info,
                   void* arg);

  // Reads a word from the mapped ELF file backing `addr` (mimicking
  // `access_dso_mem` in Linux `perf`). Used by the `access_mem` accessor when
  // `libunwind` reads `.text` instructions (e.g. `unw_is_signal_frame` on
  // every frame) or `.eh_frame_hdr`/`.eh_frame` tables that are not part of
  // the captured stack snapshot.
  bool AccessDsoMem(uint64_t addr, unw_word_t* valp);

  // Returns the mapping containing |pc|, or nullptr.
  std::shared_ptr<LibunwindMapping> FindMap(uint64_t pc);

  // Returns the sorted function symbols of the DSO backing |mapping| (parsed
  // once per DSO), or nullptr if the DSO is not a readable on-disk ELF.
  // Updates |mapping->load_bias| as a side effect.
  const std::vector<ElfSymbol>* GetSymbols(LibunwindMapping* mapping);

  unw_addr_space_t addr_space() const { return addr_space_; }
  int mem_fd() const { return *mem_fd_; }

 private:
  // Matches `struct elf_dyn_info` in libunwind (`include/libunwind_i.h`),
  // populated by `dwarf_find_unwind_table`:
  //   struct elf_image { void* image; size_t size; } ei;
  //   unw_dyn_info_t di_cache;
  //   unw_dyn_info_t di_debug;
  struct ElfDynInfo {
    void* image = nullptr;
    size_t size = 0;
    unw_dyn_info_t di_cache{};
    unw_dyn_info_t di_debug{};
  };
  static_assert(sizeof(ElfDynInfo) ==
                    sizeof(void*) + sizeof(size_t) + 2 * sizeof(unw_dyn_info_t),
                "Must match libunwind struct elf_dyn_info layout");

  struct MappedFile {
    void* data = nullptr;
    size_t size = 0;
    uint64_t load_bias = 0;
    bool unwind_table_parsed = false;
    uint64_t table_segbase = 0;
    uint64_t table_mapoff = 0;
    ElfDynInfo edi{};
    bool symbols_parsed = false;
    std::vector<ElfSymbol> symbols;
  };

  void ParseMaps();
  MappedFile* GetOrMapDso(const std::string& path);

  base::ScopedFile maps_fd_;
  base::ScopedFile mem_fd_;

  std::vector<std::shared_ptr<LibunwindMapping>> maps_;
  // Cache of mmap'd ELF binaries (DSOs) keyed by file path, used by
  // `AccessDsoMem` to serve `.text`/`.eh_frame` memory reads without
  // `pread64(mem_fd)` syscalls, by `FindProcInfo` for `.eh_frame_hdr` table
  // lookup, and by `BuildFrameFromPc` for symbol lookup.
  base::FlatHashMap<std::string, MappedFile> dso_mmaps_;
  unw_addr_space_t addr_space_ = nullptr;
};

class LibunwindEngine : public UnwindEngine {
 public:
  LibunwindEngine();
  ~LibunwindEngine() override;

  FrameData BuildFrameFromPc(ProcessUnwindState* state,
                             uint64_t pc,
                             bool resolve_names = true) override;

  UnwindResult Unwind(ProcessUnwindState* state,
                      const UnwindInputSample& sample,
                      const UnwindOptions& options = {}) override;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_UNWIND_LIBUNWIND_LIBUNWIND_ENGINE_H_
