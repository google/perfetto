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
#include <string_view>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/message.h"
#include "protos/perfetto/trace/linux/memcg_stats.pbzero.h"

namespace perfetto {
namespace {

using MC = protos::pbzero::MemcgConfig;
using MemcgField = protos::pbzero::MemcgConfig_MemcgField;
using Cgroup = protos::pbzero::MemcgStats_Cgroup;
using Events = protos::pbzero::MemcgStats_Events;
using SwapEvents = protos::pbzero::MemcgStats_SwapEvents;
using Stat = protos::pbzero::MemcgStats_Stat;

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
constexpr MemcgFileDef kMemcgFileDefs[] = {
    PERFETTO_MEMCG_FILES(PERFETTO_MEMCG_FILE_DEF)};
#undef PERFETTO_MEMCG_FILE_DEF

struct MemcgFieldDef {
  MemcgField field;
  MemcgFile file;
  // Key name for keyed files. Empty for single-valued files and the _ALL
  // values.
  std::string_view key;
  // The field number that the value is written to: within MemcgStats.Cgroup
  // for single-valued files, and within the file's nested message (e.g.
  // MemcgStats.Stat) for keyed files.
  uint32_t proto_field;
};

// Indexed by the proto MemcgField enum value.
// clang-format off
constexpr MemcgFieldDef kMemcgFieldDefs[] = {
    {MC::MEMCG_UNSPECIFIED, {}, {}, 0},
    {MC::MEMCG_CURRENT, MemcgFile::kCurrent, {}, Cgroup::kCurrentFieldNumber},
    {MC::MEMCG_PEAK, MemcgFile::kPeak, {}, Cgroup::kPeakFieldNumber},
    {MC::MEMCG_MIN, MemcgFile::kMin, {}, Cgroup::kMinFieldNumber},
    {MC::MEMCG_LOW, MemcgFile::kLow, {}, Cgroup::kLowFieldNumber},
    {MC::MEMCG_HIGH, MemcgFile::kHigh, {}, Cgroup::kHighFieldNumber},
    {MC::MEMCG_MAX, MemcgFile::kMax, {}, Cgroup::kMaxFieldNumber},
    {MC::MEMCG_SWAP_CURRENT, MemcgFile::kSwapCurrent, {}, Cgroup::kSwapCurrentFieldNumber},
    {MC::MEMCG_SWAP_PEAK, MemcgFile::kSwapPeak, {}, Cgroup::kSwapPeakFieldNumber},
    {MC::MEMCG_SWAP_HIGH, MemcgFile::kSwapHigh, {}, Cgroup::kSwapHighFieldNumber},
    {MC::MEMCG_SWAP_MAX, MemcgFile::kSwapMax, {}, Cgroup::kSwapMaxFieldNumber},
    {MC::MEMCG_ZSWAP_CURRENT, MemcgFile::kZswapCurrent, {}, Cgroup::kZswapCurrentFieldNumber},
    {MC::MEMCG_ZSWAP_MAX, MemcgFile::kZswapMax, {}, Cgroup::kZswapMaxFieldNumber},
    {MC::MEMCG_EVENTS_ALL, MemcgFile::kEvents, {}, 0},
    {MC::MEMCG_EVENTS_LOW, MemcgFile::kEvents, "low", Events::kLowFieldNumber},
    {MC::MEMCG_EVENTS_HIGH, MemcgFile::kEvents, "high", Events::kHighFieldNumber},
    {MC::MEMCG_EVENTS_MAX, MemcgFile::kEvents, "max", Events::kMaxFieldNumber},
    {MC::MEMCG_EVENTS_OOM, MemcgFile::kEvents, "oom", Events::kOomFieldNumber},
    {MC::MEMCG_EVENTS_OOM_KILL, MemcgFile::kEvents, "oom_kill", Events::kOomKillFieldNumber},
    {MC::MEMCG_EVENTS_OOM_GROUP_KILL, MemcgFile::kEvents, "oom_group_kill", Events::kOomGroupKillFieldNumber},
    {MC::MEMCG_EVENTS_SOCK_THROTTLED, MemcgFile::kEvents, "sock_throttled", Events::kSockThrottledFieldNumber},
    {MC::MEMCG_EVENTS_LOCAL_ALL, MemcgFile::kEventsLocal, {}, 0},
    {MC::MEMCG_EVENTS_LOCAL_LOW, MemcgFile::kEventsLocal, "low", Events::kLowFieldNumber},
    {MC::MEMCG_EVENTS_LOCAL_HIGH, MemcgFile::kEventsLocal, "high", Events::kHighFieldNumber},
    {MC::MEMCG_EVENTS_LOCAL_MAX, MemcgFile::kEventsLocal, "max", Events::kMaxFieldNumber},
    {MC::MEMCG_EVENTS_LOCAL_OOM, MemcgFile::kEventsLocal, "oom", Events::kOomFieldNumber},
    {MC::MEMCG_EVENTS_LOCAL_OOM_KILL, MemcgFile::kEventsLocal, "oom_kill", Events::kOomKillFieldNumber},
    {MC::MEMCG_EVENTS_LOCAL_OOM_GROUP_KILL, MemcgFile::kEventsLocal, "oom_group_kill", Events::kOomGroupKillFieldNumber},
    {MC::MEMCG_EVENTS_LOCAL_SOCK_THROTTLED, MemcgFile::kEventsLocal, "sock_throttled", Events::kSockThrottledFieldNumber},
    {MC::MEMCG_SWAP_EVENTS_ALL, MemcgFile::kSwapEvents, {}, 0},
    {MC::MEMCG_SWAP_EVENTS_HIGH, MemcgFile::kSwapEvents, "high", SwapEvents::kHighFieldNumber},
    {MC::MEMCG_SWAP_EVENTS_MAX, MemcgFile::kSwapEvents, "max", SwapEvents::kMaxFieldNumber},
    {MC::MEMCG_SWAP_EVENTS_FAIL, MemcgFile::kSwapEvents, "fail", SwapEvents::kFailFieldNumber},
    {MC::MEMCG_STAT_ALL, MemcgFile::kStat, {}, 0},
    {MC::MEMCG_STAT_ANON, MemcgFile::kStat, "anon", Stat::kAnonFieldNumber},
    {MC::MEMCG_STAT_FILE, MemcgFile::kStat, "file", Stat::kFileFieldNumber},
    {MC::MEMCG_STAT_KERNEL, MemcgFile::kStat, "kernel", Stat::kKernelFieldNumber},
    {MC::MEMCG_STAT_KERNEL_STACK, MemcgFile::kStat, "kernel_stack", Stat::kKernelStackFieldNumber},
    {MC::MEMCG_STAT_PAGETABLES, MemcgFile::kStat, "pagetables", Stat::kPagetablesFieldNumber},
    {MC::MEMCG_STAT_SEC_PAGETABLES, MemcgFile::kStat, "sec_pagetables", Stat::kSecPagetablesFieldNumber},
    {MC::MEMCG_STAT_PERCPU, MemcgFile::kStat, "percpu", Stat::kPercpuFieldNumber},
    {MC::MEMCG_STAT_SOCK, MemcgFile::kStat, "sock", Stat::kSockFieldNumber},
    {MC::MEMCG_STAT_VMALLOC, MemcgFile::kStat, "vmalloc", Stat::kVmallocFieldNumber},
    {MC::MEMCG_STAT_SHMEM, MemcgFile::kStat, "shmem", Stat::kShmemFieldNumber},
    {MC::MEMCG_STAT_ZSWAP, MemcgFile::kStat, "zswap", Stat::kZswapFieldNumber},
    {MC::MEMCG_STAT_ZSWAPPED, MemcgFile::kStat, "zswapped", Stat::kZswappedFieldNumber},
    {MC::MEMCG_STAT_ZSWAP_INCOMP, MemcgFile::kStat, "zswap_incomp", Stat::kZswapIncompFieldNumber},
    {MC::MEMCG_STAT_FILE_MAPPED, MemcgFile::kStat, "file_mapped", Stat::kFileMappedFieldNumber},
    {MC::MEMCG_STAT_FILE_DIRTY, MemcgFile::kStat, "file_dirty", Stat::kFileDirtyFieldNumber},
    {MC::MEMCG_STAT_FILE_WRITEBACK, MemcgFile::kStat, "file_writeback", Stat::kFileWritebackFieldNumber},
    {MC::MEMCG_STAT_SWAPCACHED, MemcgFile::kStat, "swapcached", Stat::kSwapcachedFieldNumber},
    {MC::MEMCG_STAT_ANON_THP, MemcgFile::kStat, "anon_thp", Stat::kAnonThpFieldNumber},
    {MC::MEMCG_STAT_FILE_THP, MemcgFile::kStat, "file_thp", Stat::kFileThpFieldNumber},
    {MC::MEMCG_STAT_SHMEM_THP, MemcgFile::kStat, "shmem_thp", Stat::kShmemThpFieldNumber},
    {MC::MEMCG_STAT_INACTIVE_ANON, MemcgFile::kStat, "inactive_anon", Stat::kInactiveAnonFieldNumber},
    {MC::MEMCG_STAT_ACTIVE_ANON, MemcgFile::kStat, "active_anon", Stat::kActiveAnonFieldNumber},
    {MC::MEMCG_STAT_INACTIVE_FILE, MemcgFile::kStat, "inactive_file", Stat::kInactiveFileFieldNumber},
    {MC::MEMCG_STAT_ACTIVE_FILE, MemcgFile::kStat, "active_file", Stat::kActiveFileFieldNumber},
    {MC::MEMCG_STAT_UNEVICTABLE, MemcgFile::kStat, "unevictable", Stat::kUnevictableFieldNumber},
    {MC::MEMCG_STAT_SLAB_RECLAIMABLE, MemcgFile::kStat, "slab_reclaimable", Stat::kSlabReclaimableFieldNumber},
    {MC::MEMCG_STAT_SLAB_UNRECLAIMABLE, MemcgFile::kStat, "slab_unreclaimable", Stat::kSlabUnreclaimableFieldNumber},
    {MC::MEMCG_STAT_SLAB, MemcgFile::kStat, "slab", Stat::kSlabFieldNumber},
    {MC::MEMCG_STAT_HUGETLB, MemcgFile::kStat, "hugetlb", Stat::kHugetlbFieldNumber},
    {MC::MEMCG_STAT_WORKINGSET_REFAULT_ANON, MemcgFile::kStat, "workingset_refault_anon", Stat::kWorkingsetRefaultAnonFieldNumber},
    {MC::MEMCG_STAT_WORKINGSET_REFAULT_FILE, MemcgFile::kStat, "workingset_refault_file", Stat::kWorkingsetRefaultFileFieldNumber},
    {MC::MEMCG_STAT_WORKINGSET_ACTIVATE_ANON, MemcgFile::kStat, "workingset_activate_anon", Stat::kWorkingsetActivateAnonFieldNumber},
    {MC::MEMCG_STAT_WORKINGSET_ACTIVATE_FILE, MemcgFile::kStat, "workingset_activate_file", Stat::kWorkingsetActivateFileFieldNumber},
    {MC::MEMCG_STAT_WORKINGSET_RESTORE_ANON, MemcgFile::kStat, "workingset_restore_anon", Stat::kWorkingsetRestoreAnonFieldNumber},
    {MC::MEMCG_STAT_WORKINGSET_RESTORE_FILE, MemcgFile::kStat, "workingset_restore_file", Stat::kWorkingsetRestoreFileFieldNumber},
    {MC::MEMCG_STAT_WORKINGSET_NODERECLAIM, MemcgFile::kStat, "workingset_nodereclaim", Stat::kWorkingsetNodereclaimFieldNumber},
    {MC::MEMCG_STAT_PGDEMOTE_KSWAPD, MemcgFile::kStat, "pgdemote_kswapd", Stat::kPgdemoteKswapdFieldNumber},
    {MC::MEMCG_STAT_PGDEMOTE_DIRECT, MemcgFile::kStat, "pgdemote_direct", Stat::kPgdemoteDirectFieldNumber},
    {MC::MEMCG_STAT_PGDEMOTE_KHUGEPAGED, MemcgFile::kStat, "pgdemote_khugepaged", Stat::kPgdemoteKhugepagedFieldNumber},
    {MC::MEMCG_STAT_PGDEMOTE_PROACTIVE, MemcgFile::kStat, "pgdemote_proactive", Stat::kPgdemoteProactiveFieldNumber},
    {MC::MEMCG_STAT_PGSTEAL_KSWAPD, MemcgFile::kStat, "pgsteal_kswapd", Stat::kPgstealKswapdFieldNumber},
    {MC::MEMCG_STAT_PGSTEAL_DIRECT, MemcgFile::kStat, "pgsteal_direct", Stat::kPgstealDirectFieldNumber},
    {MC::MEMCG_STAT_PGSTEAL_KHUGEPAGED, MemcgFile::kStat, "pgsteal_khugepaged", Stat::kPgstealKhugepagedFieldNumber},
    {MC::MEMCG_STAT_PGSTEAL_PROACTIVE, MemcgFile::kStat, "pgsteal_proactive", Stat::kPgstealProactiveFieldNumber},
    {MC::MEMCG_STAT_PGSCAN_KSWAPD, MemcgFile::kStat, "pgscan_kswapd", Stat::kPgscanKswapdFieldNumber},
    {MC::MEMCG_STAT_PGSCAN_DIRECT, MemcgFile::kStat, "pgscan_direct", Stat::kPgscanDirectFieldNumber},
    {MC::MEMCG_STAT_PGSCAN_KHUGEPAGED, MemcgFile::kStat, "pgscan_khugepaged", Stat::kPgscanKhugepagedFieldNumber},
    {MC::MEMCG_STAT_PGSCAN_PROACTIVE, MemcgFile::kStat, "pgscan_proactive", Stat::kPgscanProactiveFieldNumber},
    {MC::MEMCG_STAT_PGREFILL, MemcgFile::kStat, "pgrefill", Stat::kPgrefillFieldNumber},
    {MC::MEMCG_STAT_PGPROMOTE_SUCCESS, MemcgFile::kStat, "pgpromote_success", Stat::kPgpromoteSuccessFieldNumber},
    {MC::MEMCG_STAT_PGSCAN, MemcgFile::kStat, "pgscan", Stat::kPgscanFieldNumber},
    {MC::MEMCG_STAT_PGSTEAL, MemcgFile::kStat, "pgsteal", Stat::kPgstealFieldNumber},
    {MC::MEMCG_STAT_PSWPIN, MemcgFile::kStat, "pswpin", Stat::kPswpinFieldNumber},
    {MC::MEMCG_STAT_PSWPOUT, MemcgFile::kStat, "pswpout", Stat::kPswpoutFieldNumber},
    {MC::MEMCG_STAT_PGFAULT, MemcgFile::kStat, "pgfault", Stat::kPgfaultFieldNumber},
    {MC::MEMCG_STAT_PGMAJFAULT, MemcgFile::kStat, "pgmajfault", Stat::kPgmajfaultFieldNumber},
    {MC::MEMCG_STAT_PGACTIVATE, MemcgFile::kStat, "pgactivate", Stat::kPgactivateFieldNumber},
    {MC::MEMCG_STAT_PGDEACTIVATE, MemcgFile::kStat, "pgdeactivate", Stat::kPgdeactivateFieldNumber},
    {MC::MEMCG_STAT_PGLAZYFREE, MemcgFile::kStat, "pglazyfree", Stat::kPglazyfreeFieldNumber},
    {MC::MEMCG_STAT_PGLAZYFREED, MemcgFile::kStat, "pglazyfreed", Stat::kPglazyfreedFieldNumber},
    {MC::MEMCG_STAT_SWPIN_ZERO, MemcgFile::kStat, "swpin_zero", Stat::kSwpinZeroFieldNumber},
    {MC::MEMCG_STAT_SWPOUT_ZERO, MemcgFile::kStat, "swpout_zero", Stat::kSwpoutZeroFieldNumber},
    {MC::MEMCG_STAT_ZSWPIN, MemcgFile::kStat, "zswpin", Stat::kZswpinFieldNumber},
    {MC::MEMCG_STAT_ZSWPOUT, MemcgFile::kStat, "zswpout", Stat::kZswpoutFieldNumber},
    {MC::MEMCG_STAT_ZSWPWB, MemcgFile::kStat, "zswpwb", Stat::kZswpwbFieldNumber},
    {MC::MEMCG_STAT_THP_FAULT_ALLOC, MemcgFile::kStat, "thp_fault_alloc", Stat::kThpFaultAllocFieldNumber},
    {MC::MEMCG_STAT_THP_COLLAPSE_ALLOC, MemcgFile::kStat, "thp_collapse_alloc", Stat::kThpCollapseAllocFieldNumber},
    {MC::MEMCG_STAT_THP_SWPOUT, MemcgFile::kStat, "thp_swpout", Stat::kThpSwpoutFieldNumber},
    {MC::MEMCG_STAT_THP_SWPOUT_FALLBACK, MemcgFile::kStat, "thp_swpout_fallback", Stat::kThpSwpoutFallbackFieldNumber},
    {MC::MEMCG_STAT_NUMA_PAGES_MIGRATED, MemcgFile::kStat, "numa_pages_migrated", Stat::kNumaPagesMigratedFieldNumber},
    {MC::MEMCG_STAT_NUMA_PTE_UPDATES, MemcgFile::kStat, "numa_pte_updates", Stat::kNumaPteUpdatesFieldNumber},
    {MC::MEMCG_STAT_NUMA_HINT_FAULTS, MemcgFile::kStat, "numa_hint_faults", Stat::kNumaHintFaultsFieldNumber},
};
// clang-format on
static_assert(std::size(kMemcgFieldDefs) == kMemcgFieldCount);

constexpr bool FieldDefsAreDense() {
  for (size_t i = 0; i < std::size(kMemcgFieldDefs); i++) {
    if (static_cast<size_t>(kMemcgFieldDefs[i].field) != i)
      return false;
  }
  return true;
}
static_assert(FieldDefsAreDense(), "the table must be in enum order");

const MemcgFieldDef* GetFieldDef(MemcgField field) {
  auto idx = static_cast<size_t>(field);
  if (idx == 0 || idx >= kMemcgFieldCount)
    return nullptr;
  return &kMemcgFieldDefs[idx];
}

// True for files containing "<key> <value>" lines.
bool IsKeyedMemcgFile(MemcgFile file) {
  return kMemcgFileDefs[static_cast<size_t>(file)].nested_proto_field != 0;
}

// True if this is a convenience _ALL enum that expands to the entire keyed
// file.
bool IsAllField(const MemcgFieldDef& def) {
  return IsKeyedMemcgFile(def.file) && def.key.empty();
}

// The field set for this file.
const MemcgFieldSet& MemcgFieldsInFile(MemcgFile file) {
  // Cannot be constexpr until C++23.
  static const std::array<MemcgFieldSet, kMemcgFileCount> fields_in_file = [] {
    std::array<MemcgFieldSet, kMemcgFileCount> ret;
    for (const MemcgFieldDef& def : kMemcgFieldDefs) {
      if (def.field != MC::MEMCG_UNSPECIFIED && !IsAllField(def))
        ret[static_cast<size_t>(def.file)].set(static_cast<size_t>(def.field));
    }
    return ret;
  }();
  return fields_in_file[static_cast<size_t>(file)];
}

// Note: this is a linear scan of the lookup table, and will be called for each
// line in a keyed file, however the performance is acceptable and keeps the
// code simple.
std::optional<MemcgField> FieldForKey(MemcgFile file, std::string_view key) {
  const MemcgFieldSet& fields = MemcgFieldsInFile(file);
  for (size_t i = 0; i < kMemcgFieldCount; i++) {
    if (fields.test(i) && kMemcgFieldDefs[i].key == key)
      return static_cast<MemcgField>(i);
  }
  return std::nullopt;
}

// Parses the contents of a single-valued file.
std::optional<uint64_t> ParseMemcgSingleValue(std::string_view sv) {
  while (!sv.empty() && (sv.back() == '\n' || sv.back() == ' ')) {
    sv.remove_suffix(1);
  }
  // Limit files such as memory.high return "max" for no limit.
  if (sv == "max")
    return kMemcgValueMax;
  return base::StringViewToNumber<uint64_t>(sv);
}

// Parses the contents of a keyed |file|, recording the values of the keys in
// |wanted| into |out|.
void ParseMemcgKeyedFile(MemcgFile file,
                         std::string_view contents,
                         const MemcgFieldSet& wanted,
                         MemcgValues* out) {
  PERFETTO_DCHECK(IsKeyedMemcgFile(file));
  MemcgFieldSet remaining = wanted & MemcgFieldsInFile(file);
  while (remaining.any()) {
    size_t eol_pos = contents.find('\n');
    if (eol_pos == std::string_view::npos)
      break;
    std::string_view line = contents.substr(0, eol_pos);
    contents.remove_prefix(eol_pos + 1);  // advance past the line

    size_t space_pos = line.find(' ');
    if (space_pos == std::string_view::npos)
      continue;
    std::optional<MemcgField> field =
        FieldForKey(file, line.substr(0, space_pos));
    if (!field || !remaining.test(static_cast<size_t>(*field)))
      continue;
    std::optional<uint64_t> value =
        base::StringViewToNumber<uint64_t>(line.substr(space_pos + 1));
    if (!value.has_value())
      continue;

    out->Set(*field, *value);
    remaining.reset(static_cast<size_t>(*field));
  }
}

}  // namespace

const char* MemcgFileName(MemcgFile file) {
  return kMemcgFileDefs[static_cast<size_t>(file)].name;
}

MemcgFieldSet ToMemcgFieldSet(const std::vector<MemcgField>& fields) {
  MemcgFieldSet ret;
  for (MemcgField field : fields) {
    const MemcgFieldDef* def = GetFieldDef(field);
    if (!def) {
      PERFETTO_DLOG("Ignoring unknown memcg field %d", static_cast<int>(field));
      continue;
    }
    if (IsAllField(*def)) {
      ret |= MemcgFieldsInFile(def->file);
    } else {
      ret.set(static_cast<size_t>(field));
    }
  }
  return ret;
}

MemcgFileSet MemcgFilesForFields(const MemcgFieldSet& fields) {
  MemcgFileSet ret;
  for (size_t i = 0; i < kMemcgFileCount; i++) {
    if ((fields & MemcgFieldsInFile(static_cast<MemcgFile>(i))).any())
      ret.set(i);
  }
  return ret;
}

void ParseMemcgFile(MemcgFile file,
                    std::string_view contents,
                    const MemcgFieldSet& wanted,
                    MemcgValues* out) {
  if (IsKeyedMemcgFile(file)) {
    ParseMemcgKeyedFile(file, contents, wanted, out);
    return;
  }
  // Single-valued files host one field.
  MemcgFieldSet fields = wanted & MemcgFieldsInFile(file);
  std::optional<uint64_t> value = ParseMemcgSingleValue(contents);
  if (fields.none() || !value.has_value())
    return;
  for (size_t i = 0; i < fields.size(); i++) {
    if (fields.test(i)) {
      out->Set(static_cast<MemcgField>(i), *value);
      break;
    }
  }
}

void SerializeMemcgValues(const MemcgValues& values,
                          protos::pbzero::MemcgStats_Cgroup* out) {
  for (const MemcgFileDef& file_def : kMemcgFileDefs) {
    MemcgFieldSet present = values.present() & MemcgFieldsInFile(file_def.file);
    if (present.none())
      continue;
    // Open a nested message if this is a keyed file.
    protozero::Message* msg = out;
    if (IsKeyedMemcgFile(file_def.file))
      msg = out->BeginNestedMessage<protozero::Message>(
          file_def.nested_proto_field);
    for (size_t i = 0; i < kMemcgFieldCount; i++) {
      if (!present.test(i))
        continue;
      msg->AppendVarInt(kMemcgFieldDefs[i].proto_field,
                        *values.Get(static_cast<MemcgField>(i)));
    }
  }
}

}  // namespace perfetto
