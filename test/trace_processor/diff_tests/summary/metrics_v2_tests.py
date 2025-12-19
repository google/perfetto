#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import DataPath, Path, Csv
from python.generators.diff_tests.testing import MetricV2SpecTextproto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class SummaryMetricsV2(TestSuite):

  def test_smoke_metric_v2(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=MetricV2SpecTextproto('''
          id: "memory_per_process"
          dimensions: "process_name"
          value: "avg_rss_and_swap"
          query: {
            table: {
              table_name: "memory_rss_and_swap_per_process"
              module_name: "linux.memory.process"
            }
            group_by: {
              column_names: "process_name"
              aggregates: {
                column_name: "rss_and_swap"
                op: DURATION_WEIGHTED_MEAN
                result_column_name: "avg_rss_and_swap"
              }
            }
          }
        '''),
        out=Path('smoke_metric_v2.out'))

  def test_simple_slices_metric_v2(self):
    return DiffTestBlueprint(
        trace=Path('synth_simple_slices.py'),
        # Test reading dimensions in correct order
        # Dimensions are intentionally defined in different order from the query
        # Metric is defined to not be the last column in the query
        query=MetricV2SpecTextproto('''
              id: "max_duration"
              dimensions_specs {
                name: "thread_name"
                type: STRING
              }
              dimensions_specs {
                name: "slice_name"
                type: STRING
              }
              value: "max_dur"
              query {
                simple_slices {
                  slice_name_glob: "*"
                }
                group_by {
                  column_names: "slice_name"
                  column_names: "thread_name"
                  aggregates {
                    column_name: "dur"
                    op: COUNT
                    result_column_name: "count_dur"
                  }
                  aggregates {
                    column_name: "dur"
                    op: MAX
                    result_column_name: "max_dur"
                  }
                  aggregates {
                    column_name: "dur"
                    op: SUM
                    result_column_name: "sum_dur"
                  }
                }
              }
        '''),
        out=Path('simple_slices_metric_v2.out'))

  def test_sql_no_preamble(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=MetricV2SpecTextproto('''
          id: "memory_per_process"
          dimensions: "id"
          value: "ts"
          query: {
            id: "sql_source"
            sql {
              sql: "SELECT id, ts FROM slice limit 2"
              column_names: "id"
              column_names: "ts"
            }
          }
        '''),
        out=Csv("""
          bundle_id: "memory_per_process"
          row {
            values: {
              double_value: 37351104642.0
            }
            dimension {
              int64_value: 0
            }
          }
          row {
            values: {
              double_value: 37351520078.0
            }
            dimension {
            int64_value: 1
            }
          }
          specs {
            id: "memory_per_process"
            dimensions: "id"
            value: "ts"
            query {
              id: "sql_source"
              sql {
                sql: "SELECT id, ts FROM slice limit 2"
                column_names: "id"
                column_names: "ts"
              }
            }
          }
        """))

  def test_sql_miltistatements_in_sql(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=MetricV2SpecTextproto('''
          id: "memory_per_process"
          dimensions: "id"
          value: "ts"
          query: {
            id: "sql_source"
            sql {
              sql: "INCLUDE PERFETTO MODULE slices.with_context; SELECT id, ts FROM slice limit 2"
              column_names: "id"
              column_names: "ts"
            }
          }
        '''),
        out=Csv("""
          bundle_id: "memory_per_process"
          row {
            values: {
              double_value: 37351104642.0
            }
            dimension {
              int64_value: 0
            }
          }
          row {
            values: {
              double_value: 37351520078.0
            }
            dimension {
              int64_value: 1
            }
          }
          specs {
            id: "memory_per_process"
            dimensions: "id"
            value: "ts"
            query {
              id: "sql_source"
              sql {
                sql: "INCLUDE PERFETTO MODULE slices.with_context; SELECT id, ts FROM slice limit 2"
                column_names: "id"
                column_names: "ts"
              }
            }
          }
        """))

  def test_sql_with_view_preamble(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=MetricV2SpecTextproto('''
          id: "preamble_view_metric"
          dimensions: "id"
          value: "ts"
          query: {
            id: "sql_source_with_view"
            sql {
              preamble: "CREATE PERFETTO VIEW sl AS SELECT id, ts FROM slice;"
              sql: "SELECT id, ts FROM sl LIMIT 2"
              column_names: "id"
              column_names: "ts"
            }
          }
        '''),
        out=Csv("""
          bundle_id: "preamble_view_metric"
          row {
            values: {
              double_value: 37351104642.0
            }
            dimension {
              int64_value: 0
            }
          }
          row {
            values: {
              double_value: 37351520078.0
            }
            dimension {
            int64_value: 1
            }
          }
          specs {
            id: "preamble_view_metric"
            dimensions: "id"
            value: "ts"
            query {
              id: "sql_source_with_view"
              sql {
                preamble: "CREATE PERFETTO VIEW sl AS SELECT id, ts FROM slice;"
                sql: "SELECT id, ts FROM sl LIMIT 2"
                column_names: "id"
                column_names: "ts"
              }
            }
          }
        """))

  def test_sql_with_view_multistatement_view(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=MetricV2SpecTextproto('''
          id: "preamble_view_metric"
          dimensions: "id"
          value: "ts"
          query: {
            id: "sql_source_with_view"
            sql {
              sql: "CREATE PERFETTO VIEW sl AS SELECT id, ts FROM slice; SELECT id, ts FROM sl LIMIT 2"
              column_names: "id"
              column_names: "ts"
            }
          }
        '''),
        out=Csv("""
          bundle_id: "preamble_view_metric"   
          row {
            values: {
              double_value: 37351104642.0
            }
            dimension {
              int64_value: 0
            }
          }
          row {
            values: {
              double_value: 37351520078.0
            }
            dimension {
            int64_value: 1
            }
          }
          specs {
            id: "preamble_view_metric"
            dimensions: "id"
            value: "ts"
            query {
              id: "sql_source_with_view"
              sql {
              sql: "CREATE PERFETTO VIEW sl AS SELECT id, ts FROM slice; SELECT id, ts FROM sl LIMIT 2"
                column_names: "id"
                column_names: "ts"
              }
            }
          }
        """))

  def test_sql_no_columns_specified(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=MetricV2SpecTextproto('''
          id: "memory_per_process"
          dimensions: "id"
          value: "ts"
          query: {
            id: "sql_source"
            sql {
              sql: "SELECT id, ts FROM slice limit 2"
            }
          }
        '''),
        out=Csv("""
          bundle_id: "memory_per_process"
          row {
            values {
              double_value: 37351104642.0
            }
            dimension {
              int64_value: 0
            }
          }
          row {
            values {
              double_value: 37351520078.0
            }
            dimension {
              int64_value: 1
            }
          }
          specs {
            id: "memory_per_process"
            dimensions: "id"
            value: "ts"
            query {
              id: "sql_source"
              sql {
                sql: "SELECT id, ts FROM slice limit 2"
              }
            }
          }
"""))

  def test_column_transformation(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=MetricV2SpecTextproto('''
          id: "max_duration_ms"
          dimensions: "slice_name"
          dimensions: "thread_name"
          value: "max_dur_ms"
          query: {
            inner_query: {
              sql: {
                sql: "SELECT s.name, s.dur, t.name as thread_name FROM slice s JOIN thread_track tt ON s.track_id = tt.id JOIN thread t ON tt.utid = t.utid WHERE s.name = 'binder transaction'"
              }
              select_columns {
                column_name_or_expression: "name"
                alias: "slice_name"
              }
              select_columns {
                column_name_or_expression: "thread_name"
              }
              select_columns {
                column_name_or_expression: "dur / 1000"
                alias: "dur_ms"
              }
            }
            group_by: {
              column_names: "slice_name"
              column_names: "thread_name"
              aggregates: {
                column_name: "dur_ms"
                op: MAX
                result_column_name: "max_dur_ms"
              }
            }
          }
        '''),
        out=Path('column_transformation.out'))

  def test_percentile_metric_v2(self):
    return DiffTestBlueprint(
        trace=Path('synth_simple_slices.py'),
        query=MetricV2SpecTextproto('''
              id: "p99_duration"
              dimensions_specs {
                name: "slice_name"
                type: STRING
              }
              value: "p99_dur"
              query {
                simple_slices {
                  slice_name_glob: "*"
                }
                group_by {
                  column_names: "slice_name"
                  aggregates {
                    column_name: "dur"
                    op: PERCENTILE
                    result_column_name: "p99_dur"
                    percentile: 99
                  }
                }
              }
        '''),
        out=Csv("""
          bundle_id: "p99_duration"
          row {
            values {
              double_value: 99.1
            }
            dimension {
              string_value: "ProcessSliceNoThread"
            }
          }
          row {
            values {
              double_value: 298.1
            }
            dimension {
              string_value: "ThreadSlice1"
            }
          }
          row {
            values {
              double_value: 197.1
            }
            dimension {
              string_value: "ThreadSlice2"
            }
          }
          specs {
            id: "p99_duration"
            dimensions_specs {
              name: "slice_name"
              type: STRING
            }
            value: "p99_dur"
            query {
              simple_slices {
                slice_name_glob: "*"
              }
              group_by {
                column_names: "slice_name"
                aggregates {
                  column_name: "dur"
                  op: PERCENTILE
                  result_column_name: "p99_dur"
                  percentile: 99
                }
              }
            }
          }
        """))