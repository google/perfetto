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

#include <elf.h>
#include <string.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace perfetto {
namespace profiling {
namespace {

bool IsElf(const char* mem, size_t size) {
  return size >= SELFMAG && memcmp(mem, ELFMAG, SELFMAG) == 0;
}

// Mirroring `Symbols::BuildRemapTable` from `libunwindstack/Symbols.cpp`
// and `dso__load_sym` from Linux `perf` (`tools/perf/util/symbol-elf.c`).
template <typename Ehdr, typename Shdr, typename Sym>
std::vector<ElfSymbol> ParseElfSymbols(const uint8_t* bytes, size_t size) {
  std::vector<ElfSymbol> symbols;
  if (size < sizeof(Ehdr))
    return symbols;

  Ehdr ehdr;
  memcpy(&ehdr, bytes, sizeof(ehdr));
  const size_t shdr_table_size =
      static_cast<size_t>(ehdr.e_shnum) * sizeof(Shdr);
  if (ehdr.e_shoff == 0 || shdr_table_size == 0 ||
      ehdr.e_shoff + shdr_table_size > size) {
    return symbols;
  }

  for (size_t i = 0; i < ehdr.e_shnum; ++i) {
    Shdr shdr;
    memcpy(&shdr, bytes + ehdr.e_shoff + i * sizeof(Shdr), sizeof(shdr));
    if (shdr.sh_type != SHT_SYMTAB && shdr.sh_type != SHT_DYNSYM)
      continue;
    if (shdr.sh_link >= ehdr.e_shnum || shdr.sh_entsize < sizeof(Sym) ||
        shdr.sh_offset + shdr.sh_size > size) {
      continue;
    }

    Shdr str_shdr;
    memcpy(&str_shdr, bytes + ehdr.e_shoff + shdr.sh_link * sizeof(Shdr),
           sizeof(str_shdr));
    if (str_shdr.sh_type != SHT_STRTAB || str_shdr.sh_size == 0 ||
        str_shdr.sh_offset + str_shdr.sh_size > size) {
      continue;
    }

    const char* strtab =
        reinterpret_cast<const char*>(bytes + str_shdr.sh_offset);
    if (strtab[str_shdr.sh_size - 1] != '\0')
      continue;

    const size_t count = static_cast<size_t>(shdr.sh_size / shdr.sh_entsize);
    for (size_t j = 0; j < count; ++j) {
      Sym sym;
      memcpy(&sym, bytes + shdr.sh_offset + j * shdr.sh_entsize, sizeof(sym));
      if (sym.st_shndx != SHN_UNDEF && ELF32_ST_TYPE(sym.st_info) == STT_FUNC &&
          sym.st_size != 0 && sym.st_name < str_shdr.sh_size) {
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

// returns `VirtAddr - offset` from the executable (R E) load segment
// $ readelf -lW /usr/lib/x86_64-linux-gnu/libunwind.so
// Type    Offset   VirtAddr           PhysAddr         FileSiz  MemSiz  Flg...
// LOAD 0x003000 0x0000000000003000 0x0000000000003000 0x00a92e 0x00a92e R E...
//
template <typename Ehdr, typename Phdr>
uint64_t GetElfLoadBias(const uint8_t* bytes, size_t size) {
  if (size < sizeof(Ehdr))
    return 0;
  Ehdr ehdr;
  memcpy(&ehdr, bytes, sizeof(ehdr));
  const size_t phdr_table_size =
      static_cast<size_t>(ehdr.e_phnum) * sizeof(Phdr);
  if (ehdr.e_phoff == 0 || phdr_table_size == 0 ||
      ehdr.e_phoff + phdr_table_size > size) {
    return 0;
  }
  for (size_t i = 0; i < ehdr.e_phnum; ++i) {
    Phdr phdr;
    memcpy(&phdr, bytes + ehdr.e_phoff + i * sizeof(Phdr), sizeof(phdr));
    if (phdr.p_type == PT_LOAD && (phdr.p_flags & PF_X)) {
      return phdr.p_vaddr - phdr.p_offset;
    }
  }
  return 0;
}

}  // namespace

// Mirroring `GetBinaryInfoFromFd` and `GetBinaryInfo` from
// `src/trace_processor/util/symbolizer/local_symbolizer.cc`.
uint64_t GetElfLoadBias(const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  if (!data || !IsElf(reinterpret_cast<const char*>(bytes), size) ||
      size <= EI_CLASS) {
    return 0;
  }
  switch (bytes[EI_CLASS]) {
    case ELFCLASS32:
      return GetElfLoadBias<Elf32_Ehdr, Elf32_Phdr>(bytes, size);
    case ELFCLASS64:
      return GetElfLoadBias<Elf64_Ehdr, Elf64_Phdr>(bytes, size);
    default:
      return 0;
  }
}

std::vector<ElfSymbol> ParseElfSymbols(const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  if (!data || !IsElf(reinterpret_cast<const char*>(bytes), size) ||
      size <= EI_CLASS) {
    return {};
  }
  switch (bytes[EI_CLASS]) {
    case ELFCLASS32:
      return ParseElfSymbols<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym>(bytes, size);
    case ELFCLASS64:
      return ParseElfSymbols<Elf64_Ehdr, Elf64_Shdr, Elf64_Sym>(bytes, size);
    default:
      return {};
  }
}

}  // namespace profiling
}  // namespace perfetto
