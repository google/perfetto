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

#ifndef SRC_PROFILING_UNWIND_LIBUNWIND_ELF_HELPER_H_
#define SRC_PROFILING_UNWIND_LIBUNWIND_ELF_HELPER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace perfetto {
namespace profiling {

// Represents an entry in the elf symbol table. Example output:
// $ readelf -s /usr/lib/x86_64-linux-gnu/libunwind.so
//   Num:    Value          Size Type    Bind   Vis      Ndx Name
//   78: 0000000000006bb0  1338 FUNC    GLOBAL DEFAULT   11 _ULx86_64_step

struct ElfSymbol {
  uint64_t addr = 0;  // Value column from readelf -s.
  uint32_t size = 0;
  const char* name = nullptr;
};

uint64_t GetElfLoadBias(const void* data, size_t size);

// Parses and sorts function symbols (Type = FUNC) from `.symtab` and `.dynsym`
// in an mmap'd ELF image, mimicking `libunwindstack::Symbols` and Linux
// `perf`'s `dso__load_sym`. Returned `name` pointers reference the string
// table inside `[data, data + size)`.
std::vector<ElfSymbol> ParseElfSymbols(const void* data, size_t size);

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_UNWIND_LIBUNWIND_ELF_HELPER_H_
