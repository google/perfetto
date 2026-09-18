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
"""Unit tests for diff_heap_profiles.py."""

import importlib.util
import os
import unittest

# `diff_heap_profiles.py` is a standalone CLI shipped next to this test rather
# than an installed package, so load it by file path. This resolves identically
# in a plain checkout, in a bundled skill directory and in build-system runfiles.
_SCRIPT = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "diff_heap_profiles.py")
_SPEC = importlib.util.spec_from_file_location("diff_heap_profiles", _SCRIPT)
diff_heap_profiles = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(diff_heap_profiles)


class DiffHeapProfilesTest(unittest.TestCase):

  def _make_callstack_stats(
      self,
      process_name: str = "com.example.app",
      upid: int = 1,
      heap_name: str = "com.android.art",
      path: str = "main -> rasterizeDynamicVectorLayers -> AllocWithNewTLAB",
      leaf_function: str = "AllocWithNewTLAB",
      mapping_name: str = "libart.so",
      self_size: int = 4096,
      self_alloc_size: int = 65536,
      self_count: int = 1,
      self_alloc_count: int = 16,
      cumulative_size: int = 4096,
      cumulative_alloc_size: int = 65536,
  ) -> diff_heap_profiles.CallstackStats:
    return diff_heap_profiles.CallstackStats(
        process_name=process_name,
        upid=upid,
        heap_name=heap_name,
        path=path,
        leaf_function=leaf_function,
        mapping_name=mapping_name,
        source_file="",
        line_number=0,
        self_size=self_size,
        self_alloc_size=self_alloc_size,
        self_count=self_count,
        self_alloc_count=self_alloc_count,
        cumulative_size=cumulative_size,
        cumulative_alloc_size=cumulative_alloc_size,
    )

  def test_select_process_and_heap_filters_by_process_and_heap_name(self):
    rows = [
        self._make_callstack_stats(
            heap_name="com.android.art", self_alloc_size=1000),
        self._make_callstack_stats(
            heap_name="libc.malloc", self_alloc_size=5000),
    ]

    proc, heap, sel_ts, scoped = diff_heap_profiles.select_process_and_heap(
        rows,
        process_name="com.example.app",
        heap_name="libc.malloc",
    )

    self.assertEqual(proc, "com.example.app")
    self.assertEqual(heap, "libc.malloc")
    self.assertIsNone(sel_ts)
    self.assertEqual(len(scoped), 1)
    self.assertEqual(scoped[0].self_alloc_size, 5000)

  def test_select_process_and_heap_raises_on_unknown_heap(self):
    rows = [self._make_callstack_stats(heap_name="com.android.art")]

    with self.assertRaises(ValueError):
      diff_heap_profiles.select_process_and_heap(rows, heap_name="unknown_heap")

  def test_compute_callstack_diffs_calculates_exact_churn_and_unreleased_deltas(
      self,):
    before = [
        self._make_callstack_stats(
            path="main -> rasterizeDynamicVectorLayers",
            self_size=1024,
            self_alloc_size=4096,
            self_count=1,
            self_alloc_count=4,
        )
    ]
    after = [
        self._make_callstack_stats(
            path="main -> rasterizeDynamicVectorLayers",
            self_size=32768,
            self_alloc_size=65536,
            self_count=8,
            self_alloc_count=16,
        )
    ]

    diffs = diff_heap_profiles.compute_callstack_diffs(before, after)

    self.assertEqual(len(diffs), 1)
    self.assertEqual(diffs[0].delta_self_size, 31744)
    self.assertEqual(diffs[0].delta_self_alloc_size, 61440)
    self.assertEqual(diffs[0].delta_self_count, 7)
    self.assertEqual(diffs[0].delta_self_alloc_count, 12)

  def test_compute_function_summary_diffs_calculates_cumulative_deltas(self):
    before = [
        diff_heap_profiles.FunctionSummaryStats(
            process_name="com.example.app",
            upid=1,
            heap_name="com.android.art",
            function_name="com.example.app.ImageRenderer.decodeLayers",
            mapping_name="",
            self_size=0,
            cumulative_size=100000,
            self_alloc_size=0,
            cumulative_alloc_size=200000,
        )
    ]
    after = [
        diff_heap_profiles.FunctionSummaryStats(
            process_name="com.example.app",
            upid=1,
            heap_name="com.android.art",
            function_name="com.example.app.ImageRenderer.decodeLayers",
            mapping_name="",
            self_size=0,
            cumulative_size=63898816,
            self_alloc_size=0,
            cumulative_alloc_size=63898816,
        )
    ]

    diffs = diff_heap_profiles.compute_function_summary_diffs(before, after)

    self.assertEqual(len(diffs), 1)
    self.assertEqual(diffs[0].delta_cumulative_alloc_size, 63698816)
    self.assertEqual(diffs[0].delta_cumulative_size, 63798816)

  def test_format_markdown_report_includes_churn_unreleased_and_function_sections(
      self,):
    callstack_diffs = [
        diff_heap_profiles.CallstackDiffRow(
            path="main -> rasterizeDynamicVectorLayers -> AllocWithNewTLAB",
            heap_name="com.android.art",
            leaf_function="AllocWithNewTLAB",
            mapping_name="libart.so",
            before_self_size=0,
            after_self_size=45079184,
            before_self_alloc_size=0,
            after_self_alloc_size=45079184,
            before_self_count=0,
            after_self_count=10576,
            before_self_alloc_count=0,
            after_self_alloc_count=10576,
        )
    ]
    function_diffs = [
        diff_heap_profiles.FunctionSummaryDiffRow(
            function_name="com.example.app.ImageRenderer.decodeLayers",
            mapping_name="",
            before_self_size=0,
            after_self_size=0,
            before_cumulative_size=0,
            after_cumulative_size=63898816,
            before_self_alloc_size=0,
            after_self_alloc_size=0,
            before_cumulative_alloc_size=0,
            after_cumulative_alloc_size=63898816,
        )
    ]

    report_art = diff_heap_profiles.format_markdown_report(
        process_name="com.example.app",
        heap_name="com.android.art",
        callstack_diffs=callstack_diffs,
        function_diffs=function_diffs,
        top_n=5,
        sort_by="alloc_size",
    )

    self.assertIn(
        "Top 5 Regressed Callstacks (Sorted by `alloc_size` / Allocation Churn"
        " Delta)",
        report_art,
    )
    self.assertNotIn(
        "Top 5 Regressed Callstacks by Unreleased Size Delta (`self_size`)",
        report_art,
    )
    self.assertIn(
        "Top 5 Regressed Functions / Methods (Cumulative Allocation Churn"
        " Delta)",
        report_art,
    )
    self.assertIn("rasterizeDynamicVectorLayers", report_art)

    report_native = diff_heap_profiles.format_markdown_report(
        process_name="com.example.app",
        heap_name="libc.malloc",
        callstack_diffs=callstack_diffs,
        function_diffs=function_diffs,
        top_n=5,
        sort_by="alloc_size",
    )
    self.assertIn(
        "Top 5 Regressed Callstacks by Unreleased Size Delta (`self_size`)",
        report_native,
    )

  def test_load_sql_files_succeeds(self):
    callstacks_sql = diff_heap_profiles._load_sql_file(
        "query_heap_profile_callstacks.sql")
    summary_sql = diff_heap_profiles._load_sql_file(
        "query_heap_profile_summary_tree.sql")
    self.assertIn("_callstack_spc_forest", callstacks_sql)
    self.assertIn("android_heap_profile_summary_tree", summary_sql)

  def test_select_process_and_heap_supports_dump_index_and_explicit_ts(self):
    row_early = diff_heap_profiles.CallstackStats(
        process_name="com.example.app",
        upid=1,
        heap_name="com.android.art",
        path="main -> decodeBitmap",
        leaf_function="decodeBitmap",
        mapping_name="app.apk",
        source_file="",
        line_number=0,
        self_size=1024,
        self_alloc_size=1024,
        self_count=1,
        self_alloc_count=1,
        cumulative_size=1024,
        cumulative_alloc_size=1024,
        dump_ts=1000,
    )
    row_late = diff_heap_profiles.CallstackStats(
        process_name="com.example.app",
        upid=1,
        heap_name="com.android.art",
        path="main -> decodeBitmap",
        leaf_function="decodeBitmap",
        mapping_name="app.apk",
        source_file="",
        line_number=0,
        self_size=8192,
        self_alloc_size=8192,
        self_count=8,
        self_alloc_count=8,
        cumulative_size=8192,
        cumulative_alloc_size=8192,
        dump_ts=2000,
    )
    rows = [row_early, row_late]

    available = diff_heap_profiles.list_profile_dump_timestamps(
        rows, "com.example.app")
    self.assertEqual(available, [1000, 2000])

    _, _, first_ts, first_rows = diff_heap_profiles.select_process_and_heap(
        rows, process_name="com.example.app", dump_index=0)
    _, _, last_ts, last_rows = diff_heap_profiles.select_process_and_heap(
        rows, process_name="com.example.app", dump_index=-1)
    self.assertEqual(first_ts, 1000)
    self.assertEqual(first_rows[0].self_alloc_size, 1024)
    self.assertEqual(last_ts, 2000)
    self.assertEqual(last_rows[0].self_alloc_size, 8192)


if __name__ == "__main__":
  unittest.main()
