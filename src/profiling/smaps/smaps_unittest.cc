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
#include "perfetto/ext/profiling/smaps.h"

#include <stdio.h>
#include <string>
#include <vector>

#include "perfetto/ext/base/pipe.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/config/profiling/smaps_config.gen.h"
#include "protos/perfetto/trace/profiling/smaps.gen.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

struct MemFile {
  explicit MemFile(const std::string& data) {
    // fmemopen not available on android api < 23, use pipe instead
    auto pipe = base::Pipe::Create();
    if (!data.empty()) {
      ssize_t res = write(pipe.wr.get(), data.data(), data.size());
      PERFETTO_CHECK(res == static_cast<ssize_t>(data.size()));
    }
    pipe.wr.reset();
    file = fdopen(pipe.rd.release(), "r");
    PERFETTO_CHECK(file);
  }
  ~MemFile() {
    if (file)
      fclose(file);
  }
  FILE* file = nullptr;
};

protos::gen::SmapsPacket ParseAndGetGenPacket(
    const std::string& smaps_content,
    const protos::gen::SmapsConfig& config) {
  protozero::HeapBuffered<protos::pbzero::TracePacket> packet;
  auto* smaps_packet = packet->set_smaps_packet();

  MemFile mem_file(smaps_content);
  ParseAndSerializeSmaps(mem_file.file, config, smaps_packet);

  protos::gen::TracePacket gen_packet;
  gen_packet.ParseFromString(packet.SerializeAsString());
  return gen_packet.smaps_packet();
}

std::string ToSmapsFormat(const std::vector<std::string>& names) {
  std::string ret;
  for (const std::string& name : names) {
    ret += "0000-0000 rw-p 00000000 00:00 0    " + name +
           "\nSize:                   0 kB\n";
  }
  return ret;
}

constexpr char kTestSmaps[] =
    R"(72359000-7235c000 ---p 00000000 00:00 0                                  [anon:dalvik-Boot image reservation]
Size:                 12 kB
KernelPageSize:        4 kB
MMUPageSize:           4 kB
Rss:                   0 kB
Pss:                   0 kB
Pss_Dirty:             0 kB
Shared_Clean:          0 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:         0 kB
Referenced:            0 kB
Anonymous:             0 kB
LazyFree:              0 kB
AnonHugePages:         0 kB
ShmemPmdMapped:        0 kB
FilePmdMapped:         0 kB
Shared_Hugetlb:        0 kB
Private_Hugetlb:       0 kB
Swap:                  0 kB
SwapPss:               0 kB
Locked:                0 kB
THPeligible:    0
VmFlags: mr mw me 
7235c000-72360000 r--p 00000000 fe:0c 1370                               /system/framework/arm64/boot-apache-xml.oat
Size:                 16 kB
KernelPageSize:        4 kB
MMUPageSize:           4 kB
Rss:                   0 kB
Pss:                   0 kB
Pss_Dirty:             0 kB
Shared_Clean:          0 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:         0 kB
Referenced:            0 kB
Anonymous:             0 kB
LazyFree:              0 kB
AnonHugePages:         0 kB
ShmemPmdMapped:        0 kB
FilePmdMapped:         0 kB
Shared_Hugetlb:        0 kB
Private_Hugetlb:       0 kB
Swap:                  0 kB
SwapPss:               0 kB
Locked:                0 kB
THPeligible:    0
VmFlags: rd mr mw me 
72360000-72554000 r--p 00000000 fe:0c 1424                               /system/framework/arm64/boot-framework.oat
Size:               2000 kB
KernelPageSize:        4 kB
MMUPageSize:           4 kB
Rss:                1936 kB
Pss:                  35 kB
Pss_Dirty:             0 kB
Shared_Clean:       1936 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:         0 kB
Referenced:         1916 kB
Anonymous:             0 kB
LazyFree:              0 kB
AnonHugePages:         0 kB
ShmemPmdMapped:        0 kB
FilePmdMapped:         0 kB
Shared_Hugetlb:        0 kB
Private_Hugetlb:       0 kB
Swap:                  0 kB
SwapPss:               0 kB
Locked:                0 kB
THPeligible:    0
VmFlags: rd mr mw me 
72554000-72bbd000 r-xp 001f4000 fe:0c 1424                               /system/framework/arm64/boot-framework.oat
Size:               6564 kB
KernelPageSize:        4 kB
MMUPageSize:           4 kB
Rss:                6048 kB
Pss:                 107 kB
Pss_Dirty:             0 kB
Shared_Clean:       6048 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:         0 kB
Referenced:         6036 kB
Anonymous:             0 kB
LazyFree:              0 kB
AnonHugePages:         0 kB
ShmemPmdMapped:        0 kB
FilePmdMapped:         0 kB
Shared_Hugetlb:        0 kB
Private_Hugetlb:       0 kB
Swap:                  0 kB
SwapPss:               0 kB
Locked:                0 kB
THPeligible:    0
VmFlags: rd ex mr mw me 
72bbd000-72bc0000 ---p 00000000 00:00 0                                  [anon:dalvik-Boot image reservation]
Size:                 12 kB
KernelPageSize:        4 kB
MMUPageSize:           4 kB
Rss:                   0 kB
Pss:                   0 kB
Pss_Dirty:             0 kB
Shared_Clean:          0 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:         0 kB
Referenced:            0 kB
Anonymous:             0 kB
LazyFree:              0 kB
AnonHugePages:         0 kB
ShmemPmdMapped:        0 kB
FilePmdMapped:         0 kB
Shared_Hugetlb:        0 kB
Private_Hugetlb:       0 kB
Swap:                  0 kB
SwapPss:               0 kB
Locked:                0 kB
THPeligible:    0
VmFlags: mr mw me 
72bc0000-72bc1000 rw-p 00000000 00:00 0                                  [anon:.bss]
Size:                  4 kB
KernelPageSize:        4 kB
MMUPageSize:           4 kB
Rss:                   4 kB
Pss:                   4 kB
Pss_Dirty:             4 kB
Shared_Clean:          0 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:         4 kB
Referenced:            4 kB
Anonymous:             4 kB
LazyFree:              0 kB
AnonHugePages:         0 kB
ShmemPmdMapped:        0 kB
FilePmdMapped:         0 kB
Shared_Hugetlb:        0 kB
Private_Hugetlb:       0 kB
Swap:                  0 kB
SwapPss:               0 kB
Locked:                0 kB
THPeligible:    0
VmFlags: rd wr mr mw me ac 
7273ac9000-7274504000 ---p 00000000 00:00 0 
Size:              10476 kB
KernelPageSize:        4 kB
MMUPageSize:           4 kB
Rss:                   0 kB
Pss:                   0 kB
Pss_Dirty:             0 kB
Shared_Clean:          0 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:         0 kB
Referenced:            0 kB
Anonymous:             0 kB
LazyFree:              0 kB
AnonHugePages:         0 kB
ShmemPmdMapped:        0 kB
FilePmdMapped:         0 kB
Shared_Hugetlb:        0 kB
Private_Hugetlb:       0 kB
Swap:                  0 kB
SwapPss:               0 kB
Locked:                0 kB
THPeligible:    0
VmFlags: mr mw me 
)";

TEST(SmapsParserTest, DefaultAggregatedMode) {
  protos::gen::SmapsConfig config;

  auto packet = ParseAndGetGenPacket(kTestSmaps, config);
  const auto& packed = packet.packed_entries();

  EXPECT_THAT(
      packed.string_table(),
      ElementsAre("", "[anon:dalvik-Boot image reservation]",
                  "/system/framework/arm64/boot-apache-xml.oat",
                  "/system/framework/arm64/boot-framework.oat", "[anon:.bss]"));

  // aggregated -> name_id not written since it's exactly the string_table order
  EXPECT_THAT(packed.name_id(), IsEmpty());

  EXPECT_THAT(packed.aggregate_count(), ElementsAre(1u, 2u, 1u, 2u, 1u));
  EXPECT_THAT(packed.size_kb(), ElementsAre(10476u, 24u, 16u, 8564u, 4u));
  EXPECT_THAT(packed.rss_kb(), ElementsAre(0u, 0u, 0u, 7984u, 4u));
}

constexpr char kTestSmapsCustomConfig[] =
    R"(1000-2000 rw-p 00000000 00:00 0    [anon:dalvik]
Size:                  10 kB
Rss:                    5 kB
Swap:                  20 kB
)";

TEST(SmapsParserTest, CustomConfigFields) {
  // record only given fields
  protos::gen::SmapsConfig config;
  config.set_unaggregated(true);
  config.add_vma_fields(protos::gen::SmapsConfig::VMA_FIELD_SWAP);
  config.add_vma_fields(protos::gen::SmapsConfig::VMA_FIELD_SIZE);

  auto packet = ParseAndGetGenPacket(kTestSmapsCustomConfig, config);
  const auto& packed = packet.packed_entries();

  EXPECT_THAT(packed.size_kb(), ElementsAre(10u));
  EXPECT_THAT(packed.swap_kb(), ElementsAre(20u));
  // no rss_kb since not requested
  EXPECT_THAT(packed.rss_kb(), IsEmpty());
}

constexpr char kTestSmapsUnaggregated[] =
    R"(1000-2000 rw-p 00000000 00:00 0    /lib/libc.so
Size:                  10 kB
3000-4000 r--p 00000000 00:00 0    /lib/libc.so
Size:                  20 kB
)";

TEST(SmapsParserTest, UnaggregatedMode) {
  protos::gen::SmapsConfig config;
  config.set_unaggregated(true);

  auto packet = ParseAndGetGenPacket(kTestSmapsUnaggregated, config);
  const auto& packed = packet.packed_entries();

  EXPECT_THAT(packed.string_table(), ElementsAre("", "/lib/libc.so"));
  // two separate entries, both with same name_id
  EXPECT_THAT(packed.name_id(), ElementsAre(1u, 1u));
  // separate size_kb
  EXPECT_THAT(packed.size_kb(), ElementsAre(10u, 20u));
  // aggregate_count not written
  EXPECT_THAT(packed.aggregate_count(), IsEmpty());
}

TEST(SmapsParserTest, NameRedactionPrefixMatch) {
  protos::gen::SmapsConfig config;
  config.set_unaggregated(true);

  auto* rule = config.add_name_redaction_rules();
  rule->set_match_mode(protos::gen::RedactionRule::MATCH_MODE_PREFIX);
  rule->set_pattern("/usr/lib");

  std::string smaps_content = ToSmapsFormat({
      "/usr/test.so",
      "/usr/lib/test.so",
      "/usr/lib/libtest.so",
      "/usr/lib64/libtest.so",  // note: still matches prefix
      "/usr/lib64/libtest.so (deleted)",
      "/usr/lib/nested/libtest.so",
      "/proc/self/smaps",
      "/proc/cmdline",
      "[vdso]",
      "[anon:named-/lib/libtest.so]",
  });

  auto packet = ParseAndGetGenPacket(smaps_content, config);
  const auto& packed = packet.packed_entries();

  EXPECT_THAT(packed.string_table(),
              ElementsAre("",                                //
                          "/usr/test.so",                    //
                          "<pf_redacted>",                   //
                          "<pf_redacted>",                   //
                          "<pf_redacted>",                   //
                          "<pf_redacted> (deleted)",         //
                          "<pf_redacted>",                   //
                          "/proc/self/smaps",                //
                          "/proc/cmdline",                   //
                          "[vdso]",                          //
                          "[anon:named-/lib/libtest.so]"));  //
}

TEST(SmapsParserTest, NameRedactionGlobPathMatch) {
  protos::gen::SmapsConfig config;
  config.set_unaggregated(true);

  auto* rule = config.add_name_redaction_rules();
  rule->set_match_mode(protos::gen::RedactionRule::MATCH_MODE_GLOB_PATH);
  rule->set_pattern("/*/*/libtest.so");

  std::string smaps_content = ToSmapsFormat({
      "/usr/libtest.so",
      "/usr/lib/libtest.so",
      "/usr/lib64/libtest.so",
      "/usr/lib64/libtest.so (deleted)",
      "/usr/lib/nested/libtest.so",
      "/proc/self/smaps",
      "/proc/cmdline",
      "[vdso]",
      "[anon:named-/lib/libtest.so]",
  });

  auto packet = ParseAndGetGenPacket(smaps_content, config);
  const auto& packed = packet.packed_entries();

  // Expect that a glob only matches exactly one path element.
  EXPECT_THAT(packed.string_table(),
              ElementsAre("",                                //
                          "/usr/libtest.so",                 //
                          "<pf_redacted>",                   //
                          "<pf_redacted>",                   //
                          "<pf_redacted> (deleted)",         //
                          "/usr/lib/nested/libtest.so",      //
                          "/proc/self/smaps",                //
                          "/proc/cmdline",                   //
                          "[vdso]",                          //
                          "[anon:named-/lib/libtest.so]"));  //
}

TEST(SmapsParserTest, NameRedactionGlobStringMatch) {
  protos::gen::SmapsConfig config;
  config.set_unaggregated(true);

  auto* rule = config.add_name_redaction_rules();
  rule->set_match_mode(protos::gen::RedactionRule::MATCH_MODE_GLOB_STRING);
  rule->set_pattern("/usr/*/libtest.so");

  std::string smaps_content = ToSmapsFormat({
      "/usr/libtest.so",
      "/usr/lib/libtest.so",
      "/usr/lib64/libtest.so",
      "/usr/lib64/libtest.so (deleted)",
      "/usr/lib/nested/libtest.so",
      "/proc/self/smaps",
      "/proc/cmdline",
      "[vdso]",
      "[anon:named-/lib/libtest.so]",
  });

  auto packet = ParseAndGetGenPacket(smaps_content, config);
  const auto& packed = packet.packed_entries();

  // Expect that a glob can match across path elements.
  EXPECT_THAT(packed.string_table(),
              ElementsAre("",                                //
                          "/usr/libtest.so",                 //
                          "<pf_redacted>",                   //
                          "<pf_redacted>",                   //
                          "<pf_redacted> (deleted)",         //
                          "<pf_redacted>",                   //
                          "/proc/self/smaps",                //
                          "/proc/cmdline",                   //
                          "[vdso]",                          //
                          "[anon:named-/lib/libtest.so]"));  //
}

TEST(SmapsParserTest, NameRedactionKeepFileExtension) {
  protos::gen::SmapsConfig config;
  config.set_unaggregated(true);

  auto* rule = config.add_name_redaction_rules();
  rule->set_match_mode(protos::gen::RedactionRule::MATCH_MODE_PREFIX);
  rule->set_pattern("/");
  rule->set_keep_file_extension(true);

  std::string smaps_content = ToSmapsFormat({
      "/lib/lib64/libtest.so",
      "/lib/lib64/libtest.so (deleted)",
      "/files/file.txt",
      "/files/file.txt (deleted)",
      "[vdso]",
      "[anon:named-/lib/libtest.so]",
      "/files/.hidden",
      "/files/dot.dot/no_ext",
  });

  auto packet = ParseAndGetGenPacket(smaps_content, config);
  const auto& packed = packet.packed_entries();

  EXPECT_THAT(packed.string_table(),
              ElementsAre("",                              //
                          "<pf_redacted>.so",              //
                          "<pf_redacted>.so (deleted)",    //
                          "<pf_redacted>.txt",             //
                          "<pf_redacted>.txt (deleted)",   //
                          "[vdso]",                        //
                          "[anon:named-/lib/libtest.so]",  //
                          "<pf_redacted>",                 //
                          "<pf_redacted>"));               //
}

TEST(SmapsParserTest, NameRedactionKeepPathElements) {
  protos::gen::SmapsConfig config;
  config.set_unaggregated(true);

  auto* rule = config.add_name_redaction_rules();
  rule->set_match_mode(protos::gen::RedactionRule::MATCH_MODE_PREFIX);
  rule->set_pattern("/");
  rule->set_keep_file_extension(true);
  rule->set_keep_path_elements(2);

  std::string smaps_content = ToSmapsFormat({
      "/lib/lib64/libtest.so",
      "/lib/lib64/libtest.so (deleted)",
      "/file.txt",
      "/files/file.txt",
      "/files/file.txt (deleted)",
      "/files/nested/file.txt",
      "/very/deep/path/file.txt",
      "[vdso]",
      "[anon:named-/lib/libtest.so]",
  });

  auto packet = ParseAndGetGenPacket(smaps_content, config);
  const auto& packed = packet.packed_entries();

  EXPECT_THAT(packed.string_table(),
              ElementsAre("",                                       //
                          "/lib/lib64/<pf_redacted>.so",            //
                          "/lib/lib64/<pf_redacted>.so (deleted)",  //
                          "/file.txt",                              //
                          "/files/file.txt",                        //
                          "/files/file.txt (deleted)",              //
                          "/files/nested/<pf_redacted>.txt",        //
                          "/very/deep/<pf_redacted>.txt",           //
                          "[vdso]",                                 //
                          "[anon:named-/lib/libtest.so]"));         //
}

TEST(SmapsParserTest, NameRedactionReplacementName) {
  protos::gen::SmapsConfig config;
  config.set_unaggregated(true);

  auto* rule = config.add_name_redaction_rules();
  rule->set_match_mode(protos::gen::RedactionRule::MATCH_MODE_PREFIX);
  rule->set_pattern("/");
  rule->set_keep_file_extension(true);
  rule->set_keep_path_elements(1);
  rule->set_replacement_name("<snip>");

  std::string smaps_content = ToSmapsFormat({
      "/lib/lib64/libtest.so",
      "/lib/lib64/libtest.so (deleted)",
  });

  auto packet = ParseAndGetGenPacket(smaps_content, config);
  const auto& packed = packet.packed_entries();

  EXPECT_THAT(packed.string_table(),
              ElementsAre("",                            //
                          "/lib/<snip>.so",              //
                          "/lib/<snip>.so (deleted)"));  //
}

TEST(SmapsParserTest, NameRedactionFirstMatchingRuleWins) {
  protos::gen::SmapsConfig config;
  config.set_unaggregated(true);

  // Two rules: first (keep_full) more specific than the second.
  {
    auto* rule = config.add_name_redaction_rules();
    rule->set_match_mode(protos::gen::RedactionRule::MATCH_MODE_PREFIX);
    rule->set_pattern("/usr/lib/libtest.so");
    rule->set_keep_full(true);
  }
  {
    auto* rule = config.add_name_redaction_rules();
    rule->set_match_mode(protos::gen::RedactionRule::MATCH_MODE_PREFIX);
    rule->set_pattern("/usr");
  }

  std::string smaps_content = ToSmapsFormat({
      "/usr/lib/libtest.so",
      "/usr/lib/libtest2.so",
      "/usr/lib64/libtest.so",
      "/proc/cmdline",
      "[vdso]",
  });

  auto packet = ParseAndGetGenPacket(smaps_content, config);
  const auto& packed = packet.packed_entries();

  // Everything under /usr is redacted unless it's exactly /usr/lib/libtest.so
  EXPECT_THAT(packed.string_table(),
              ElementsAre("",                     //
                          "/usr/lib/libtest.so",  //
                          "<pf_redacted>",        //
                          "<pf_redacted>",        //
                          "/proc/cmdline",        //
                          "[vdso]"));             //
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
