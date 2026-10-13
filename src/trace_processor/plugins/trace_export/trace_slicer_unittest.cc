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

#include "src/trace_processor/plugins/trace_export/trace_slicer.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/core/plugin/registration.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/counter_tables_py.h"
#include "src/trace_processor/tables/flow_tables_py.h"
#include "src/trace_processor/tables/memory_tables_py.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::trace_export {
namespace {

template <typename T>
PluginDataframe MakePluginDf(T* table) {
  return PluginDataframe{
      &table->dataframe(), T::Name(), {}, T::ForeignKeyTables()};
}

TEST(TraceSlicerTest, RejectsInvalidOptions) {
  TraceStorage storage;
  std::vector<PluginDataframe> dfs = {
      MakePluginDf(storage.mutable_slice_table())};

  {
    TraceProcessor::ExportOptions opts;
    opts.start_ts = -1;
    EXPECT_FALSE(TraceSlicer::Slice(dfs, opts).ok());
  }
  {
    TraceProcessor::ExportOptions opts;
    opts.end_ts = -5;
    EXPECT_FALSE(TraceSlicer::Slice(dfs, opts).ok());
  }
  {
    TraceProcessor::ExportOptions opts;
    opts.start_ts = 200;
    opts.end_ts = 100;
    EXPECT_FALSE(TraceSlicer::Slice(dfs, opts).ok());
  }
}

TEST(TraceSlicerTest, IntervalClampingParentRemapAndFlowFiltering) {
  TraceStorage storage;
  auto* tracks = storage.mutable_track_table();
  auto* slices = storage.mutable_slice_table();
  auto* flows = storage.mutable_flow_table();

  auto track_id = tracks->Insert({}).id;

  // Row 0: [10, 30) - outside before [50, 150], should be dropped.
  auto s0 =
      slices->Insert({10, 20, track_id, {}, {}, 0, {}, {}, {}, {}, {}}).id;

  // Row 1: [40, 160) - straddles both bounds of [50, 150], clamped to [50, 150)
  // (ts=50, dur=100), remapped from id 1 -> id 0.
  auto s1 =
      slices->Insert({40, 120, track_id, {}, {}, 0, {}, {}, {}, {}, {}}).id;

  // Row 2: [60, 120) - child of s1, inside [50, 150], remapped from id 2 ->
  // id 1, parent_id remapped from 1 -> 0, depth remains 1.
  auto s2 =
      slices->Insert({60, 60, track_id, {}, {}, 1, s1, {}, {}, {}, {}}).id;

  // Row 3: [180, 200) - outside after [50, 150], should be dropped.
  auto s3 =
      slices->Insert({180, 20, track_id, {}, {}, 0, {}, {}, {}, {}, {}}).id;

  // Flow 0: s0 -> s1 (s0 dropped => flow dropped).
  flows->Insert({s0, s1, {}, {}});
  // Flow 1: s1 -> s2 (both survive => flow kept, remapped to 0 -> 1).
  flows->Insert({s1, s2, {}, {}});
  // Flow 2: s2 -> s3 (s3 dropped => flow dropped).
  flows->Insert({s2, s3, {}, {}});

  tracks->dataframe().Finalize();
  slices->dataframe().Finalize();
  flows->dataframe().Finalize();

  std::vector<PluginDataframe> dfs = {
      MakePluginDf(tracks),
      MakePluginDf(slices),
      MakePluginDf(flows),
  };

  TraceProcessor::ExportOptions opts;
  opts.start_ts = 50;
  opts.end_ts = 150;
  auto sliced_or = TraceSlicer::Slice(dfs, opts);
  ASSERT_TRUE(sliced_or.ok()) << sliced_or.status().c_message();

  const auto& out_slices = *sliced_or->dataframes[1].dataframe;
  ASSERT_EQ(out_slices.row_count(), 2u);
  EXPECT_EQ((out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::ts>(
                tables::SliceTable::kSpec, 0)),
            50);
  EXPECT_EQ((out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::dur>(
                tables::SliceTable::kSpec, 0)),
            100);
  EXPECT_EQ(
      (out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::parent_id>(
          tables::SliceTable::kSpec, 0)),
      std::nullopt);
  EXPECT_EQ(
      (out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::depth>(
          tables::SliceTable::kSpec, 0)),
      0u);

  EXPECT_EQ((out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::ts>(
                tables::SliceTable::kSpec, 1)),
            60);
  EXPECT_EQ((out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::dur>(
                tables::SliceTable::kSpec, 1)),
            60);
  EXPECT_EQ(
      (out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::parent_id>(
          tables::SliceTable::kSpec, 1)),
      std::make_optional(0u));
  EXPECT_EQ(
      (out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::depth>(
          tables::SliceTable::kSpec, 1)),
      1u);

  const auto& out_flows = *sliced_or->dataframes[2].dataframe;
  ASSERT_EQ(out_flows.row_count(), 1u);
  EXPECT_EQ(
      (out_flows.GetCellUnchecked<tables::FlowTable::ColumnIndex::slice_out>(
          tables::FlowTable::kSpec, 0)),
      0u);
  EXPECT_EQ(
      (out_flows.GetCellUnchecked<tables::FlowTable::ColumnIndex::slice_in>(
          tables::FlowTable::kSpec, 0)),
      1u);
}

TEST(TraceSlicerTest, CounterCarryForwardAndArgsSetIdCompaction) {
  TraceStorage storage;
  auto* tracks = storage.mutable_track_table();
  auto* counters = storage.mutable_counter_table();
  auto* args = storage.mutable_arg_table();

  auto t1 = tracks->Insert({}).id;
  auto t2 = tracks->Insert({}).id;

  StringId k1 = storage.InternString("k1");
  StringId k2 = storage.InternString("k2");
  StringId int_type = storage.InternString("int");

  // Arg set 0 at row 0..1 (will be dropped because its counter row is dropped).
  args->Insert({0, k1, k1, 10, {}, {}, int_type});
  args->Insert({0, k2, k2, 20, {}, {}, int_type});
  // Arg set 2 at row 2 (will survive and be compacted to arg_set_id 0!).
  args->Insert({2, k1, k1, 99, {}, {}, int_type});

  // Track 1: sample at ts=10 (value=1.0, arg_set_id=0) and ts=30 (value=5.0,
  // arg_set_id=2) before start_ts=50 -> only ts=30 is carried forward to ts=50!
  counters->Insert({10, t1, 1.0, 0u});
  counters->Insert({30, t1, 5.0, 2u});
  counters->Insert({80, t1, 9.0, std::nullopt});

  // Track 2: sample at ts=20 (value=2.0) and exact start_ts=50 (value=7.0) ->
  // since a sample already exists at ts=50, ts=20 is NOT carried forward!
  counters->Insert({20, t2, 2.0, std::nullopt});
  counters->Insert({50, t2, 7.0, std::nullopt});

  tracks->dataframe().Finalize();
  counters->dataframe().Finalize();
  args->dataframe().Finalize();

  std::vector<PluginDataframe> dfs = {
      MakePluginDf(tracks),
      MakePluginDf(counters),
      MakePluginDf(args),
  };

  TraceProcessor::ExportOptions opts;
  opts.start_ts = 50;
  opts.end_ts = 100;
  auto sliced_or = TraceSlicer::Slice(dfs, opts);
  ASSERT_TRUE(sliced_or.ok()) << sliced_or.status().c_message();

  const auto& out_counters = *sliced_or->dataframes[1].dataframe;
  ASSERT_EQ(out_counters.row_count(), 3u);
  // Row 0: carried-forward t1 sample clamped to ts=50, value=5.0, remapped
  // arg_set_id=0.
  EXPECT_EQ(
      (out_counters.GetCellUnchecked<tables::CounterTable::ColumnIndex::ts>(
          tables::CounterTable::kSpec, 0)),
      50);
  EXPECT_DOUBLE_EQ(
      (out_counters.GetCellUnchecked<tables::CounterTable::ColumnIndex::value>(
          tables::CounterTable::kSpec, 0)),
      5.0);
  EXPECT_EQ(
      (out_counters
           .GetCellUnchecked<tables::CounterTable::ColumnIndex::arg_set_id>(
               tables::CounterTable::kSpec, 0)),
      std::make_optional(0u));

  // Row 1: t1 sample at ts=80, value=9.0.
  EXPECT_EQ(
      (out_counters.GetCellUnchecked<tables::CounterTable::ColumnIndex::ts>(
          tables::CounterTable::kSpec, 1)),
      80);
  // Row 2: t2 sample at ts=50, value=7.0.
  EXPECT_EQ(
      (out_counters.GetCellUnchecked<tables::CounterTable::ColumnIndex::ts>(
          tables::CounterTable::kSpec, 2)),
      50);
  EXPECT_DOUBLE_EQ(
      (out_counters.GetCellUnchecked<tables::CounterTable::ColumnIndex::value>(
          tables::CounterTable::kSpec, 2)),
      7.0);

  // Args table compacted from 3 rows down to 1 row with arg_set_id == 0.
  const auto& out_args = *sliced_or->dataframes[2].dataframe;
  ASSERT_EQ(out_args.row_count(), 1u);
  EXPECT_EQ(
      (out_args.GetCellUnchecked<tables::ArgTable::ColumnIndex::arg_set_id>(
          tables::ArgTable::kSpec, 0)),
      0u);
  EXPECT_EQ(
      (out_args.GetCellUnchecked<tables::ArgTable::ColumnIndex::int_value>(
          tables::ArgTable::kSpec, 0)),
      std::make_optional<int64_t>(99));
}

TEST(TraceSlicerTest, DroppedAncestorDepthRecomputationAndUnclosedSlice) {
  TraceStorage storage;
  auto* tracks = storage.mutable_track_table();
  auto* slices = storage.mutable_slice_table();

  auto track_id = tracks->Insert({}).id;

  // Row 0: [10, 10) (dur=0, depth=0) - dropped because ts < 50.
  auto root_dropped =
      slices->Insert({10, 0, track_id, {}, {}, 0, {}, {}, {}, {}, {}}).id;
  // Row 1: [60, 120) (depth=1, parent=root_dropped) - survives as id 0,
  // parent_id becomes nullopt, depth recomputed to 0.
  auto child =
      slices
          ->Insert({60, 60, track_id, {}, {}, 1, root_dropped, {}, {}, {}, {}})
          .id;
  // Row 2: [70, 90) (depth=2, parent=child) - survives as id 1, parent_id
  // remapped to 0, depth recomputed to 1.
  slices->Insert({70, 20, track_id, {}, {}, 2, child, {}, {}, {}, {}});
  // Row 3: ts=30, dur=-1 (unclosed slice starting before start_ts=50) -
  // survives as id 2 with ts clamped to 50 and dur preserved as -1.
  slices->Insert({30, -1, track_id, {}, {}, 0, {}, {}, {}, {}, {}});

  tracks->dataframe().Finalize();
  slices->dataframe().Finalize();

  std::vector<PluginDataframe> dfs = {
      MakePluginDf(tracks),
      MakePluginDf(slices),
  };

  TraceProcessor::ExportOptions opts;
  opts.start_ts = 50;
  opts.end_ts = 150;
  auto sliced_or = TraceSlicer::Slice(dfs, opts);
  ASSERT_TRUE(sliced_or.ok()) << sliced_or.status().c_message();

  const auto& out_slices = *sliced_or->dataframes[1].dataframe;
  ASSERT_EQ(out_slices.row_count(), 3u);
  EXPECT_EQ(
      (out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::parent_id>(
          tables::SliceTable::kSpec, 0)),
      std::nullopt);
  EXPECT_EQ(
      (out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::depth>(
          tables::SliceTable::kSpec, 0)),
      0u);
  EXPECT_EQ(
      (out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::parent_id>(
          tables::SliceTable::kSpec, 1)),
      std::make_optional(0u));
  EXPECT_EQ(
      (out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::depth>(
          tables::SliceTable::kSpec, 1)),
      1u);
  EXPECT_EQ((out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::ts>(
                tables::SliceTable::kSpec, 2)),
            50);
  EXPECT_EQ((out_slices.GetCellUnchecked<tables::SliceTable::ColumnIndex::dur>(
                tables::SliceTable::kSpec, 2)),
            -1);
}

TEST(TraceSlicerTest, HeapGraphProfilerSampleAndMemorySnapshotSlicing) {
  TraceStorage storage;
  auto* hg_cls = storage.mutable_heap_graph_class_table();
  auto* hg_obj = storage.mutable_heap_graph_object_table();
  auto* hg_ref = storage.mutable_heap_graph_reference_table();
  auto* tracks = storage.mutable_track_table();
  auto* counters = storage.mutable_counter_table();
  auto* prof_cset = storage.mutable_profiler_counter_set_table();
  auto* prof_samples = storage.mutable_profiler_sample_table();
  auto* mem_snap = storage.mutable_memory_snapshot_table();
  auto* proc_snap = storage.mutable_process_memory_snapshot_table();
  auto* snap_node = storage.mutable_memory_snapshot_node_table();
  auto* snap_edge = storage.mutable_memory_snapshot_edge_table();

  StringPool::Id s_cls = storage.InternString("Foo");
  StringPool::Id s_kind = storage.InternString("KIND_NORMAL");
  StringPool::Id s_field = storage.InternString("Foo.bar");
  StringPool::Id s_src = storage.InternString("linux.perf");
  StringPool::Id s_path = storage.InternString("malloc/allocated_objects");

  auto cls_id = hg_cls->Insert({s_cls, {}, {}, {}, {}, s_kind, 0}).id;
  // Dump 1 at ts=20 (dropped): obj 0 referencing itself via reference_set_id=0.
  auto obj0 =
      hg_obj->Insert({1, 20, 64, 0, 0u, 1, {}, cls_id, {}, 0, std::nullopt}).id;
  hg_ref->Insert({0u, obj0, obj0, s_field, s_cls, std::nullopt});
  // Dump 2 at ts=80 (kept): obj 1 and obj 2, with obj 1 referencing obj 2 via
  // reference_set_id=1.
  auto obj1 =
      hg_obj->Insert({1, 80, 128, 0, 1u, 1, {}, cls_id, {}, 0, std::nullopt})
          .id;
  auto obj2 =
      hg_obj
          ->Insert(
              {1, 80, 256, 0, std::nullopt, 1, {}, cls_id, {}, 0, std::nullopt})
          .id;
  hg_ref->Insert({1u, obj1, obj2, s_field, s_cls, std::nullopt});

  // Profiler samples + counter sets:
  auto track_id = tracks->Insert({}).id;
  auto c0 = counters->Insert({15, track_id, 100.0, std::nullopt}).id;
  auto c1 = counters->Insert({85, track_id, 200.0, std::nullopt}).id;
  prof_cset->Insert({0u, c0});
  prof_cset->Insert({1u, c1});
  prof_samples->Insert({15, s_src, {}, {}, {}, {}, {}, 0u});
  prof_samples->Insert({85, s_src, {}, {}, {}, {}, {}, 1u});

  // Memory snapshots at timestamp=10 (dropped) and timestamp=90 (kept):
  auto ms0 = mem_snap->Insert({10, track_id, s_path}).id;
  auto ms1 = mem_snap->Insert({90, track_id, s_path}).id;
  auto pms0 = proc_snap->Insert({ms0, 1}).id;
  auto pms1 = proc_snap->Insert({ms1, 1}).id;
  auto n0 = snap_node->Insert({pms0, std::nullopt, s_path, 10, 10, {}}).id;
  auto n1 = snap_node->Insert({pms1, std::nullopt, s_path, 20, 20, {}}).id;
  auto n2 = snap_node->Insert({pms1, n1, s_path, 30, 30, {}}).id;
  snap_edge->Insert({n0, n0, 0});
  snap_edge->Insert({n1, n2, 1});

  hg_cls->dataframe().Finalize();
  hg_obj->dataframe().Finalize();
  hg_ref->dataframe().Finalize();
  tracks->dataframe().Finalize();
  counters->dataframe().Finalize();
  prof_cset->dataframe().Finalize();
  prof_samples->dataframe().Finalize();
  mem_snap->dataframe().Finalize();
  proc_snap->dataframe().Finalize();
  snap_node->dataframe().Finalize();
  snap_edge->dataframe().Finalize();

  std::vector<PluginDataframe> dfs = {
      MakePluginDf(hg_cls),       MakePluginDf(hg_obj),
      MakePluginDf(hg_ref),       MakePluginDf(tracks),
      MakePluginDf(counters),     MakePluginDf(prof_cset),
      MakePluginDf(prof_samples), MakePluginDf(mem_snap),
      MakePluginDf(proc_snap),    MakePluginDf(snap_node),
      MakePluginDf(snap_edge),
  };

  TraceProcessor::ExportOptions opts;
  opts.start_ts = 50;
  opts.end_ts = 100;
  auto sliced_or = TraceSlicer::Slice(dfs, opts);
  ASSERT_TRUE(sliced_or.ok()) << sliced_or.status().c_message();

  // HeapGraphObject: 2 objects kept (old obj1 -> 0, old obj2 -> 1),
  // reference_set_id on new obj 0 compacted to 0.
  const auto& out_hg_obj = *sliced_or->dataframes[1].dataframe;
  ASSERT_EQ(out_hg_obj.row_count(), 2u);
  EXPECT_EQ((out_hg_obj.GetCellUnchecked<
                tables::HeapGraphObjectTable::ColumnIndex::reference_set_id>(
                tables::HeapGraphObjectTable::kSpec, 0)),
            std::make_optional(0u));
  // HeapGraphReference: 1 row kept with reference_set_id=0, owner_id=0,
  // owned_id=1.
  const auto& out_hg_ref = *sliced_or->dataframes[2].dataframe;
  ASSERT_EQ(out_hg_ref.row_count(), 1u);
  EXPECT_EQ((out_hg_ref.GetCellUnchecked<
                tables::HeapGraphReferenceTable::ColumnIndex::reference_set_id>(
                tables::HeapGraphReferenceTable::kSpec, 0)),
            0u);
  EXPECT_EQ((out_hg_ref.GetCellUnchecked<
                tables::HeapGraphReferenceTable::ColumnIndex::owner_id>(
                tables::HeapGraphReferenceTable::kSpec, 0)),
            0u);
  EXPECT_EQ((out_hg_ref.GetCellUnchecked<
                tables::HeapGraphReferenceTable::ColumnIndex::owned_id>(
                tables::HeapGraphReferenceTable::kSpec, 0)),
            std::make_optional(1u));

  // ProfilerSample: 1 sample kept at ts=85 with counter_set_id=0;
  // ProfilerCounterSet: 1 row with counter_set_id=0, counter_id=1 (since c0
  // was carried forward at start_ts=50 as row 0, and c1 at ts=85 became row 1).
  const auto& out_samples = *sliced_or->dataframes[6].dataframe;
  ASSERT_EQ(out_samples.row_count(), 1u);
  EXPECT_EQ((out_samples.GetCellUnchecked<
                tables::ProfilerSampleTable::ColumnIndex::counter_set_id>(
                tables::ProfilerSampleTable::kSpec, 0)),
            std::make_optional(0u));
  const auto& out_cset = *sliced_or->dataframes[5].dataframe;
  ASSERT_EQ(out_cset.row_count(), 1u);
  EXPECT_EQ((out_cset.GetCellUnchecked<
                tables::ProfilerCounterSetTable::ColumnIndex::counter_id>(
                tables::ProfilerCounterSetTable::kSpec, 0)),
            1u);

  // MemorySnapshot 3-hop FK cascade: 1 snapshot, 1 process snapshot, 2 nodes
  // (0 and 1 with process_snapshot_id=0), 1 edge (0 -> 1).
  ASSERT_EQ(sliced_or->dataframes[7].dataframe->row_count(), 1u);
  ASSERT_EQ(sliced_or->dataframes[8].dataframe->row_count(), 1u);
  const auto& out_nodes = *sliced_or->dataframes[9].dataframe;
  ASSERT_EQ(out_nodes.row_count(), 2u);
  EXPECT_EQ(
      (out_nodes.GetCellUnchecked<
          tables::MemorySnapshotNodeTable::ColumnIndex::process_snapshot_id>(
          tables::MemorySnapshotNodeTable::kSpec, 1)),
      0u);
  const auto& out_edges = *sliced_or->dataframes[10].dataframe;
  ASSERT_EQ(out_edges.row_count(), 1u);
  EXPECT_EQ((out_edges.GetCellUnchecked<
                tables::MemorySnapshotEdgeTable::ColumnIndex::source_node_id>(
                tables::MemorySnapshotEdgeTable::kSpec, 0)),
            0u);
  EXPECT_EQ((out_edges.GetCellUnchecked<
                tables::MemorySnapshotEdgeTable::ColumnIndex::target_node_id>(
                tables::MemorySnapshotEdgeTable::kSpec, 0)),
            1u);
}

}  // namespace
}  // namespace perfetto::trace_processor::trace_export
