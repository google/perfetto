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
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

TEST(ElfHelperTest, GetElfLoadBiasInvalidInputs) {
  EXPECT_EQ(GetElfLoadBias(nullptr, 0), 0u);
  const char not_elf[] = "NOT_AN_ELF_BINARY_HEADER";
  EXPECT_EQ(GetElfLoadBias(not_elf, sizeof(not_elf)), 0u);

  Elf64_Ehdr truncated_ehdr{};
  memcpy(truncated_ehdr.e_ident, ELFMAG, SELFMAG);
  truncated_ehdr.e_ident[EI_CLASS] = ELFCLASS64;
  truncated_ehdr.e_phoff = 4096;  // Out of bounds.
  truncated_ehdr.e_phnum = 2;
  EXPECT_EQ(GetElfLoadBias(&truncated_ehdr, sizeof(truncated_ehdr)), 0u);
}

TEST(ElfHelperTest, GetElfLoadBiasSynthetic64) {
  struct SyntheticElf64 {
    Elf64_Ehdr ehdr;
    Elf64_Phdr phdrs[2];
  } image{};

  memcpy(image.ehdr.e_ident, ELFMAG, SELFMAG);
  image.ehdr.e_ident[EI_CLASS] = ELFCLASS64;
  image.ehdr.e_phoff = offsetof(SyntheticElf64, phdrs);
  image.ehdr.e_phnum = 2;

  // Non-executable PT_LOAD segment (should be skipped).
  image.phdrs[0].p_type = PT_LOAD;
  image.phdrs[0].p_flags = PF_R;
  image.phdrs[0].p_offset = 0x0;
  image.phdrs[0].p_vaddr = 0x100000;

  // Executable PT_LOAD segment: load_bias = p_vaddr - p_offset = 0x400000.
  image.phdrs[1].p_type = PT_LOAD;
  image.phdrs[1].p_flags = PF_R | PF_X;
  image.phdrs[1].p_offset = 0x1000;
  image.phdrs[1].p_vaddr = 0x401000;

  EXPECT_EQ(GetElfLoadBias(&image, sizeof(image)), 0x400000u);
}

TEST(ElfHelperTest, ParseElfSymbolsInvalidInputs) {
  EXPECT_TRUE(ParseElfSymbols(nullptr, 0).empty());
  const char not_elf[] = "NOT_AN_ELF_BINARY_HEADER";
  EXPECT_TRUE(ParseElfSymbols(not_elf, sizeof(not_elf)).empty());

  Elf64_Ehdr truncated_ehdr{};
  memcpy(truncated_ehdr.e_ident, ELFMAG, SELFMAG);
  truncated_ehdr.e_ident[EI_CLASS] = ELFCLASS64;
  truncated_ehdr.e_shoff = 4096;  // Out of bounds.
  truncated_ehdr.e_shnum = 3;
  EXPECT_TRUE(ParseElfSymbols(&truncated_ehdr, sizeof(truncated_ehdr)).empty());
}

TEST(ElfHelperTest, ParseElfSymbolsSyntheticFilteringAndSorting) {
  // String table:
  // offset 0: ""
  // offset 1: "undef_fn"
  // offset 10: "global_var"
  // offset 21: "func_b"
  // offset 28: "func_a"
  // offset 35: "func_a_alias"
  static constexpr char kStrTab[] =
      "\0undef_fn\0global_var\0func_b\0func_a\0func_a_alias";

  struct SyntheticElfSymImage {
    Elf64_Ehdr ehdr;
    Elf64_Shdr shdrs[3];
    Elf64_Sym syms[6];
    char strtab[sizeof(kStrTab)];
  } image{};

  memcpy(image.ehdr.e_ident, ELFMAG, SELFMAG);
  image.ehdr.e_ident[EI_CLASS] = ELFCLASS64;
  image.ehdr.e_shoff = offsetof(SyntheticElfSymImage, shdrs);
  image.ehdr.e_shnum = 3;

  // Section 1: .symtab (linked to section 2 .strtab).
  image.shdrs[1].sh_type = SHT_SYMTAB;
  image.shdrs[1].sh_offset = offsetof(SyntheticElfSymImage, syms);
  image.shdrs[1].sh_size = sizeof(image.syms);
  image.shdrs[1].sh_entsize = sizeof(Elf64_Sym);
  image.shdrs[1].sh_link = 2;

  // Section 2: .strtab.
  image.shdrs[2].sh_type = SHT_STRTAB;
  image.shdrs[2].sh_offset = offsetof(SyntheticElfSymImage, strtab);
  image.shdrs[2].sh_size = sizeof(kStrTab);
  memcpy(image.strtab, kStrTab, sizeof(kStrTab));

  // Sym 0: Null symbol -> skipped.
  image.syms[0] = {};

  // Sym 1: Undefined function (SHN_UNDEF) -> skipped.
  image.syms[1].st_name = 1;  // "undef_fn"
  image.syms[1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
  image.syms[1].st_shndx = SHN_UNDEF;
  image.syms[1].st_value = 0x1000;
  image.syms[1].st_size = 32;

  // Sym 2: Defined OBJECT (not STT_FUNC) -> skipped.
  image.syms[2].st_name = 10;  // "global_var"
  image.syms[2].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
  image.syms[2].st_shndx = 1;
  image.syms[2].st_value = 0x2000;
  image.syms[2].st_size = 8;

  // Sym 3: Defined function at higher address 0x4000 -> kept and sorted second.
  image.syms[3].st_name = 21;  // "func_b"
  image.syms[3].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
  image.syms[3].st_shndx = 1;
  image.syms[3].st_value = 0x4000;
  image.syms[3].st_size = 64;

  // Sym 4: Defined function at lower address 0x3000 -> kept and sorted first.
  image.syms[4].st_name = 28;  // "func_a"
  image.syms[4].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
  image.syms[4].st_shndx = 1;
  image.syms[4].st_value = 0x3000;
  image.syms[4].st_size = 48;

  // Sym 5: Alias at same address 0x3000 -> deduplicated.
  image.syms[5].st_name = 35;  // "func_a_alias"
  image.syms[5].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
  image.syms[5].st_shndx = 1;
  image.syms[5].st_value = 0x3000;
  image.syms[5].st_size = 48;

  std::vector<ElfSymbol> symbols = ParseElfSymbols(&image, sizeof(image));
  ASSERT_EQ(symbols.size(), 2u);
  EXPECT_EQ(symbols[0].addr, 0x3000u);
  EXPECT_EQ(symbols[0].size, 48u);
  EXPECT_STREQ(symbols[0].name, "func_a");

  EXPECT_EQ(symbols[1].addr, 0x4000u);
  EXPECT_EQ(symbols[1].size, 64u);
  EXPECT_STREQ(symbols[1].name, "func_b");
}

TEST(ElfHelperTest, ParseElfSymbolsSelfExe) {
  base::ScopedFile fd(base::OpenFile("/proc/self/exe", O_RDONLY));
  ASSERT_TRUE(fd);
  struct stat st{};
  ASSERT_EQ(fstat(*fd, &st), 0);
  ASSERT_GT(st.st_size, 0);

  const size_t size = static_cast<size_t>(st.st_size);
  void* mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, *fd, 0);
  ASSERT_NE(mapped, MAP_FAILED);

  std::vector<ElfSymbol> symbols = ParseElfSymbols(mapped, size);
  EXPECT_FALSE(symbols.empty());

  bool found_parse_elf_symbols = false;
  for (size_t i = 0; i < symbols.size(); ++i) {
    EXPECT_GT(symbols[i].size, 0u);
    ASSERT_NE(symbols[i].name, nullptr);
    if (i > 0) {
      EXPECT_LT(symbols[i - 1].addr, symbols[i].addr);
    }
    if (strstr(symbols[i].name, "ParseElfSymbols") != nullptr) {
      found_parse_elf_symbols = true;
    }
  }
  EXPECT_TRUE(found_parse_elf_symbols);

  munmap(mapped, size);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
