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

#ifndef INCLUDE_PERFETTO_EXT_TRACED_MEMCG_FIELD_DEFS_H_
#define INCLUDE_PERFETTO_EXT_TRACED_MEMCG_FIELD_DEFS_H_

#include <cstdint>
#include <string_view>

#include "protos/perfetto/config/linux/cgroup_config.pbzero.h"
#include "protos/perfetto/trace/linux/memcg_stats.pbzero.h"

// The memcg files and fields that the linux.cgroup data source can record,
// where their values go in MemcgStats, and what the values are. Shared by the
// data source and the trace processor importer.

namespace perfetto {

// The files in a memcg directory that the data source can read, as
// X(enum name, file name, nested message field number).
// clang-format off
#define PERFETTO_MEMCG_FILES(X)                                                                         \
  X(kCurrent,      "memory.current",       0),                                                          \
  X(kPeak,         "memory.peak",          0),                                                          \
  X(kMin,          "memory.min",           0),                                                          \
  X(kLow,          "memory.low",           0),                                                          \
  X(kHigh,         "memory.high",          0),                                                          \
  X(kMax,          "memory.max",           0),                                                          \
  X(kSwapCurrent,  "memory.swap.current",  0),                                                          \
  X(kSwapPeak,     "memory.swap.peak",     0),                                                          \
  X(kSwapHigh,     "memory.swap.high",     0),                                                          \
  X(kSwapMax,      "memory.swap.max",      0),                                                          \
  X(kZswapCurrent, "memory.zswap.current", 0),                                                          \
  X(kZswapMax,     "memory.zswap.max",     0),                                                          \
  X(kEvents,       "memory.events",        protos::pbzero::MemcgStats_Cgroup::kEventsFieldNumber),      \
  X(kEventsLocal,  "memory.events.local",  protos::pbzero::MemcgStats_Cgroup::kEventsLocalFieldNumber), \
  X(kSwapEvents,   "memory.swap.events",   protos::pbzero::MemcgStats_Cgroup::kSwapEventsFieldNumber),  \
  X(kStat,         "memory.stat",          protos::pbzero::MemcgStats_Cgroup::kStatFieldNumber)
// clang-format on

#define PERFETTO_MEMCG_FILE_ENUM(file, ...) file
enum class MemcgFile : uint8_t {
  PERFETTO_MEMCG_FILES(PERFETTO_MEMCG_FILE_ENUM),
  kNumFiles,
};
#undef PERFETTO_MEMCG_FILE_ENUM

struct MemcgFileDef {
  MemcgFile file;
  const char* name;
  // For keyed files: the field number of the file's nested message within
  // MemcgStats.Cgroup. Zero otherwise.
  uint32_t nested_proto_field;
};

// Indexed by MemcgFile.
#define PERFETTO_MEMCG_FILE_DEF(file, name, nested_proto_field) \
  {MemcgFile::file, name, nested_proto_field}
inline constexpr MemcgFileDef kMemcgFileDefs[] = {
    PERFETTO_MEMCG_FILES(PERFETTO_MEMCG_FILE_DEF)};
#undef PERFETTO_MEMCG_FILE_DEF

// Units of the field. Used in the trace processor importer.
enum class MemcgFieldKind : uint8_t {
  // A size in bytes.
  kSize,
  // A limit in bytes (can be "max", serialised as uint64_max).
  kLimit,
  // A count of events.
  kCount,
};

struct MemcgFieldDef {
  protos::pbzero::MemcgConfig_MemcgField field;
  MemcgFile file;
  // Key name for keyed files. Empty for single-valued files and the _ALL
  // values.
  std::string_view key;
  // The field number that the value is written to: within MemcgStats.Cgroup
  // for single-valued files, and within the file's nested message (e.g.
  // MemcgStats.Stat) for keyed files.
  uint32_t proto_field;
  // Meaningless for the _ALL values.
  MemcgFieldKind kind;
};

// clang-format off
// A row of kMemcgFieldDefs: the config enum value, the file, the key within a
// keyed file, the MemcgStats message the value is written to and its field
// number in it, and the kind of value.
#define PERFETTO_MEMCG_FIELD(field, file, key, msg, num, kind) \
  {protos::pbzero::MemcgConfig::field, MemcgFile::file, key,   \
   protos::pbzero::MemcgStats_##msg::num, MemcgFieldKind::kind}
// The _ALL values, which stand for all the fields of a keyed file.
#define PERFETTO_MEMCG_FIELD_ALL(field, file) \
  {protos::pbzero::MemcgConfig::field, MemcgFile::file, {}, 0, {}}

// Indexed by the proto MemcgField enum value.
inline constexpr MemcgFieldDef kMemcgFieldDefs[] = {
    {protos::pbzero::MemcgConfig::MEMCG_UNSPECIFIED, {}, {}, 0, {}},
    PERFETTO_MEMCG_FIELD(MEMCG_CURRENT, kCurrent, {}, Cgroup, kCurrentFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_PEAK, kPeak, {}, Cgroup, kPeakFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_MIN, kMin, {}, Cgroup, kMinFieldNumber, kLimit),
    PERFETTO_MEMCG_FIELD(MEMCG_LOW, kLow, {}, Cgroup, kLowFieldNumber, kLimit),
    PERFETTO_MEMCG_FIELD(MEMCG_HIGH, kHigh, {}, Cgroup, kHighFieldNumber, kLimit),
    PERFETTO_MEMCG_FIELD(MEMCG_MAX, kMax, {}, Cgroup, kMaxFieldNumber, kLimit),
    PERFETTO_MEMCG_FIELD(MEMCG_SWAP_CURRENT, kSwapCurrent, {}, Cgroup, kSwapCurrentFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_SWAP_PEAK, kSwapPeak, {}, Cgroup, kSwapPeakFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_SWAP_HIGH, kSwapHigh, {}, Cgroup, kSwapHighFieldNumber, kLimit),
    PERFETTO_MEMCG_FIELD(MEMCG_SWAP_MAX, kSwapMax, {}, Cgroup, kSwapMaxFieldNumber, kLimit),
    PERFETTO_MEMCG_FIELD(MEMCG_ZSWAP_CURRENT, kZswapCurrent, {}, Cgroup, kZswapCurrentFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_ZSWAP_MAX, kZswapMax, {}, Cgroup, kZswapMaxFieldNumber, kLimit),
    PERFETTO_MEMCG_FIELD_ALL(MEMCG_EVENTS_ALL, kEvents),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_LOW, kEvents, "low", Events, kLowFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_HIGH, kEvents, "high", Events, kHighFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_MAX, kEvents, "max", Events, kMaxFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_OOM, kEvents, "oom", Events, kOomFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_OOM_KILL, kEvents, "oom_kill", Events, kOomKillFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_OOM_GROUP_KILL, kEvents, "oom_group_kill", Events, kOomGroupKillFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_SOCK_THROTTLED, kEvents, "sock_throttled", Events, kSockThrottledFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD_ALL(MEMCG_EVENTS_LOCAL_ALL, kEventsLocal),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_LOCAL_LOW, kEventsLocal, "low", Events, kLowFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_LOCAL_HIGH, kEventsLocal, "high", Events, kHighFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_LOCAL_MAX, kEventsLocal, "max", Events, kMaxFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_LOCAL_OOM, kEventsLocal, "oom", Events, kOomFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_LOCAL_OOM_KILL, kEventsLocal, "oom_kill", Events, kOomKillFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_LOCAL_OOM_GROUP_KILL, kEventsLocal, "oom_group_kill", Events, kOomGroupKillFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_EVENTS_LOCAL_SOCK_THROTTLED, kEventsLocal, "sock_throttled", Events, kSockThrottledFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD_ALL(MEMCG_SWAP_EVENTS_ALL, kSwapEvents),
    PERFETTO_MEMCG_FIELD(MEMCG_SWAP_EVENTS_HIGH, kSwapEvents, "high", SwapEvents, kHighFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_SWAP_EVENTS_MAX, kSwapEvents, "max", SwapEvents, kMaxFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_SWAP_EVENTS_FAIL, kSwapEvents, "fail", SwapEvents, kFailFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD_ALL(MEMCG_STAT_ALL, kStat),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_ANON, kStat, "anon", Stat, kAnonFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_FILE, kStat, "file", Stat, kFileFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_KERNEL, kStat, "kernel", Stat, kKernelFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_KERNEL_STACK, kStat, "kernel_stack", Stat, kKernelStackFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PAGETABLES, kStat, "pagetables", Stat, kPagetablesFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_SEC_PAGETABLES, kStat, "sec_pagetables", Stat, kSecPagetablesFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PERCPU, kStat, "percpu", Stat, kPercpuFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_SOCK, kStat, "sock", Stat, kSockFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_VMALLOC, kStat, "vmalloc", Stat, kVmallocFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_SHMEM, kStat, "shmem", Stat, kShmemFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_ZSWAP, kStat, "zswap", Stat, kZswapFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_ZSWAPPED, kStat, "zswapped", Stat, kZswappedFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_ZSWAP_INCOMP, kStat, "zswap_incomp", Stat, kZswapIncompFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_FILE_MAPPED, kStat, "file_mapped", Stat, kFileMappedFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_FILE_DIRTY, kStat, "file_dirty", Stat, kFileDirtyFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_FILE_WRITEBACK, kStat, "file_writeback", Stat, kFileWritebackFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_SWAPCACHED, kStat, "swapcached", Stat, kSwapcachedFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_ANON_THP, kStat, "anon_thp", Stat, kAnonThpFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_FILE_THP, kStat, "file_thp", Stat, kFileThpFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_SHMEM_THP, kStat, "shmem_thp", Stat, kShmemThpFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_INACTIVE_ANON, kStat, "inactive_anon", Stat, kInactiveAnonFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_ACTIVE_ANON, kStat, "active_anon", Stat, kActiveAnonFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_INACTIVE_FILE, kStat, "inactive_file", Stat, kInactiveFileFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_ACTIVE_FILE, kStat, "active_file", Stat, kActiveFileFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_UNEVICTABLE, kStat, "unevictable", Stat, kUnevictableFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_SLAB_RECLAIMABLE, kStat, "slab_reclaimable", Stat, kSlabReclaimableFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_SLAB_UNRECLAIMABLE, kStat, "slab_unreclaimable", Stat, kSlabUnreclaimableFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_SLAB, kStat, "slab", Stat, kSlabFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_HUGETLB, kStat, "hugetlb", Stat, kHugetlbFieldNumber, kSize),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_WORKINGSET_REFAULT_ANON, kStat, "workingset_refault_anon", Stat, kWorkingsetRefaultAnonFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_WORKINGSET_REFAULT_FILE, kStat, "workingset_refault_file", Stat, kWorkingsetRefaultFileFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_WORKINGSET_ACTIVATE_ANON, kStat, "workingset_activate_anon", Stat, kWorkingsetActivateAnonFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_WORKINGSET_ACTIVATE_FILE, kStat, "workingset_activate_file", Stat, kWorkingsetActivateFileFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_WORKINGSET_RESTORE_ANON, kStat, "workingset_restore_anon", Stat, kWorkingsetRestoreAnonFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_WORKINGSET_RESTORE_FILE, kStat, "workingset_restore_file", Stat, kWorkingsetRestoreFileFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_WORKINGSET_NODERECLAIM, kStat, "workingset_nodereclaim", Stat, kWorkingsetNodereclaimFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGDEMOTE_KSWAPD, kStat, "pgdemote_kswapd", Stat, kPgdemoteKswapdFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGDEMOTE_DIRECT, kStat, "pgdemote_direct", Stat, kPgdemoteDirectFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGDEMOTE_KHUGEPAGED, kStat, "pgdemote_khugepaged", Stat, kPgdemoteKhugepagedFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGDEMOTE_PROACTIVE, kStat, "pgdemote_proactive", Stat, kPgdemoteProactiveFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGSTEAL_KSWAPD, kStat, "pgsteal_kswapd", Stat, kPgstealKswapdFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGSTEAL_DIRECT, kStat, "pgsteal_direct", Stat, kPgstealDirectFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGSTEAL_KHUGEPAGED, kStat, "pgsteal_khugepaged", Stat, kPgstealKhugepagedFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGSTEAL_PROACTIVE, kStat, "pgsteal_proactive", Stat, kPgstealProactiveFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGSCAN_KSWAPD, kStat, "pgscan_kswapd", Stat, kPgscanKswapdFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGSCAN_DIRECT, kStat, "pgscan_direct", Stat, kPgscanDirectFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGSCAN_KHUGEPAGED, kStat, "pgscan_khugepaged", Stat, kPgscanKhugepagedFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGSCAN_PROACTIVE, kStat, "pgscan_proactive", Stat, kPgscanProactiveFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGREFILL, kStat, "pgrefill", Stat, kPgrefillFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGPROMOTE_SUCCESS, kStat, "pgpromote_success", Stat, kPgpromoteSuccessFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGSCAN, kStat, "pgscan", Stat, kPgscanFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGSTEAL, kStat, "pgsteal", Stat, kPgstealFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PSWPIN, kStat, "pswpin", Stat, kPswpinFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PSWPOUT, kStat, "pswpout", Stat, kPswpoutFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGFAULT, kStat, "pgfault", Stat, kPgfaultFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGMAJFAULT, kStat, "pgmajfault", Stat, kPgmajfaultFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGACTIVATE, kStat, "pgactivate", Stat, kPgactivateFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGDEACTIVATE, kStat, "pgdeactivate", Stat, kPgdeactivateFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGLAZYFREE, kStat, "pglazyfree", Stat, kPglazyfreeFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_PGLAZYFREED, kStat, "pglazyfreed", Stat, kPglazyfreedFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_SWPIN_ZERO, kStat, "swpin_zero", Stat, kSwpinZeroFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_SWPOUT_ZERO, kStat, "swpout_zero", Stat, kSwpoutZeroFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_ZSWPIN, kStat, "zswpin", Stat, kZswpinFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_ZSWPOUT, kStat, "zswpout", Stat, kZswpoutFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_ZSWPWB, kStat, "zswpwb", Stat, kZswpwbFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_THP_FAULT_ALLOC, kStat, "thp_fault_alloc", Stat, kThpFaultAllocFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_THP_COLLAPSE_ALLOC, kStat, "thp_collapse_alloc", Stat, kThpCollapseAllocFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_THP_SWPOUT, kStat, "thp_swpout", Stat, kThpSwpoutFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_THP_SWPOUT_FALLBACK, kStat, "thp_swpout_fallback", Stat, kThpSwpoutFallbackFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_NUMA_PAGES_MIGRATED, kStat, "numa_pages_migrated", Stat, kNumaPagesMigratedFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_NUMA_PTE_UPDATES, kStat, "numa_pte_updates", Stat, kNumaPteUpdatesFieldNumber, kCount),
    PERFETTO_MEMCG_FIELD(MEMCG_STAT_NUMA_HINT_FAULTS, kStat, "numa_hint_faults", Stat, kNumaHintFaultsFieldNumber, kCount),
};
// clang-format on
#undef PERFETTO_MEMCG_FIELD_ALL
#undef PERFETTO_MEMCG_FIELD

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_TRACED_MEMCG_FIELD_DEFS_H_
