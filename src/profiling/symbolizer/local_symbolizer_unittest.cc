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

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#include <unistd.h>
#endif

namespace perfetto {
namespace profiling {
namespace {

TEST(LocalSymbolizerTest, ParseJsonLine) {
  std::vector<SymbolizedFrame> result;
  ASSERT_TRUE(ParseLlvmSymbolizerJsonLine(
      "{\"Address\":\"0x1b72f\",\"ModuleName\":\"...\",\"Symbol\":[{\"Column\":"
      "0,\"Discriminator\":0,\"FileName\":\"foo.h\",\"FunctionName\":\"foo\","
      "\"Line\":10,\"StartAddress\":\"\",\"StartFileName\":\"...\","
      "\"StartLine\":0},{\"Column\":"
      "0,\"Discriminator\":0,\"FileName\":\"bar.h\",\"FunctionName\":\"bar\","
      "\"Line\":20,\"StartAddress\":\"\",\"StartFileName\":\"...\","
      "\"StartLine\":0}]}",
      &result));
  EXPECT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0].file_name, "foo.h");
  EXPECT_EQ(result[0].function_name, "foo");
  EXPECT_EQ(result[0].line, 10u);
  EXPECT_EQ(result[1].file_name, "bar.h");
  EXPECT_EQ(result[1].function_name, "bar");
  EXPECT_EQ(result[1].line, 20u);
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

#if defined(MEMORY_SANITIZER)
// fts_read() causes some error under msan.
#define NOMSAN_SimpleTree DISABLED_SimpleTree
#else
#define NOMSAN_SimpleTree SimpleTree
#endif
TEST(LocalBinaryIndexerTest, NOMSAN_SimpleTree) {
  base::TmpDirTree tmp;
  tmp.AddDir("dir1");
  tmp.AddFile("dir1/elf1", CreateElfWithBuildId("AAAAAAAAAAAAAAAAAAAA"));
  tmp.AddFile("dir1/nonelf1", "OTHERDATA");
  tmp.AddDir("dir2");
  tmp.AddFile("dir2/elf1", CreateElfWithBuildId("BBBBBBBBBBBBBBBBBBBB"));
  tmp.AddFile("dir2/nonelf1", "other text");

  LocalBinaryIndexer indexer({tmp.path() + "/dir1", tmp.path() + "/dir2"});

  std::optional<FoundBinary> bin1 =
      indexer.FindBinary("", "AAAAAAAAAAAAAAAAAAAA");
  ASSERT_TRUE(bin1.has_value());
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  EXPECT_EQ(bin1.value().file_name, tmp.path() + "/dir1\\elf1");
#else
  EXPECT_EQ(bin1.value().file_name, tmp.path() + "/dir1/elf1");
#endif
  std::optional<FoundBinary> bin2 =
      indexer.FindBinary("", "BBBBBBBBBBBBBBBBBBBB");
  ASSERT_TRUE(bin2.has_value());
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  EXPECT_EQ(bin2.value().file_name, tmp.path() + "/dir2\\elf1");
#else
  EXPECT_EQ(bin2.value().file_name, tmp.path() + "/dir2/elf1");
#endif
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)

#if defined(MEMORY_SANITIZER)
// fts_read() causes some error under msan.
#define NOMSAN_Symlinks DISABLED_Symlinks
#else
#define NOMSAN_Symlinks Symlinks
#endif
TEST(LocalBinaryIndexerTest, NOMSAN_Symlinks) {
  base::TmpDirTree tmp;
  tmp.AddDir("real");
  tmp.AddFile("real/elf1", CreateElfWithBuildId("AAAAAAAAAAAAAAAAAAAA"));
  tmp.AddDir("real/dir1");
  tmp.AddFile("real/dir1/elf2", CreateElfWithBuildId("BBBBBBBBBBBBBBBBBBBB"));
  tmp.AddFile("real/dir1/elf3", CreateElfWithBuildId("CCCCCCCCCCCCCCCCCCCC"));
  tmp.AddDir("sym");
  EXPECT_EQ(symlink(tmp.AbsolutePath("real/elf1").c_str(),
                    tmp.AbsolutePath("sym/elf1").c_str()),
            0);
  tmp.TrackFile("sym/elf1");
  EXPECT_EQ(symlink(tmp.AbsolutePath("real/dir1").c_str(),
                    tmp.AbsolutePath("sym/dir1").c_str()),
            0);
  tmp.TrackFile("sym/dir1");

  LocalBinaryIndexer indexer({tmp.AbsolutePath("sym")});

  std::optional<FoundBinary> bin1 =
      indexer.FindBinary("", "AAAAAAAAAAAAAAAAAAAA");
  ASSERT_TRUE(bin1.has_value());
  EXPECT_EQ(bin1.value().file_name, tmp.AbsolutePath("sym/elf1"));

  std::optional<FoundBinary> bin2 =
      indexer.FindBinary("", "BBBBBBBBBBBBBBBBBBBB");
  ASSERT_TRUE(bin2.has_value());
  EXPECT_EQ(bin2.value().file_name, tmp.AbsolutePath("sym/dir1/elf2"));

  std::optional<FoundBinary> bin3 =
      indexer.FindBinary("", "CCCCCCCCCCCCCCCCCCCC");
  ASSERT_TRUE(bin3.has_value());
  EXPECT_EQ(bin3.value().file_name, tmp.AbsolutePath("sym/dir1/elf3"));
}

#if defined(MEMORY_SANITIZER)
// fts_read() causes some error under msan.
#define NOMSAN_RecursiveSymlinks DISABLED_RecursiveSymlinks
#else
#define NOMSAN_RecursiveSymlinks RecursiveSymlinks
#endif
TEST(LocalBinaryIndexerTest, NOMSAN_RecursiveSymlinks) {
  base::TmpDirTree tmp;
  tmp.AddDir("main");
  tmp.AddFile("main/elf1", CreateElfWithBuildId("AAAAAAAAAAAAAAAAAAAA"));
  tmp.AddDir("main/dir1");
  EXPECT_EQ(symlink(tmp.AbsolutePath("main").c_str(),
                    tmp.AbsolutePath("main/dir1/sym").c_str()),
            0);
  tmp.TrackFile("main/dir1/sym");

  LocalBinaryIndexer indexer({tmp.AbsolutePath("main")});

  std::optional<FoundBinary> bin1 =
      indexer.FindBinary("", "AAAAAAAAAAAAAAAAAAAA");
  ASSERT_TRUE(bin1.has_value());
  EXPECT_EQ(bin1.value().file_name, tmp.AbsolutePath("main/elf1"));
}

#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||
        // PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) ||
        // PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)

TEST(LocalBinaryFinderTest, AbsolutePath) {
  base::TmpDirTree tmp;
  tmp.AddDir("root");
  tmp.AddDir("root/dir");
  tmp.AddFile("root/dir/elf1.so", CreateElfWithBuildId("AAAAAAAAAAAAAAAAAAAA"));

  LocalBinaryFinder finder({tmp.path() + "/root"});

  std::optional<FoundBinary> bin1 =
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

  std::optional<FoundBinary> bin1 =
      finder.FindBinary("/dir/base.apk!elf1.so", "AAAAAAAAAAAAAAAAAAAA");
  ASSERT_TRUE(bin1.has_value());
  EXPECT_EQ(bin1.value().file_name, tmp.path() + "/root/dir/elf1.so");
}

TEST(LocalBinaryFinderTest, OnlyFilename) {
  base::TmpDirTree tmp;
  tmp.AddDir("root");
  tmp.AddFile("root/elf1.so", CreateElfWithBuildId("AAAAAAAAAAAAAAAAAAAA"));

  LocalBinaryFinder finder({tmp.path() + "/root"});

  std::optional<FoundBinary> bin1 =
      finder.FindBinary("/ignored_dir/elf1.so", "AAAAAAAAAAAAAAAAAAAA");
  ASSERT_TRUE(bin1.has_value());
  EXPECT_EQ(bin1.value().file_name, tmp.path() + "/root/elf1.so");
}

TEST(LocalBinaryFinderTest, OnlyFilenameWithoutBaseApk) {
  base::TmpDirTree tmp;
  tmp.AddDir("root");
  tmp.AddFile("root/elf1.so", CreateElfWithBuildId("AAAAAAAAAAAAAAAAAAAA"));

  LocalBinaryFinder finder({tmp.path() + "/root"});

  std::optional<FoundBinary> bin1 = finder.FindBinary(
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

  std::optional<FoundBinary> bin1 =
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
