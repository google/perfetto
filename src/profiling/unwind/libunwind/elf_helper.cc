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

#include "src/profiling/unwind/libunwind/elf_helper.h"

#include <gelf.h>
#include <libelf.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace perfetto {
namespace profiling {
namespace {

// RAII wrapper around libelf's `Elf*` handle opened over an in-memory buffer.
class ScopedElf {
 public:
  ScopedElf(const void* data, size_t size) {
    if (!data || size == 0 || elf_version(EV_CURRENT) == EV_NONE)
      return;
    // `elf_memory()` takes a non-const `char*` due to the legacy SVR4 C API,
    // but only reads the buffer when used with read-only inspection APIs.
    Elf* elf =
        elf_memory(const_cast<char*>(static_cast<const char*>(data)), size);
    if (elf && elf_kind(elf) == ELF_K_ELF) {
      elf_ = elf;
    } else if (elf) {
      elf_end(elf);
    }
  }

  ~ScopedElf() {
    if (elf_)
      elf_end(elf_);
  }

  ScopedElf(const ScopedElf&) = delete;
  ScopedElf& operator=(const ScopedElf&) = delete;

  Elf* get() const { return elf_; }
  explicit operator bool() const { return elf_ != nullptr; }

 private:
  Elf* elf_ = nullptr;
};

}  // namespace

// Returns `p_vaddr - p_offset` from the first executable (`PF_X`) `PT_LOAD`
// program header (supports both ELF32 and ELF64 via `gelf`), matching
// `src/trace_processor/util/symbolizer/local_symbolizer.cc`.
// (Linux `perf` computes the same `p_vaddr - p_offset` in `dso__load_sym`
// via `gelf_getphdr`, subtracted per symbol from `sym.st_value`.)
uint64_t GetElfLoadBias(const void* data, size_t size) {
  ScopedElf elf(data, size);
  if (!elf)
    return 0;

  size_t phnum = 0;
  if (elf_getphdrnum(elf.get(), &phnum) != 0)
    return 0;

  for (size_t i = 0; i < phnum; ++i) {
    if (i > static_cast<size_t>(std::numeric_limits<int>::max()))
      break;
    GElf_Phdr phdr;
    if (!gelf_getphdr(elf.get(), static_cast<int>(i), &phdr))
      continue;
    if (phdr.p_type == PT_LOAD && (phdr.p_flags & PF_X))
      return phdr.p_vaddr - phdr.p_offset;
  }
  return 0;
}

// Parses and sorts function symbols (`STT_FUNC`) from `.symtab` and `.dynsym`
// via `libelf`/`gelf`, supporting both ELF32 and ELF64. Mirrors Linux `perf`'s
// `elf_symtab__for_each_symbol` / `gelf_getsym` loop in
// `tools/perf/util/symbol-elf.c` (`dso__load_sym`).
std::vector<ElfSymbol> ParseElfSymbols(const void* data, size_t size) {
  std::vector<ElfSymbol> symbols;
  ScopedElf elf(data, size);
  if (!elf)
    return symbols;

  const size_t sym_entsize = gelf_fsize(elf.get(), ELF_T_SYM, 1, EV_CURRENT);
  if (sym_entsize == 0)
    return symbols;

  const uintptr_t base_addr = reinterpret_cast<uintptr_t>(data);
  const uintptr_t end_addr = base_addr + size;

  Elf_Scn* scn = nullptr;
  while ((scn = elf_nextscn(elf.get(), scn)) != nullptr) {
    GElf_Shdr shdr;
    if (!gelf_getshdr(scn, &shdr))
      continue;
    if (shdr.sh_type != SHT_SYMTAB && shdr.sh_type != SHT_DYNSYM)
      continue;

    // Use `elf_rawdata()` for the linked string table so returned `name`
    // pointers reference `[data, data + size)` directly and remain valid after
    // `elf_end()` (portable across both elfutils and BSD elftoolchain).
    Elf_Scn* str_scn = elf_getscn(elf.get(), shdr.sh_link);
    GElf_Shdr str_shdr;
    if (!str_scn || !gelf_getshdr(str_scn, &str_shdr) ||
        str_shdr.sh_type != SHT_STRTAB) {
      continue;
    }
    Elf_Data* str_data = elf_rawdata(str_scn, nullptr);
    if (!str_data || !str_data->d_buf || str_data->d_size == 0)
      continue;
    const uintptr_t str_addr = reinterpret_cast<uintptr_t>(str_data->d_buf);
    if (str_addr < base_addr || str_data->d_size > end_addr - str_addr)
      continue;
    const char* strtab = static_cast<const char*>(str_data->d_buf);
    if (strtab[str_data->d_size - 1] != '\0')
      continue;

    Elf_Data* sym_data = elf_getdata(scn, nullptr);
    if (!sym_data || !sym_data->d_buf || sym_data->d_size == 0)
      continue;

    const size_t count = sym_data->d_size / sym_entsize;
    for (size_t i = 0; i < count; ++i) {
      if (i > static_cast<size_t>(std::numeric_limits<int>::max()))
        break;
      GElf_Sym sym;
      if (!gelf_getsym(sym_data, static_cast<int>(i), &sym))
        continue;
      if (sym.st_shndx != SHN_UNDEF && GELF_ST_TYPE(sym.st_info) == STT_FUNC &&
          sym.st_size != 0 && sym.st_name < str_data->d_size) {
        const char* name = strtab + sym.st_name;
        if (*name != '\0') {
          symbols.push_back(
              {sym.st_value, static_cast<uint32_t>(sym.st_size), name});
        }
      }
    }
  }

  std::sort(symbols.begin(), symbols.end(),
            [](const ElfSymbol& a, const ElfSymbol& b) {
              return a.addr != b.addr ? a.addr < b.addr : a.size > b.size;
            });
  symbols.erase(std::unique(symbols.begin(), symbols.end(),
                            [](const ElfSymbol& a, const ElfSymbol& b) {
                              return a.addr == b.addr;
                            }),
                symbols.end());
  return symbols;
}

}  // namespace profiling
}  // namespace perfetto
