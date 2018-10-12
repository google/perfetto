/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_TRACED_SYS_STATS_COUNTERS_H_
#define INCLUDE_PERFETTO_TRACED_SYS_STATS_COUNTERS_H_

#include "perfetto/base/utils.h"
#include "perfetto/common/sys_stats_counters.pb.h"

#include <vector>

namespace perfetto {

struct KeyAndId {
  const char* str;
  int id;
};

constexpr KeyAndId kMeminfoKeys[] = {
    {"MemUnspecified", protos::MeminfoCounters::MEMINFO_UNSPECIFIED},
    {"MemTotal", protos::MeminfoCounters::MEMINFO_MEM_TOTAL},
    {"MemFree", protos::MeminfoCounters::MEMINFO_MEM_FREE},
    {"MemAvailable", protos::MeminfoCounters::MEMINFO_MEM_AVAILABLE},
    {"Buffers", protos::MeminfoCounters::MEMINFO_BUFFERS},
    {"Cached", protos::MeminfoCounters::MEMINFO_CACHED},
    {"SwapCached", protos::MeminfoCounters::MEMINFO_SWAP_CACHED},
    {"Active", protos::MeminfoCounters::MEMINFO_ACTIVE},
    {"Inactive", protos::MeminfoCounters::MEMINFO_INACTIVE},
    {"Active(anon)", protos::MeminfoCounters::MEMINFO_ACTIVE_ANON},
    {"Inactive(anon)", protos::MeminfoCounters::MEMINFO_INACTIVE_ANON},
    {"Active(file)", protos::MeminfoCounters::MEMINFO_ACTIVE_FILE},
    {"Inactive(file)", protos::MeminfoCounters::MEMINFO_INACTIVE_FILE},
    {"Unevictable", protos::MeminfoCounters::MEMINFO_UNEVICTABLE},
    {"Mlocked", protos::MeminfoCounters::MEMINFO_MLOCKED},
    {"SwapTotal", protos::MeminfoCounters::MEMINFO_SWAP_TOTAL},
    {"SwapFree", protos::MeminfoCounters::MEMINFO_SWAP_FREE},
    {"Dirty", protos::MeminfoCounters::MEMINFO_DIRTY},
    {"Writeback", protos::MeminfoCounters::MEMINFO_WRITEBACK},
    {"AnonPages", protos::MeminfoCounters::MEMINFO_ANON_PAGES},
    {"Mapped", protos::MeminfoCounters::MEMINFO_MAPPED},
    {"Shmem", protos::MeminfoCounters::MEMINFO_SHMEM},
    {"Slab", protos::MeminfoCounters::MEMINFO_SLAB},
    {"SReclaimable", protos::MeminfoCounters::MEMINFO_SLAB_RECLAIMABLE},
    {"SUnreclaim", protos::MeminfoCounters::MEMINFO_SLAB_UNRECLAIMABLE},
    {"KernelStack", protos::MeminfoCounters::MEMINFO_KERNEL_STACK},
    {"PageTables", protos::MeminfoCounters::MEMINFO_PAGE_TABLES},
    {"CommitLimit", protos::MeminfoCounters::MEMINFO_COMMIT_LIMIT},
    {"Committed_AS", protos::MeminfoCounters::MEMINFO_COMMITED_AS},
    {"VmallocTotal", protos::MeminfoCounters::MEMINFO_VMALLOC_TOTAL},
    {"VmallocUsed", protos::MeminfoCounters::MEMINFO_VMALLOC_USED},
    {"VmallocChunk", protos::MeminfoCounters::MEMINFO_VMALLOC_CHUNK},
    {"CmaTotal", protos::MeminfoCounters::MEMINFO_CMA_TOTAL},
    {"CmaFree", protos::MeminfoCounters::MEMINFO_CMA_FREE},
};

const KeyAndId kVmstatKeys[] = {
    {"VmstatUnspecified", protos::VmstatCounters::VMSTAT_UNSPECIFIED},
    {"nr_free_pages", protos::VmstatCounters::VMSTAT_NR_FREE_PAGES},
    {"nr_alloc_batch", protos::VmstatCounters::VMSTAT_NR_ALLOC_BATCH},
    {"nr_inactive_anon", protos::VmstatCounters::VMSTAT_NR_INACTIVE_ANON},
    {"nr_active_anon", protos::VmstatCounters::VMSTAT_NR_ACTIVE_ANON},
    {"nr_inactive_file", protos::VmstatCounters::VMSTAT_NR_INACTIVE_FILE},
    {"nr_active_file", protos::VmstatCounters::VMSTAT_NR_ACTIVE_FILE},
    {"nr_unevictable", protos::VmstatCounters::VMSTAT_NR_UNEVICTABLE},
    {"nr_mlock", protos::VmstatCounters::VMSTAT_NR_MLOCK},
    {"nr_anon_pages", protos::VmstatCounters::VMSTAT_NR_ANON_PAGES},
    {"nr_mapped", protos::VmstatCounters::VMSTAT_NR_MAPPED},
    {"nr_file_pages", protos::VmstatCounters::VMSTAT_NR_FILE_PAGES},
    {"nr_dirty", protos::VmstatCounters::VMSTAT_NR_DIRTY},
    {"nr_writeback", protos::VmstatCounters::VMSTAT_NR_WRITEBACK},
    {"nr_slab_reclaimable", protos::VmstatCounters::VMSTAT_NR_SLAB_RECLAIMABLE},
    {"nr_slab_unreclaimable",
     protos::VmstatCounters::VMSTAT_NR_SLAB_UNRECLAIMABLE},
    {"nr_page_table_pages", protos::VmstatCounters::VMSTAT_NR_PAGE_TABLE_PAGES},
    {"nr_kernel_stack", protos::VmstatCounters::VMSTAT_NR_KERNEL_STACK},
    {"nr_overhead", protos::VmstatCounters::VMSTAT_NR_OVERHEAD},
    {"nr_unstable", protos::VmstatCounters::VMSTAT_NR_UNSTABLE},
    {"nr_bounce", protos::VmstatCounters::VMSTAT_NR_BOUNCE},
    {"nr_vmscan_write", protos::VmstatCounters::VMSTAT_NR_VMSCAN_WRITE},
    {"nr_vmscan_immediate_reclaim",
     protos::VmstatCounters::VMSTAT_NR_VMSCAN_IMMEDIATE_RECLAIM},
    {"nr_writeback_temp", protos::VmstatCounters::VMSTAT_NR_WRITEBACK_TEMP},
    {"nr_isolated_anon", protos::VmstatCounters::VMSTAT_NR_ISOLATED_ANON},
    {"nr_isolated_file", protos::VmstatCounters::VMSTAT_NR_ISOLATED_FILE},
    {"nr_shmem", protos::VmstatCounters::VMSTAT_NR_SHMEM},
    {"nr_dirtied", protos::VmstatCounters::VMSTAT_NR_DIRTIED},
    {"nr_written", protos::VmstatCounters::VMSTAT_NR_WRITTEN},
    {"nr_pages_scanned", protos::VmstatCounters::VMSTAT_NR_PAGES_SCANNED},
    {"workingset_refault", protos::VmstatCounters::VMSTAT_WORKINGSET_REFAULT},
    {"workingset_activate", protos::VmstatCounters::VMSTAT_WORKINGSET_ACTIVATE},
    {"workingset_nodereclaim",
     protos::VmstatCounters::VMSTAT_WORKINGSET_NODERECLAIM},
    {"nr_anon_transparent_hugepages",
     protos::VmstatCounters::VMSTAT_NR_ANON_TRANSPARENT_HUGEPAGES},
    {"nr_free_cma", protos::VmstatCounters::VMSTAT_NR_FREE_CMA},
    {"nr_swapcache", protos::VmstatCounters::VMSTAT_NR_SWAPCACHE},
    {"nr_dirty_threshold", protos::VmstatCounters::VMSTAT_NR_DIRTY_THRESHOLD},
    {"nr_dirty_background_threshold",
     protos::VmstatCounters::VMSTAT_NR_DIRTY_BACKGROUND_THRESHOLD},
    {"pgpgin", protos::VmstatCounters::VMSTAT_PGPGIN},
    {"pgpgout", protos::VmstatCounters::VMSTAT_PGPGOUT},
    {"pgpgoutclean", protos::VmstatCounters::VMSTAT_PGPGOUTCLEAN},
    {"pswpin", protos::VmstatCounters::VMSTAT_PSWPIN},
    {"pswpout", protos::VmstatCounters::VMSTAT_PSWPOUT},
    {"pgalloc_dma", protos::VmstatCounters::VMSTAT_PGALLOC_DMA},
    {"pgalloc_normal", protos::VmstatCounters::VMSTAT_PGALLOC_NORMAL},
    {"pgalloc_movable", protos::VmstatCounters::VMSTAT_PGALLOC_MOVABLE},
    {"pgfree", protos::VmstatCounters::VMSTAT_PGFREE},
    {"pgactivate", protos::VmstatCounters::VMSTAT_PGACTIVATE},
    {"pgdeactivate", protos::VmstatCounters::VMSTAT_PGDEACTIVATE},
    {"pgfault", protos::VmstatCounters::VMSTAT_PGFAULT},
    {"pgmajfault", protos::VmstatCounters::VMSTAT_PGMAJFAULT},
    {"pgrefill_dma", protos::VmstatCounters::VMSTAT_PGREFILL_DMA},
    {"pgrefill_normal", protos::VmstatCounters::VMSTAT_PGREFILL_NORMAL},
    {"pgrefill_movable", protos::VmstatCounters::VMSTAT_PGREFILL_MOVABLE},
    {"pgsteal_kswapd_dma", protos::VmstatCounters::VMSTAT_PGSTEAL_KSWAPD_DMA},
    {"pgsteal_kswapd_normal",
     protos::VmstatCounters::VMSTAT_PGSTEAL_KSWAPD_NORMAL},
    {"pgsteal_kswapd_movable",
     protos::VmstatCounters::VMSTAT_PGSTEAL_KSWAPD_MOVABLE},
    {"pgsteal_direct_dma", protos::VmstatCounters::VMSTAT_PGSTEAL_DIRECT_DMA},
    {"pgsteal_direct_normal",
     protos::VmstatCounters::VMSTAT_PGSTEAL_DIRECT_NORMAL},
    {"pgsteal_direct_movable",
     protos::VmstatCounters::VMSTAT_PGSTEAL_DIRECT_MOVABLE},
    {"pgscan_kswapd_dma", protos::VmstatCounters::VMSTAT_PGSCAN_KSWAPD_DMA},
    {"pgscan_kswapd_normal",
     protos::VmstatCounters::VMSTAT_PGSCAN_KSWAPD_NORMAL},
    {"pgscan_kswapd_movable",
     protos::VmstatCounters::VMSTAT_PGSCAN_KSWAPD_MOVABLE},
    {"pgscan_direct_dma", protos::VmstatCounters::VMSTAT_PGSCAN_DIRECT_DMA},
    {"pgscan_direct_normal",
     protos::VmstatCounters::VMSTAT_PGSCAN_DIRECT_NORMAL},
    {"pgscan_direct_movable",
     protos::VmstatCounters::VMSTAT_PGSCAN_DIRECT_MOVABLE},
    {"pgscan_direct_throttle",
     protos::VmstatCounters::VMSTAT_PGSCAN_DIRECT_THROTTLE},
    {"pginodesteal", protos::VmstatCounters::VMSTAT_PGINODESTEAL},
    {"slabs_scanned", protos::VmstatCounters::VMSTAT_SLABS_SCANNED},
    {"kswapd_inodesteal", protos::VmstatCounters::VMSTAT_KSWAPD_INODESTEAL},
    {"kswapd_low_wmark_hit_quickly",
     protos::VmstatCounters::VMSTAT_KSWAPD_LOW_WMARK_HIT_QUICKLY},
    {"kswapd_high_wmark_hit_quickly",
     protos::VmstatCounters::VMSTAT_KSWAPD_HIGH_WMARK_HIT_QUICKLY},
    {"pageoutrun", protos::VmstatCounters::VMSTAT_PAGEOUTRUN},
    {"allocstall", protos::VmstatCounters::VMSTAT_ALLOCSTALL},
    {"pgrotated", protos::VmstatCounters::VMSTAT_PGROTATED},
    {"drop_pagecache", protos::VmstatCounters::VMSTAT_DROP_PAGECACHE},
    {"drop_slab", protos::VmstatCounters::VMSTAT_DROP_SLAB},
    {"pgmigrate_success", protos::VmstatCounters::VMSTAT_PGMIGRATE_SUCCESS},
    {"pgmigrate_fail", protos::VmstatCounters::VMSTAT_PGMIGRATE_FAIL},
    {"compact_migrate_scanned",
     protos::VmstatCounters::VMSTAT_COMPACT_MIGRATE_SCANNED},
    {"compact_free_scanned",
     protos::VmstatCounters::VMSTAT_COMPACT_FREE_SCANNED},
    {"compact_isolated", protos::VmstatCounters::VMSTAT_COMPACT_ISOLATED},
    {"compact_stall", protos::VmstatCounters::VMSTAT_COMPACT_STALL},
    {"compact_fail", protos::VmstatCounters::VMSTAT_COMPACT_FAIL},
    {"compact_success", protos::VmstatCounters::VMSTAT_COMPACT_SUCCESS},
    {"compact_daemon_wake", protos::VmstatCounters::VMSTAT_COMPACT_DAEMON_WAKE},
    {"unevictable_pgs_culled",
     protos::VmstatCounters::VMSTAT_UNEVICTABLE_PGS_CULLED},
    {"unevictable_pgs_scanned",
     protos::VmstatCounters::VMSTAT_UNEVICTABLE_PGS_SCANNED},
    {"unevictable_pgs_rescued",
     protos::VmstatCounters::VMSTAT_UNEVICTABLE_PGS_RESCUED},
    {"unevictable_pgs_mlocked",
     protos::VmstatCounters::VMSTAT_UNEVICTABLE_PGS_MLOCKED},
    {"unevictable_pgs_munlocked",
     protos::VmstatCounters::VMSTAT_UNEVICTABLE_PGS_MUNLOCKED},
    {"unevictable_pgs_cleared",
     protos::VmstatCounters::VMSTAT_UNEVICTABLE_PGS_CLEARED},
    {"unevictable_pgs_stranded",
     protos::VmstatCounters::VMSTAT_UNEVICTABLE_PGS_STRANDED},
};

// Returns a lookup table of meminfo counter names addressable by counter id.
inline std::vector<const char*> BuildMeminfoCounterNames() {
  int max_id = 0;
  for (size_t i = 0; i < base::ArraySize(kMeminfoKeys); i++)
    max_id = std::max(max_id, kMeminfoKeys[i].id);
  std::vector<const char*> v;
  v.resize(static_cast<size_t>(max_id) + 1);
  for (size_t i = 0; i < base::ArraySize(kMeminfoKeys); i++)
    v[static_cast<size_t>(kMeminfoKeys[i].id)] = kMeminfoKeys[i].str;
  return v;
}

inline std::vector<const char*> BuildVmstatCounterNames() {
  int max_id = 0;
  for (size_t i = 0; i < base::ArraySize(kVmstatKeys); i++)
    max_id = std::max(max_id, kVmstatKeys[i].id);
  std::vector<const char*> v;
  v.resize(static_cast<size_t>(max_id) + 1);
  for (size_t i = 0; i < base::ArraySize(kVmstatKeys); i++)
    v[static_cast<size_t>(kVmstatKeys[i].id)] = kVmstatKeys[i].str;
  return v;
}

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACED_SYS_STATS_COUNTERS_H_
