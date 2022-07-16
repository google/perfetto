/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/trace_processor/dynamic/flamegraph_construction_algorithms.h"

#include <set>
#include <unordered_set>

#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {
struct MergedCallsite {
  StringId frame_name;
  StringId mapping_name;
  base::Optional<StringId> source_file;
  base::Optional<uint32_t> line_number;
  base::Optional<uint32_t> parent_idx;
  bool operator<(const MergedCallsite& o) const {
    return std::tie(frame_name, mapping_name, parent_idx) <
           std::tie(o.frame_name, o.mapping_name, o.parent_idx);
  }
};

struct FlamegraphTableAndMergedCallsites {
  std::unique_ptr<tables::ExperimentalFlamegraphNodesTable> tbl;
  std::vector<uint32_t> callsite_to_merged_callsite;
};

std::vector<MergedCallsite> GetMergedCallsites(TraceStorage* storage,
                                               uint32_t callstack_row) {
  const tables::StackProfileCallsiteTable& callsites_tbl =
      storage->stack_profile_callsite_table();
  const tables::StackProfileFrameTable& frames_tbl =
      storage->stack_profile_frame_table();
  const tables::SymbolTable& symbols_tbl = storage->symbol_table();
  const tables::StackProfileMappingTable& mapping_tbl =
      storage->stack_profile_mapping_table();

  uint32_t frame_idx =
      *frames_tbl.id().IndexOf(callsites_tbl.frame_id()[callstack_row]);

  uint32_t mapping_idx =
      *mapping_tbl.id().IndexOf(frames_tbl.mapping()[frame_idx]);
  StringId mapping_name = mapping_tbl.name()[mapping_idx];

  base::Optional<uint32_t> symbol_set_id =
      frames_tbl.symbol_set_id()[frame_idx];

  if (!symbol_set_id) {
    StringId frame_name = frames_tbl.name()[frame_idx];
    base::Optional<StringId> deobfuscated_name =
        frames_tbl.deobfuscated_name()[frame_idx];
    return {{deobfuscated_name ? *deobfuscated_name : frame_name, mapping_name,
             base::nullopt, base::nullopt, base::nullopt}};
  }

  std::vector<MergedCallsite> result;
  // id == symbol_set_id for the bottommost frame.
  // TODO(lalitm): Encode this optimization in the table and remove this
  // custom optimization.
  uint32_t symbol_set_idx = *symbols_tbl.id().IndexOf(SymbolId(*symbol_set_id));
  for (uint32_t i = symbol_set_idx;
       i < symbols_tbl.row_count() &&
       symbols_tbl.symbol_set_id()[i] == *symbol_set_id;
       ++i) {
    result.emplace_back(MergedCallsite{
        symbols_tbl.name()[i], mapping_name, symbols_tbl.source_file()[i],
        symbols_tbl.line_number()[i], base::nullopt});
  }
  std::reverse(result.begin(), result.end());
  return result;
}
}  // namespace

static FlamegraphTableAndMergedCallsites BuildFlamegraphTableTreeStructure(
    TraceStorage* storage,
    base::Optional<UniquePid> upid,
    base::Optional<std::string> upid_group,
    int64_t default_timestamp,
    StringId profile_type) {
  const tables::StackProfileCallsiteTable& callsites_tbl =
      storage->stack_profile_callsite_table();

  std::vector<uint32_t> callsite_to_merged_callsite(callsites_tbl.row_count(),
                                                    0);
  std::map<MergedCallsite, uint32_t> merged_callsites_to_table_idx;

  std::unique_ptr<tables::ExperimentalFlamegraphNodesTable> tbl(
      new tables::ExperimentalFlamegraphNodesTable(
          storage->mutable_string_pool(), nullptr));

  // FORWARD PASS:
  // Aggregate callstacks by frame name / mapping name. Use symbolization
  // data.
  for (uint32_t i = 0; i < callsites_tbl.row_count(); ++i) {
    base::Optional<uint32_t> parent_idx;

    auto opt_parent_id = callsites_tbl.parent_id()[i];
    if (opt_parent_id) {
      parent_idx = callsites_tbl.id().IndexOf(*opt_parent_id);
      // Make sure what we index into has been populated already.
      PERFETTO_CHECK(*parent_idx < i);
      parent_idx = callsite_to_merged_callsite[*parent_idx];
    }

    auto callsites = GetMergedCallsites(storage, i);
    // Loop below needs to run at least once for parent_idx to get updated.
    PERFETTO_CHECK(!callsites.empty());
    std::map<MergedCallsite, uint32_t> callsites_to_rowid;
    for (MergedCallsite& merged_callsite : callsites) {
      merged_callsite.parent_idx = parent_idx;
      auto it = merged_callsites_to_table_idx.find(merged_callsite);
      if (it == merged_callsites_to_table_idx.end()) {
        std::tie(it, std::ignore) = merged_callsites_to_table_idx.emplace(
            merged_callsite, merged_callsites_to_table_idx.size());
        tables::ExperimentalFlamegraphNodesTable::Row row{};
        if (parent_idx) {
          row.depth = tbl->depth()[*parent_idx] + 1;
        } else {
          row.depth = 0;
        }

        // The 'ts' column is given a default value, taken from the query.
        // So if the query is:
        // `select * form experimental_flamegraph
        //  where ts = 605908369259172
        //  and upid = 1
        //  and profile_type = 'native'`
        // then row.ts == 605908369259172, for all rows
        // This is not accurate. However, at present there is no other
        // straightforward way of assigning timestamps to non-leaf nodes in the
        // flamegraph tree. Non-leaf nodes would have to be assigned >= 1
        // timestamps, which would increase data size without an advantage.
        row.ts = default_timestamp;
        if (upid) {
          row.upid = *upid;
        }
        if (upid_group) {
          row.upid_group = storage->InternString(base::StringView(*upid_group));
        }
        row.profile_type = profile_type;
        row.name = merged_callsite.frame_name;
        row.map_name = merged_callsite.mapping_name;
        if (parent_idx)
          row.parent_id = tbl->id()[*parent_idx];
        tbl->Insert(row);
        callsites_to_rowid[merged_callsite] =
            static_cast<uint32_t>(merged_callsites_to_table_idx.size() - 1);

        PERFETTO_CHECK(merged_callsites_to_table_idx.size() ==
                       tbl->row_count());
      } else {
        MergedCallsite saved_callsite = it->first;
        callsites_to_rowid.erase(saved_callsite);
        if (saved_callsite.source_file != merged_callsite.source_file) {
          saved_callsite.source_file = base::nullopt;
        }
        if (saved_callsite.line_number != merged_callsite.line_number) {
          saved_callsite.line_number = base::nullopt;
        }
        callsites_to_rowid[saved_callsite] = it->second;
      }
      parent_idx = it->second;
    }

    for (const auto& it : callsites_to_rowid) {
      if (it.first.source_file) {
        tbl->mutable_source_file()->Set(it.second, *it.first.source_file);
      }
      if (it.first.line_number) {
        tbl->mutable_line_number()->Set(it.second, *it.first.line_number);
      }
    }

    PERFETTO_CHECK(parent_idx);
    callsite_to_merged_callsite[i] = *parent_idx;
  }

  return {std::move(tbl), callsite_to_merged_callsite};
}

static std::unique_ptr<tables::ExperimentalFlamegraphNodesTable>
BuildFlamegraphTableHeapSizeAndCount(
    std::unique_ptr<tables::ExperimentalFlamegraphNodesTable> tbl,
    const std::vector<uint32_t>& callsite_to_merged_callsite,
    const Table& filtered) {
  for (auto it = filtered.IterateRows(); it; it.Next()) {
    int64_t size =
        it.Get(static_cast<uint32_t>(
                   tables::HeapProfileAllocationTable::ColumnIndex::size))
            .long_value;
    int64_t count =
        it.Get(static_cast<uint32_t>(
                   tables::HeapProfileAllocationTable::ColumnIndex::count))
            .long_value;
    int64_t callsite_id =
        it
            .Get(static_cast<uint32_t>(
                tables::HeapProfileAllocationTable::ColumnIndex::callsite_id))
            .long_value;

    PERFETTO_CHECK((size <= 0 && count <= 0) || (size >= 0 && count >= 0));
    uint32_t merged_idx =
        callsite_to_merged_callsite[static_cast<unsigned long>(callsite_id)];
    // On old heapprofd producers, the count field is incorrectly set and we
    // zero it in proto_trace_parser.cc.
    // As such, we cannot depend on count == 0 to imply size == 0, so we check
    // for both of them separately.
    if (size > 0) {
      tbl->mutable_alloc_size()->Set(merged_idx,
                                     tbl->alloc_size()[merged_idx] + size);
    }
    if (count > 0) {
      tbl->mutable_alloc_count()->Set(merged_idx,
                                      tbl->alloc_count()[merged_idx] + count);
    }

    tbl->mutable_size()->Set(merged_idx, tbl->size()[merged_idx] + size);
    tbl->mutable_count()->Set(merged_idx, tbl->count()[merged_idx] + count);
  }

  // BACKWARD PASS:
  // Propagate sizes to parents.
  for (int64_t i = tbl->row_count() - 1; i >= 0; --i) {
    auto idx = static_cast<uint32_t>(i);

    tbl->mutable_cumulative_size()->Set(
        idx, tbl->cumulative_size()[idx] + tbl->size()[idx]);
    tbl->mutable_cumulative_count()->Set(
        idx, tbl->cumulative_count()[idx] + tbl->count()[idx]);

    tbl->mutable_cumulative_alloc_size()->Set(
        idx, tbl->cumulative_alloc_size()[idx] + tbl->alloc_size()[idx]);
    tbl->mutable_cumulative_alloc_count()->Set(
        idx, tbl->cumulative_alloc_count()[idx] + tbl->alloc_count()[idx]);

    auto parent = tbl->parent_id()[idx];
    if (parent) {
      uint32_t parent_idx = *tbl->id().IndexOf(
          tables::ExperimentalFlamegraphNodesTable::Id(*parent));
      tbl->mutable_cumulative_size()->Set(
          parent_idx,
          tbl->cumulative_size()[parent_idx] + tbl->cumulative_size()[idx]);
      tbl->mutable_cumulative_count()->Set(
          parent_idx,
          tbl->cumulative_count()[parent_idx] + tbl->cumulative_count()[idx]);

      tbl->mutable_cumulative_alloc_size()->Set(
          parent_idx, tbl->cumulative_alloc_size()[parent_idx] +
                          tbl->cumulative_alloc_size()[idx]);
      tbl->mutable_cumulative_alloc_count()->Set(
          parent_idx, tbl->cumulative_alloc_count()[parent_idx] +
                          tbl->cumulative_alloc_count()[idx]);
    }
  }

  return tbl;
}

static std::unique_ptr<tables::ExperimentalFlamegraphNodesTable>
BuildFlamegraphTableCallstackSizeAndCount(
    std::unique_ptr<tables::ExperimentalFlamegraphNodesTable> tbl,
    const std::vector<uint32_t>& callsite_to_merged_callsite,
    const Table& filtered) {
  for (auto it = filtered.IterateRows(); it; it.Next()) {
    int64_t callsite_id =
        it.Get(static_cast<uint32_t>(
                   tables::PerfSampleTable::ColumnIndex::callsite_id))
            .long_value;
    int64_t ts =
        it.Get(static_cast<uint32_t>(tables::PerfSampleTable::ColumnIndex::ts))
            .long_value;

    uint32_t merged_idx =
        callsite_to_merged_callsite[static_cast<unsigned long>(callsite_id)];
    tbl->mutable_size()->Set(merged_idx, tbl->size()[merged_idx] + 1);
    tbl->mutable_count()->Set(merged_idx, tbl->count()[merged_idx] + 1);
    tbl->mutable_ts()->Set(merged_idx, ts);
  }

  // BACKWARD PASS:
  // Propagate sizes to parents.
  for (int64_t i = tbl->row_count() - 1; i >= 0; --i) {
    auto idx = static_cast<uint32_t>(i);

    tbl->mutable_cumulative_size()->Set(
        idx, tbl->cumulative_size()[idx] + tbl->size()[idx]);
    tbl->mutable_cumulative_count()->Set(
        idx, tbl->cumulative_count()[idx] + tbl->count()[idx]);

    auto parent = tbl->parent_id()[idx];
    if (parent) {
      uint32_t parent_idx = *tbl->id().IndexOf(
          tables::ExperimentalFlamegraphNodesTable::Id(*parent));
      tbl->mutable_cumulative_size()->Set(
          parent_idx,
          tbl->cumulative_size()[parent_idx] + tbl->cumulative_size()[idx]);
      tbl->mutable_cumulative_count()->Set(
          parent_idx,
          tbl->cumulative_count()[parent_idx] + tbl->cumulative_count()[idx]);
    }
  }
  return tbl;
}

std::unique_ptr<tables::ExperimentalFlamegraphNodesTable>
BuildHeapProfileFlamegraph(TraceStorage* storage,
                           UniquePid upid,
                           int64_t timestamp) {
  const tables::HeapProfileAllocationTable& allocation_tbl =
      storage->heap_profile_allocation_table();
  // PASS OVER ALLOCATIONS:
  // Aggregate allocations into the newly built tree.
  auto filtered = allocation_tbl.Filter(
      {allocation_tbl.ts().le(timestamp), allocation_tbl.upid().eq(upid)});
  if (filtered.row_count() == 0) {
    return nullptr;
  }
  StringId profile_type = storage->InternString("native");
  FlamegraphTableAndMergedCallsites table_and_callsites =
      BuildFlamegraphTableTreeStructure(storage, upid, base::nullopt, timestamp,
                                        profile_type);
  return BuildFlamegraphTableHeapSizeAndCount(
      std::move(table_and_callsites.tbl),
      table_and_callsites.callsite_to_merged_callsite, filtered);
}

std::unique_ptr<tables::ExperimentalFlamegraphNodesTable>
BuildNativeCallStackSamplingFlamegraph(
    TraceStorage* storage,
    base::Optional<UniquePid> upid,
    base::Optional<std::string> upid_group,
    const std::vector<TimeConstraints>& time_constraints) {
  // 1.Extract required upids from input.
  std::unordered_set<UniquePid> upids;
  if (upid) {
    upids.insert(*upid);
  } else {
    for (base::StringSplitter sp(*upid_group, ','); sp.Next();) {
      base::Optional<uint32_t> maybe = base::CStringToUInt32(sp.cur_token());
      if (maybe) {
        upids.insert(*maybe);
      }
    }
  }

  // 2.Create set of all utids mapped to the given vector of upids
  std::set<tables::ThreadTable::Id> utids;
  RowMap threads_in_pid_rm;
  for (uint32_t i = 0; i < storage->thread_table().row_count(); ++i) {
    base::Optional<uint32_t> row_upid = storage->thread_table().upid()[i];
    if (row_upid && upids.count(*row_upid) > 0) {
      threads_in_pid_rm.Insert(i);
    }
  }

  for (auto it = threads_in_pid_rm.IterateRows(); it; it.Next()) {
    utids.insert(storage->thread_table().id()[it.index()]);
  }

  // 3.Get all row indices in perf_sample that have callstacks (some samples
  // can have only counter values) and correspond to the requested utids.
  std::vector<uint32_t> cs_rows;
  for (uint32_t i = 0; i < storage->perf_sample_table().row_count(); ++i) {
    if (storage->perf_sample_table().callsite_id()[i].has_value() &&
        utids.find(static_cast<tables::ThreadTable::Id>(
            storage->perf_sample_table().utid()[i])) != utids.end()) {
      cs_rows.push_back(i);
    }
  }

  // 4.Filter rows that correspond to the selected utids
  RowMap filtered_rm = RowMap(std::move(cs_rows));
  Table filtered = storage->perf_sample_table().Apply(std::move(filtered_rm));

  // 5.Filter rows by time constraints
  for (const auto& tc : time_constraints) {
    if (!(tc.op == FilterOp::kGt || tc.op == FilterOp::kLt ||
          tc.op == FilterOp::kGe || tc.op == FilterOp::kLe)) {
      PERFETTO_FATAL("Filter operation %d not permitted for perf.",
                     static_cast<int>(tc.op));
    }
    Constraint cs = Constraint{
        static_cast<uint32_t>(tables::PerfSampleTable::ColumnIndex::ts), tc.op,
        SqlValue::Long(tc.value)};
    filtered = filtered.Filter({cs});
  }
  if (filtered.row_count() == 0) {
    std::unique_ptr<tables::ExperimentalFlamegraphNodesTable> empty_tbl(
        new tables::ExperimentalFlamegraphNodesTable(
            storage->mutable_string_pool(), nullptr));
    return empty_tbl;
  }

  // The logic underneath is selecting a default timestamp to be used by all
  // frames which do not have a timestamp. The timestamp is taken from the query
  // value and it's not meaningful for the row. It prevents however the rows
  // with no timestamp from being filtered out by Sqlite, after we create the
  // table ExperimentalFlamegraphNodesTable in this class.
  int64_t default_timestamp = 0;
  if (!time_constraints.empty()) {
    auto& tc = time_constraints[0];
    if (tc.op == FilterOp::kGt) {
      default_timestamp = tc.value + 1;
    } else if (tc.op == FilterOp::kLt) {
      default_timestamp = tc.value - 1;
    } else {
      default_timestamp = tc.value;
    }
  }
  StringId profile_type = storage->InternString("perf");
  FlamegraphTableAndMergedCallsites table_and_callsites =
      BuildFlamegraphTableTreeStructure(storage, upid, upid_group,
                                        default_timestamp, profile_type);
  return BuildFlamegraphTableCallstackSizeAndCount(
      std::move(table_and_callsites.tbl),
      table_and_callsites.callsite_to_merged_callsite, filtered);
}

}  // namespace trace_processor
}  // namespace perfetto
