#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Unit tests for diff_heap_graphs.py."""

import importlib.util
import os
import unittest

# `diff_heap_graphs.py` is a standalone CLI shipped next to this test rather than
# an installed package, so load it by file path. This resolves identically in a
# plain checkout, in a bundled skill directory and in build-system runfiles.
_SCRIPT = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "diff_heap_graphs.py")
_SPEC = importlib.util.spec_from_file_location("diff_heap_graphs", _SCRIPT)
diff_heap_graphs = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(diff_heap_graphs)


class DiffHeapGraphsTest(unittest.TestCase):

  def _make_class_stats(
      self,
      process_name: str = "com.android.systemui",
      upid: int = 1,
      graph_sample_ts: int = 1000,
      class_name: str = "com.example.Foo",
      reachable_obj_count: int = 10,
      reachable_size_bytes: int = 1000,
      reachable_native_size_bytes: int = 0,
      dominated_size_bytes: int = 2000,
      dominated_native_size_bytes: int = 0,
  ) -> diff_heap_graphs.ClassStats:
    return diff_heap_graphs.ClassStats(
        process_name=process_name,
        upid=upid,
        graph_sample_ts=graph_sample_ts,
        class_name=class_name,
        is_libcore_or_array=False,
        obj_count=reachable_obj_count,
        size_bytes=reachable_size_bytes,
        native_size_bytes=reachable_native_size_bytes,
        reachable_obj_count=reachable_obj_count,
        reachable_size_bytes=reachable_size_bytes,
        reachable_native_size_bytes=reachable_native_size_bytes,
        dominated_obj_count=reachable_obj_count,
        dominated_size_bytes=dominated_size_bytes,
        dominated_native_size_bytes=dominated_native_size_bytes,
    )

  def test_select_process_and_latest_ts_picks_latest_timestamp_and_upid(self):
    rows = [
        self._make_class_stats(
            upid=1, graph_sample_ts=100, reachable_obj_count=5),
        self._make_class_stats(
            upid=1, graph_sample_ts=200, reachable_obj_count=15),
    ]

    proc, upid, ts, scoped = diff_heap_graphs.select_process_and_latest_ts(
        rows, process_name="com.android.systemui")

    self.assertEqual(proc, "com.android.systemui")
    self.assertEqual(upid, 1)
    self.assertEqual(ts, 200)
    self.assertEqual(len(scoped), 1)
    self.assertEqual(scoped[0].reachable_obj_count, 15)

  def test_select_process_and_latest_ts_isolates_multiple_upids_for_same_process(
      self,):
    rows = [
        self._make_class_stats(
            upid=10, graph_sample_ts=100, reachable_obj_count=50),
        self._make_class_stats(
            upid=20, graph_sample_ts=200, reachable_obj_count=75),
        self._make_class_stats(
            upid=10, graph_sample_ts=200, reachable_obj_count=99),
    ]

    proc, upid, ts, scoped = diff_heap_graphs.select_process_and_latest_ts(
        rows, process_name="com.android.systemui")

    self.assertEqual(proc, "com.android.systemui")
    self.assertEqual(ts, 200)
    self.assertEqual(upid, 20)
    self.assertEqual(len(scoped), 1)
    self.assertEqual(scoped[0].reachable_obj_count, 75)

  def test_select_process_and_latest_ts_raises_on_unknown_process(self):
    rows = [self._make_class_stats(process_name="com.android.systemui")]

    with self.assertRaises(ValueError):
      diff_heap_graphs.select_process_and_latest_ts(
          rows, process_name="com.unknown.app")

  def test_compute_class_diffs_calculates_exact_deltas(self):
    before = [
        self._make_class_stats(
            class_name="android.database.ContentObserver$Transport",
            reachable_obj_count=232,
            reachable_size_bytes=8352,
            dominated_size_bytes=8352,
        )
    ]
    after = [
        self._make_class_stats(
            class_name="android.database.ContentObserver$Transport",
            reachable_obj_count=287,
            reachable_size_bytes=10332,
            dominated_size_bytes=37772,
        )
    ]

    diffs = diff_heap_graphs.compute_class_diffs(before, after)

    self.assertEqual(len(diffs), 1)
    self.assertEqual(diffs[0].delta_reachable_count, 55)
    self.assertEqual(diffs[0].delta_reachable_size_bytes, 1980)
    self.assertEqual(diffs[0].delta_total_dominated_bytes, 29420)

  def test_compute_class_diffs_aggregates_duplicate_class_rows_across_classloaders(
      self,):
    before = [
        self._make_class_stats(
            class_name="com.example.PluginView",
            reachable_obj_count=3,
            reachable_size_bytes=300,
            dominated_size_bytes=600,
        ),
        self._make_class_stats(
            class_name="com.example.PluginView",
            reachable_obj_count=7,
            reachable_size_bytes=700,
            dominated_size_bytes=1400,
        ),
    ]
    after = [
        self._make_class_stats(
            class_name="com.example.PluginView",
            reachable_obj_count=15,
            reachable_size_bytes=1500,
            dominated_size_bytes=3000,
        )
    ]

    diffs = diff_heap_graphs.compute_class_diffs(before, after)

    self.assertEqual(len(diffs), 1)
    self.assertEqual(diffs[0].before_reachable_count, 10)
    self.assertEqual(diffs[0].after_reachable_count, 15)
    self.assertEqual(diffs[0].delta_reachable_count, 5)
    self.assertEqual(diffs[0].delta_total_dominated_bytes, 1000)

  def test_compute_dominator_path_diffs_joins_on_path_and_heap_type(self):
    before = [
        diff_heap_graphs.DominatorPathStats(
            process_name="com.android.systemui",
            upid=1,
            graph_sample_ts=100,
            path=(
                "[ROOT]"
                " com.android.systemui.globalactions.GlobalActionsDialogLite"),
            heap_type="HEAP_TYPE_APP",
            class_name=(
                "com.android.systemui.globalactions.GlobalActionsDialogLite"),
            self_count=26,
            self_size=6240,
        )
    ]
    after = [
        diff_heap_graphs.DominatorPathStats(
            process_name="com.android.systemui",
            upid=1,
            graph_sample_ts=200,
            path=(
                "[ROOT]"
                " com.android.systemui.globalactions.GlobalActionsDialogLite"),
            heap_type="HEAP_TYPE_APP",
            class_name=(
                "com.android.systemui.globalactions.GlobalActionsDialogLite"),
            self_count=54,
            self_size=12960,
        )
    ]

    diffs = diff_heap_graphs.compute_dominator_path_diffs(before, after)

    self.assertEqual(len(diffs), 1)
    self.assertEqual(diffs[0].delta_self_count, 28)
    self.assertEqual(diffs[0].delta_self_size, 6720)

  def test_format_markdown_report_includes_dominated_count_and_path_sections(
      self,):
    class_diffs = [
        diff_heap_graphs.ClassDiffRow(
            class_name="android.database.ContentObserver$Transport",
            before_reachable_count=232,
            after_reachable_count=287,
            before_reachable_size_bytes=8352,
            after_reachable_size_bytes=10332,
            before_native_size_bytes=0,
            after_native_size_bytes=0,
            before_total_dominated_bytes=8352,
            after_total_dominated_bytes=37772,
        )
    ]
    path_diffs = [
        diff_heap_graphs.DominatorPathDiffRow(
            path="[ROOT_JNI_GLOBAL] android.database.ContentObserver$Transport",
            heap_type="HEAP_TYPE_APP",
            class_name="android.database.ContentObserver$Transport",
            before_self_count=232,
            after_self_count=287,
            before_self_size=8352,
            after_self_size=10332,
        )
    ]

    report = diff_heap_graphs.format_markdown_report(
        process_name="com.example.app",
        before_ts=100,
        after_ts=200,
        class_diffs=class_diffs,
        path_diffs=path_diffs,
        top_n=5,
        sort_by="dominated_size",
    )

    self.assertIn("Top 5 Regressed Classes (Sorted by `dominated_size`)",
                  report)
    self.assertIn("Top 5 Regressed Classes by Instance Count", report)
    self.assertIn("Top 5 Regressed Dominator Class Paths", report)

  def test_load_sql_files_succeeds(self):
    class_sql = diff_heap_graphs._load_sql_file("query_class_aggregation.sql")
    paths_sql = diff_heap_graphs._load_sql_file(
        "query_dominator_class_paths.sql")
    self.assertIn("android_heap_graph_class_aggregation", class_sql)
    self.assertIn("dominator_class_tree", paths_sql)

  def test_select_process_with_dump_index_and_available_timestamps(self):
    rows = [
        self._make_class_stats(graph_sample_ts=100, reachable_obj_count=10),
        self._make_class_stats(graph_sample_ts=200, reachable_obj_count=25),
    ]
    available = diff_heap_graphs.list_process_dump_timestamps(
        rows, "com.android.systemui")
    self.assertEqual(available, [100, 200])

    _, _, first_ts, first_rows = diff_heap_graphs.select_process_and_latest_ts(
        rows, process_name="com.android.systemui", dump_index=0)
    _, _, last_ts, last_rows = diff_heap_graphs.select_process_and_latest_ts(
        rows, process_name="com.android.systemui", dump_index=-1)
    self.assertEqual(first_ts, 100)
    self.assertEqual(first_rows[0].reachable_obj_count, 10)
    self.assertEqual(last_ts, 200)
    self.assertEqual(last_rows[0].reachable_obj_count, 25)

  def test_compute_field_value_and_instance_config_diffs(self):
    before_fields = [
        diff_heap_graphs.FieldValueStats(
            process_name="pid=0",
            upid=1,
            graph_sample_ts=100,
            heap_type="app",
            class_name="com.android.systemui.qs.panel.TileSessionState",
            field_name="tileSpec",
            field_type="java.lang.String",
            field_value="'wifi'",
            instance_count=20,
            self_size=500,
            dominated_size=1312420,
        )
    ]
    after_fields = [
        diff_heap_graphs.FieldValueStats(
            process_name="pid=0",
            upid=1,
            graph_sample_ts=200,
            heap_type="app",
            class_name="com.android.systemui.qs.panel.TileSessionState",
            field_name="tileSpec",
            field_type="java.lang.String",
            field_value="'bluetooth_audio'",
            instance_count=50,
            self_size=1250,
            dominated_size=3281050,
        )
    ]
    field_diffs = diff_heap_graphs.compute_field_value_diffs(
        before_fields, after_fields)
    self.assertEqual(len(field_diffs), 2)

    after_configs = [
        diff_heap_graphs.InstanceConfigStats(
            process_name="pid=0",
            upid=1,
            graph_sample_ts=200,
            heap_type="app",
            class_name="com.android.systemui.qs.panel.TileSessionState",
            config_tuple="autoRefreshEnabled=true, tileSpec='bluetooth_audio'",
            instance_count=50,
            self_size=1250,
            dominated_size=3281050,
        )
    ]
    config_diffs = diff_heap_graphs.compute_instance_config_diffs([],
                                                                  after_configs)
    self.assertEqual(len(config_diffs), 1)
    self.assertEqual(config_diffs[0].delta_instance_count, 50)

    report = diff_heap_graphs.format_markdown_report(
        process_name="pid=0",
        before_ts=100,
        after_ts=200,
        class_diffs=[],
        path_diffs=[],
        top_n=5,
        sort_by="dominated_size",
        field_value_diffs=field_diffs,
        instance_config_diffs=config_diffs,
    )
    self.assertIn("Top 5 Regressed Field Values", report)
    self.assertIn("Top 5 Regressed Instance Configurations", report)
    self.assertIn("bluetooth_audio", report)


if __name__ == "__main__":
  unittest.main()
