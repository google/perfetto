/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "perfetto/base/build_config.h"
#include "test/gtest_and_gmock.h"

// This translation unit is built only on Linux and MacOS. See //gn/BUILD.gn.
#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)

#include <cstddef>

#include "src/base/test/tmp_dir_tree.h"
#include "src/base/test/utils.h"
#include "src/profiling/symbolizer/elf.h"
#include "src/profiling/symbolizer/local_symbolizer.h"
#include "src/profiling/symbolizer/subprocess.h"

namespace perfetto {
namespace profiling {
namespace {

void RunAndValidateParseLines(std::string raw_contents) {
  std::istringstream stream(raw_contents);
  auto read_callback = [&stream](char* buffer, size_t size) {
    stream.get(buffer, static_cast<int>(size), '\0');
    return strlen(buffer);
  };
  std::vector<std::string> lines = GetLines(read_callback);
  std::istringstream validation(raw_contents);
  for (const std::string& actual : lines) {
    std::string expected;
    getline(validation, expected);
    EXPECT_EQ(actual, expected);
  }
}

TEST(LocalSymbolizerTest, ParseLineWindows) {
  std::string file_name;
  uint32_t lineno;
  ASSERT_TRUE(
      ParseLlvmSymbolizerLine("C:\\Foo\\Bar.cc:123:1", &file_name, &lineno));
  EXPECT_EQ(file_name, "C:\\Foo\\Bar.cc");
  EXPECT_EQ(lineno, 123u);
}

TEST(LocalSymbolizerTest, ParseLinesExpectedOutput) {
  std::string raw_contents =
      "FSlateRHIRenderingPolicy::DrawElements(FRHICommandListImmediate&, "
      "FSlateBackBuffer&, TRefCountPtr<FRHITexture2D>&, "
      "TRefCountPtr<FRHITexture2D>&, TRefCountPtr<FRHITexture2D>&, int, "
      "TArray<FSlateRenderBatch, TSizedDefaultAllocator<32> > const&, "
      "FSlateRenderingParams const&)\n"
      "F:/P4/EngineReleaseA/Engine/Source/Runtime/SlateRHIRenderer/"
      "Private\\SlateRHIRenderingPolicy.cpp:1187:19\n";
  RunAndValidateParseLines(raw_contents);
}

TEST(LocalSymbolizerTest, ParseLinesErrorOutput) {
  std::string raw_contents =
      "LLVMSymbolizer: error reading file: No such file or directory\n"
      "??\n"
      "??:0:0\n";
  RunAndValidateParseLines(raw_contents);
}

TEST(LocalSymbolizerTest, ParseLinesSingleCharRead) {
  std::string raw_contents =
      "FSlateRHIRenderingPolicy::DrawElements(FRHICommandListImmediate&, "
      "FSlateBackBuffer&, TRefCountPtr<FRHITexture2D>&, "
      "TRefCountPtr<FRHITexture2D>&, TRefCountPtr<FRHITexture2D>&, int, "
      "TArray<FSlateRenderBatch, TSizedDefaultAllocator<32> > const&, "
      "FSlateRenderingParams const&)\n"
      "F:/P4/EngineReleaseA/Engine/Source/Runtime/SlateRHIRenderer/"
      "Private\\SlateRHIRenderingPolicy.cpp:1187:19\n";
  std::istringstream stream(raw_contents);
  auto read_callback = [&stream](char* buffer, size_t) {
    stream.get(buffer, 1, '\0');
    return strlen(buffer);
  };
  std::vector<std::string> lines = GetLines(read_callback);
  std::istringstream validation(raw_contents);
  for (const std::string& actual : lines) {
    std::string expected;
    getline(validation, expected);
    EXPECT_EQ(actual, expected);
  }
}

// Creates a very simple ELF file content with the first 20 bytes of `build_id`
// as build id (if build id is shorter the remainin bytes are zero).
std::string CreateElfWithBuildId(const std::string& build_id) {
  struct SimpleElf {
    Elf64::Ehdr ehdr;
    Elf64::Shdr shdr;
    Elf64::Nhdr nhdr;
    char note_name[4];
    char note_desc[20];
  } e;
  memset(&e, 0, sizeof e);

  e.ehdr.e_ident[EI_MAG0] = ELFMAG0;
  e.ehdr.e_ident[EI_MAG1] = ELFMAG1;
  e.ehdr.e_ident[EI_MAG2] = ELFMAG2;
  e.ehdr.e_ident[EI_MAG3] = ELFMAG3;
  e.ehdr.e_ident[EI_CLASS] = ELFCLASS64;
  e.ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
  e.ehdr.e_ident[EI_VERSION] = EV_CURRENT;
  e.ehdr.e_version = EV_CURRENT;
  e.ehdr.e_shentsize = sizeof(Elf64::Shdr);
  e.ehdr.e_shnum = 1;
  e.ehdr.e_ehsize = sizeof e.ehdr;
  e.ehdr.e_shoff = offsetof(SimpleElf, shdr);

  e.shdr.sh_type = SHT_NOTE;
  e.shdr.sh_offset = offsetof(SimpleElf, nhdr);

  e.nhdr.n_type = NT_GNU_BUILD_ID;
  e.nhdr.n_namesz = sizeof e.note_name;
  e.nhdr.n_descsz = sizeof e.note_desc;
  strcpy(e.note_name, "GNU");
  memcpy(e.note_desc, build_id.c_str(),
         std::min(build_id.size(), sizeof(e.note_desc)));

  e.shdr.sh_size = offsetof(SimpleElf, note_desc) + sizeof(e.note_desc) -
                   offsetof(SimpleElf, nhdr);

  return std::string(reinterpret_cast<const char*>(&e), sizeof e);
}

TEST(LocalBinaryIndexerTest, SimpleTree) {
  base::TmpDirTree tmp;
  tmp.AddDir("dir1");
  tmp.AddFile("dir1/elf1", CreateElfWithBuildId("AAAAAAAAAAAAAAAAAAAA"));
  tmp.AddFile("dir1/nonelf1", "OTHERDATA");
  tmp.AddDir("dir2");
  tmp.AddFile("dir2/elf1", CreateElfWithBuildId("BBBBBBBBBBBBBBBBBBBB"));
  tmp.AddFile("dir2/nonelf1", "other text");

  LocalBinaryIndexer indexer({tmp.path() + "/dir1", tmp.path() + "/dir2"});

  base::Optional<FoundBinary> bin1 =
      indexer.FindBinary("", "AAAAAAAAAAAAAAAAAAAA");
  ASSERT_TRUE(bin1.has_value());
  EXPECT_EQ(bin1.value().file_name, tmp.path() + "/dir1/elf1");

  base::Optional<FoundBinary> bin2 =
      indexer.FindBinary("", "BBBBBBBBBBBBBBBBBBBB");
  ASSERT_TRUE(bin2.has_value());
  EXPECT_EQ(bin2.value().file_name, tmp.path() + "/dir2/elf1");
}

TEST(LocalBinaryFinderTest, AbsolutePath) {
  base::TmpDirTree tmp;
  tmp.AddDir("root");
  tmp.AddDir("root/dir");
  tmp.AddFile("root/dir/elf1.so", CreateElfWithBuildId("AAAAAAAAAAAAAAAAAAAA"));

  LocalBinaryFinder finder({tmp.path() + "/root"});

  base::Optional<FoundBinary> bin1 =
      finder.FindBinary("/dir/elf1.so", "AAAAAAAAAAAAAAAAAAAA");
  ASSERT_TRUE(bin1.has_value());
  EXPECT_EQ(bin1.value().file_name, tmp.path() + "/root/dir/elf1.so");
}

TEST(LocalBinaryFinderTest, AbsolutePathWithoutBaseApk) {
  base::TmpDirTree tmp;
  tmp.AddDir("root");
  tmp.AddDir("root/dir");
  tmp.AddFile("root/dir/elf1.so", CreateElfWithBuildId("AAAAAAAAAAAAAAAAAAAA"));

  LocalBinaryFinder finder({tmp.path() + "/root"});

  base::Optional<FoundBinary> bin1 =
      finder.FindBinary("/dir/base.apk!elf1.so", "AAAAAAAAAAAAAAAAAAAA");
  ASSERT_TRUE(bin1.has_value());
  EXPECT_EQ(bin1.value().file_name, tmp.path() + "/root/dir/elf1.so");
}

TEST(LocalBinaryFinderTest, OnlyFilename) {
  base::TmpDirTree tmp;
  tmp.AddDir("root");
  tmp.AddFile("root/elf1.so", CreateElfWithBuildId("AAAAAAAAAAAAAAAAAAAA"));

  LocalBinaryFinder finder({tmp.path() + "/root"});

  base::Optional<FoundBinary> bin1 =
      finder.FindBinary("/ignored_dir/elf1.so", "AAAAAAAAAAAAAAAAAAAA");
  ASSERT_TRUE(bin1.has_value());
  EXPECT_EQ(bin1.value().file_name, tmp.path() + "/root/elf1.so");
}

TEST(LocalBinaryFinderTest, OnlyFilenameWithoutBaseApk) {
  base::TmpDirTree tmp;
  tmp.AddDir("root");
  tmp.AddFile("root/elf1.so", CreateElfWithBuildId("AAAAAAAAAAAAAAAAAAAA"));

  LocalBinaryFinder finder({tmp.path() + "/root"});

  base::Optional<FoundBinary> bin1 = finder.FindBinary(
      "/ignored_dir/base.apk!elf1.so", "AAAAAAAAAAAAAAAAAAAA");
  ASSERT_TRUE(bin1.has_value());
  EXPECT_EQ(bin1.value().file_name, tmp.path() + "/root/elf1.so");
}

TEST(LocalBinaryFinderTest, BuildIdSubdir) {
  base::TmpDirTree tmp;
  tmp.AddDir("root");
  tmp.AddDir("root/.build-id");
  tmp.AddDir("root/.build-id/41");
  tmp.AddFile("root/.build-id/41/41414141414141414141414141414141414141.debug",
              CreateElfWithBuildId("AAAAAAAAAAAAAAAAAAAA"));

  LocalBinaryFinder finder({tmp.path() + "/root"});

  base::Optional<FoundBinary> bin1 =
      finder.FindBinary("/ignored_dir/ignored_name.so", "AAAAAAAAAAAAAAAAAAAA");
  ASSERT_TRUE(bin1.has_value());
  EXPECT_EQ(
      bin1.value().file_name,
      tmp.path() +
          "/root/.build-id/41/41414141414141414141414141414141414141.debug");
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto

#endif
