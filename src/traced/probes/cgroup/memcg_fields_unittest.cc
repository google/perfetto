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

#include "src/traced/probes/cgroup/memcg_fields.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/linux/memcg_stats.gen.h"
#include "protos/perfetto/trace/linux/memcg_stats.pbzero.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

using MC = protos::pbzero::MemcgConfig;
using MemcgField = protos::pbzero::MemcgConfig_MemcgField;

// From /sys/fs/cgroup/system/uid_1000/pid_579/memory.stat on a 6.1 kernel.
// Note that the key order differs from newer kernels (e.g. pgscan comes before
// pgscan_kswapd here).
constexpr char kMemoryStat[] =
    "anon 48840704\n"
    "file 73920512\n"
    "kernel 0\n"
    "kernel_stack 0\n"
    "pagetables 0\n"
    "sec_pagetables 0\n"
    "percpu 0\n"
    "sock 0\n"
    "vmalloc 0\n"
    "shmem 4096\n"
    "file_mapped 61972480\n"
    "file_dirty 0\n"
    "file_writeback 0\n"
    "swapcached 0\n"
    "anon_thp 0\n"
    "file_thp 0\n"
    "shmem_thp 0\n"
    "inactive_anon 39817216\n"
    "active_anon 9027584\n"
    "inactive_file 69201920\n"
    "active_file 135168\n"
    "unevictable 4579328\n"
    "slab_reclaimable 0\n"
    "slab_unreclaimable 0\n"
    "slab 0\n"
    "workingset_refault_anon 0\n"
    "workingset_refault_file 1\n"
    "workingset_activate_anon 0\n"
    "workingset_activate_file 1\n"
    "workingset_restore_anon 0\n"
    "workingset_restore_file 0\n"
    "workingset_nodereclaim 0\n"
    "pgscan 94\n"
    "pgsteal 90\n"
    "pgscan_kswapd 94\n"
    "pgscan_direct 0\n"
    "pgsteal_kswapd 90\n"
    "pgsteal_direct 0\n"
    "pgfault 30250\n"
    "pgmajfault 601\n"
    "pgrefill 171\n"
    "pgactivate 3\n"
    "pgdeactivate 0\n"
    "pglazyfree 0\n"
    "pglazyfreed 0\n"
    "thp_fault_alloc 0\n"
    "thp_collapse_alloc 0\n";

constexpr char kMemoryEvents[] =
    "low 0\n"
    "high 12\n"
    "max 3\n"
    "oom 0\n"
    "oom_kill 1\n"
    "oom_group_kill 0\n";

MemcgFieldSet SetOf(const std::vector<MemcgField>& fields) {
  MemcgFieldSet ret;
  for (MemcgField f : fields)
    ret.set(static_cast<size_t>(f));
  return ret;
}

TEST(MemcgFieldsTest, ExpandFields) {
  EXPECT_EQ(ToMemcgFieldSet({MC::MEMCG_CURRENT, MC::MEMCG_STAT_ANON}),
            SetOf({MC::MEMCG_CURRENT, MC::MEMCG_STAT_ANON}));

  // *_ALL expands to the concrete fields of its file, and is never itself set.
  MemcgFieldSet stat_all = ToMemcgFieldSet({MC::MEMCG_STAT_ALL});
  EXPECT_TRUE(stat_all.test(MC::MEMCG_STAT_ANON));
  EXPECT_TRUE(stat_all.test(MC::MEMCG_STAT_NUMA_HINT_FAULTS));
  EXPECT_FALSE(stat_all.test(MC::MEMCG_EVENTS_HIGH));
  EXPECT_FALSE(stat_all.test(MC::MEMCG_STAT_ALL));

  // Union semantics.
  EXPECT_EQ(ToMemcgFieldSet({MC::MEMCG_STAT_ALL, MC::MEMCG_STAT_ANON}),
            stat_all);

  // Unknown values (e.g. from a newer config) are dropped.
  EXPECT_EQ(ToMemcgFieldSet({MC::MEMCG_UNSPECIFIED,
                             static_cast<MemcgField>(999), MC::MEMCG_PEAK}),
            SetOf({MC::MEMCG_PEAK}));
}

TEST(MemcgFieldsTest, FilesForFields) {
  MemcgFileSet files = MemcgFilesForFields(
      ToMemcgFieldSet({MC::MEMCG_CURRENT, MC::MEMCG_STAT_ANON,
                       MC::MEMCG_STAT_FILE, MC::MEMCG_EVENTS_LOCAL_ALL}));
  MemcgFileSet expected;
  expected.set(static_cast<size_t>(MemcgFile::kCurrent));
  expected.set(static_cast<size_t>(MemcgFile::kStat));
  expected.set(static_cast<size_t>(MemcgFile::kEventsLocal));
  EXPECT_EQ(files, expected);
}

// Parses |contents| as memory.high, returning the recorded value.
std::optional<uint64_t> ParseHigh(std::string_view contents) {
  MemcgValues values;
  ParseMemcgFile(MemcgFile::kHigh, contents, SetOf({MC::MEMCG_HIGH}), &values);
  return values.Get(MC::MEMCG_HIGH);
}

TEST(MemcgFieldsTest, ParseSingleValuedFile) {
  EXPECT_EQ(ParseHigh("122761216\n"), 122761216u);
  EXPECT_EQ(ParseHigh("max\n"), kMemcgValueMax);
  // Malformed contents record nothing.
  EXPECT_EQ(ParseHigh(""), std::nullopt);
  EXPECT_EQ(ParseHigh("maximum\n"), std::nullopt);
  EXPECT_EQ(ParseHigh("-1\n"), std::nullopt);

  // Nor does a file whose field isn't wanted.
  MemcgValues values;
  ParseMemcgFile(MemcgFile::kCurrent, "42\n", SetOf({MC::MEMCG_HIGH}), &values);
  EXPECT_TRUE(values.present().none());
}

TEST(MemcgFieldsTest, ParseStatSubset) {
  MemcgFieldSet wanted =
      SetOf({MC::MEMCG_STAT_ANON, MC::MEMCG_STAT_SHMEM, MC::MEMCG_STAT_PGSCAN,
             MC::MEMCG_STAT_PGSCAN_KSWAPD, MC::MEMCG_STAT_THP_COLLAPSE_ALLOC});
  MemcgValues values;
  ParseMemcgFile(MemcgFile::kStat, kMemoryStat, wanted, &values);
  EXPECT_EQ(values.present(), wanted);
  EXPECT_EQ(values.Get(MC::MEMCG_STAT_ANON), 48840704u);
  EXPECT_EQ(values.Get(MC::MEMCG_STAT_SHMEM), 4096u);
  EXPECT_EQ(values.Get(MC::MEMCG_STAT_PGSCAN), 94u);
  EXPECT_EQ(values.Get(MC::MEMCG_STAT_PGSCAN_KSWAPD), 94u);
  EXPECT_EQ(values.Get(MC::MEMCG_STAT_THP_COLLAPSE_ALLOC), 0u);
}

TEST(MemcgFieldsTest, ParseStatAll) {
  MemcgValues values;
  ParseMemcgFile(MemcgFile::kStat, kMemoryStat,
                 ToMemcgFieldSet({MC::MEMCG_STAT_ALL}), &values);
  // Every key in the file is recorded. The rest (e.g. zswap stats, or keys
  // from newer kernels) are absent.
  EXPECT_EQ(values.present().count(), 47u);
  EXPECT_EQ(values.Get(MC::MEMCG_STAT_UNEVICTABLE), 4579328u);
  EXPECT_EQ(values.Get(MC::MEMCG_STAT_ZSWAP), std::nullopt);
  EXPECT_EQ(values.Get(MC::MEMCG_STAT_PGDEMOTE_KSWAPD), std::nullopt);
}

TEST(MemcgFieldsTest, ParseKeyedFileSkipsUnwantedAndMalformedLines) {
  constexpr char kContents[] =
      "some_future_key 5\n"
      "malformed_line\n"
      "anon notanumber\n"
      "\n"
      "file 7\n"
      "anon 3\n"
      // An unterminated last line can only come from a truncated read.
      "shmem 4";
  MemcgValues values;
  ParseMemcgFile(MemcgFile::kStat, kContents,
                 SetOf({MC::MEMCG_STAT_ANON, MC::MEMCG_STAT_SHMEM}), &values);
  EXPECT_EQ(values.present(), SetOf({MC::MEMCG_STAT_ANON}));
  EXPECT_EQ(values.Get(MC::MEMCG_STAT_ANON), 3u);
}

TEST(MemcgFieldsTest, ParseEventsVsEventsLocal) {
  // The same contents parse into different fields depending on the file.
  MemcgFieldSet wanted =
      ToMemcgFieldSet({MC::MEMCG_EVENTS_HIGH, MC::MEMCG_EVENTS_LOCAL_ALL});

  MemcgValues values;
  ParseMemcgFile(MemcgFile::kEvents, kMemoryEvents, wanted, &values);
  ParseMemcgFile(MemcgFile::kEventsLocal, kMemoryEvents, wanted, &values);
  EXPECT_EQ(values.Get(MC::MEMCG_EVENTS_HIGH), 12u);
  EXPECT_EQ(values.Get(MC::MEMCG_EVENTS_MAX), std::nullopt);
  EXPECT_EQ(values.Get(MC::MEMCG_EVENTS_LOCAL_HIGH), 12u);
  EXPECT_EQ(values.Get(MC::MEMCG_EVENTS_LOCAL_MAX), 3u);
  EXPECT_EQ(values.Get(MC::MEMCG_EVENTS_LOCAL_OOM_KILL), 1u);
  // Not present in the file (added in newer kernels).
  EXPECT_EQ(values.Get(MC::MEMCG_EVENTS_LOCAL_SOCK_THROTTLED), std::nullopt);
}

TEST(MemcgFieldsTest, ParseSwapEvents) {
  MemcgValues values;
  ParseMemcgFile(MemcgFile::kSwapEvents, "high 0\nmax 1\nfail 2\n",
                 ToMemcgFieldSet({MC::MEMCG_SWAP_EVENTS_ALL}), &values);
  EXPECT_EQ(values.present().count(), 3u);
  EXPECT_EQ(values.Get(MC::MEMCG_SWAP_EVENTS_FAIL), 2u);
}

std::string Serialize(const MemcgValues& values) {
  protozero::HeapBuffered<protos::pbzero::MemcgStats_Cgroup> msg;
  SerializeMemcgValues(values, msg.get());
  return msg.SerializeAsString();
}

TEST(MemcgFieldsTest, ValuesGetAndClear) {
  MemcgValues values;
  EXPECT_EQ(values.Get(MC::MEMCG_CURRENT), std::nullopt);
  values.Set(MC::MEMCG_CURRENT, 0);
  EXPECT_EQ(values.Get(MC::MEMCG_CURRENT), 0u);
  values.Clear();
  EXPECT_EQ(values.Get(MC::MEMCG_CURRENT), std::nullopt);
  EXPECT_TRUE(values.present().none());
}

TEST(MemcgFieldsTest, SerializeNothing) {
  EXPECT_EQ(Serialize(MemcgValues()), "");
}

TEST(MemcgFieldsTest, Serialize) {
  MemcgValues values;
  values.Set(MC::MEMCG_CURRENT, 123);
  values.Set(MC::MEMCG_HIGH, kMemcgValueMax);
  values.Set(MC::MEMCG_EVENTS_HIGH, 1);
  values.Set(MC::MEMCG_EVENTS_LOCAL_HIGH, 2);
  values.Set(MC::MEMCG_EVENTS_LOCAL_OOM_KILL, 0);
  values.Set(MC::MEMCG_SWAP_EVENTS_FAIL, 3);
  values.Set(MC::MEMCG_STAT_ANON, 4);
  values.Set(MC::MEMCG_STAT_FILE, 0);
  values.Set(MC::MEMCG_STAT_NUMA_HINT_FAULTS, 5);

  protos::gen::MemcgStats_Cgroup cgroup;
  ASSERT_TRUE(cgroup.ParseFromString(Serialize(values)));
  EXPECT_FALSE(cgroup.has_android_group());
  EXPECT_EQ(cgroup.current(), 123u);
  EXPECT_EQ(cgroup.high(), kMemcgValueMax);
  EXPECT_FALSE(cgroup.has_peak());
  EXPECT_FALSE(cgroup.has_max());

  // memory.events and memory.events.local go into separate messages.
  EXPECT_EQ(cgroup.events().high(), 1u);
  EXPECT_FALSE(cgroup.events().has_oom_kill());
  EXPECT_EQ(cgroup.events_local().high(), 2u);
  // Zeros are recorded explicitly.
  EXPECT_TRUE(cgroup.events_local().has_oom_kill());
  EXPECT_EQ(cgroup.events_local().oom_kill(), 0u);

  EXPECT_EQ(cgroup.swap_events().fail(), 3u);
  EXPECT_FALSE(cgroup.swap_events().has_high());

  EXPECT_EQ(cgroup.stat().anon(), 4u);
  EXPECT_TRUE(cgroup.stat().has_file());
  EXPECT_EQ(cgroup.stat().file(), 0u);
  EXPECT_EQ(cgroup.stat().numa_hint_faults(), 5u);
  EXPECT_FALSE(cgroup.stat().has_shmem());
}

TEST(MemcgFieldsTest, SerializeOmitsEmptyFiles) {
  MemcgValues values;
  values.Set(MC::MEMCG_CURRENT, 1);
  protos::gen::MemcgStats_Cgroup cgroup;
  ASSERT_TRUE(cgroup.ParseFromString(Serialize(values)));
  EXPECT_EQ(cgroup.current(), 1u);
  EXPECT_FALSE(cgroup.has_events());
  EXPECT_FALSE(cgroup.has_events_local());
  EXPECT_FALSE(cgroup.has_swap_events());
  EXPECT_FALSE(cgroup.has_stat());
}

TEST(MemcgFieldsTest, SerializeAllFields) {
  // Every concrete field is written exactly once, to its own field number.
  std::vector<MemcgField> every_field;
  for (size_t i = 1; i < kMemcgFieldCount; i++)
    every_field.push_back(static_cast<MemcgField>(i));
  MemcgFieldSet all = ToMemcgFieldSet(every_field);
  // All but UNSPECIFIED and the four _ALL values.
  EXPECT_EQ(all.count(), kMemcgFieldCount - 1 - 4);
  MemcgValues values;
  for (size_t i = 0; i < kMemcgFieldCount; i++) {
    if (all.test(i))
      values.Set(static_cast<MemcgField>(i), i);
  }
  std::string serialized = Serialize(values);

  std::set<uint64_t> seen_values;
  std::set<uint32_t> top_level_ids;
  protozero::ProtoDecoder decoder(serialized.data(), serialized.size());
  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    EXPECT_TRUE(top_level_ids.insert(field.id()).second);
    if (field.type() !=
        protozero::proto_utils::ProtoWireType::kLengthDelimited) {
      seen_values.insert(field.as_uint64());
      continue;
    }
    std::set<uint32_t> nested_ids;
    protozero::ProtoDecoder nested(field.data(), field.size());
    for (auto f = nested.ReadField(); f.valid(); f = nested.ReadField()) {
      EXPECT_TRUE(nested_ids.insert(f.id()).second);
      EXPECT_TRUE(seen_values.insert(f.as_uint64()).second);
    }
  }
  // 12 single-valued files + 4 keyed files.
  EXPECT_EQ(top_level_ids.size(), 16u);
  EXPECT_EQ(seen_values.size(), all.count());
}

}  // namespace
}  // namespace perfetto
