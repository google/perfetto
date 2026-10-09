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

#ifndef SRC_PROFILING_UNWIND_LIBUNWIND_LIBUNWIND_CONTEXT_H_
#define SRC_PROFILING_UNWIND_LIBUNWIND_LIBUNWIND_CONTEXT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <libunwind-x86_64.h>
#include <libunwind.h>

#include "perfetto/base/time.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/unwind/libunwind/elf_helper.h"
#include "src/profiling/unwind/unwind_context.h"
#include "src/profiling/unwind/unwind_types.h"

namespace perfetto {
namespace profiling {

class LibunwindContext : public UnwindContext {
 public:
  LibunwindContext(base::ScopedFile maps_fd, base::ScopedFile mem_fd);
  ~LibunwindContext() override;

  uint64_t reparses() const override { return reparses_count_; }
  void ReparseMaps() override;
  void ResetMaps() override;

  FrameData BuildFrameFromPc(uint64_t pc, bool resolve_names = true) override;

  UnwindResult Unwind(const UnwindInputSample& /* sample */,
                      const UnwindOptions& /*options*/ = {}) override;

  int FindProcInfo(unw_addr_space_t as,
                   unw_word_t ip,
                   unw_proc_info_t* pi,
                   int need_unwind_info,
                   void* arg);

  // Reads a word from the mapped ELF file backing `addr` (mimicking
  // `access_dso_mem` in Linux `perf`). Used by `AccessMem` when `libunwind`
  // reads `.text` instructions (e.g. `unw_is_signal_frame` on every frame)
  // or `.eh_frame_hdr`/`.eh_frame` tables that are not part of the captured
  // stack snapshot.
  bool AccessDsoMem(uint64_t addr, unw_word_t* valp);

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

    const std::vector<ElfSymbol>& GetSymbols() {
      if (!symbols_parsed) {
        symbols = ParseElfSymbols(data, size);
        symbols_parsed = true;
      }
      return symbols;
    }
  };

  void ParseMaps();
  MapInfo* FindMap(uint64_t pc);
  MappedFile* GetOrMapDso(const std::string& path);

  base::ScopedFile maps_fd_;
  base::ScopedFile mem_fd_;

  uint64_t reparses_count_ = 0;
  base::TimeMillis last_maps_reparse_time_{0};
  std::vector<MapInfo> maps_;
  // Cache of mmap'd ELF binaries (DSOs) keyed by file path, used by
  // `AccessDsoMem` to serve `.text`/`.eh_frame` memory reads without
  // `pread64(mem_fd)` syscalls, by `FindProcInfo` for `.eh_frame_hdr` table
  // lookup, and by `BuildFrameFromPc` for symbol lookup.
  base::FlatHashMap<std::string, MappedFile> dso_mmaps_;
  unw_addr_space_t addr_space_ = nullptr;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_UNWIND_LIBUNWIND_LIBUNWIND_CONTEXT_H_
