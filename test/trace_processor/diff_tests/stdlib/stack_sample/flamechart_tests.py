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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import ExpectedError
from python.generators.diff_tests.testing import TestSuite


class FlamechartRuns(TestSuite):

  def test_shared_prefix_merges(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.stack_sample.flamechart;

          CREATE PERFETTO TABLE stacks AS
          SELECT 100 AS id, NULL AS parent_id, 'A' AS name
          UNION ALL SELECT 101, 100, 'B'
          UNION ALL SELECT 102, 101, 'C'
          UNION ALL SELECT 103, 101, 'D';

          CREATE PERFETTO TABLE points AS
          SELECT 10 AS ts, 102 AS leaf_id
          UNION ALL SELECT 20, 102
          UNION ALL SELECT 30, 103
          UNION ALL SELECT 40, 103;

          SELECT ts, dur, depth, id, sample_count
          FROM _stack_sample_flamechart_runs!(
            _tree_from_table!((SELECT * FROM stacks), (name)),
            (SELECT ts, leaf_id FROM points ORDER BY ts)
          )
          ORDER BY depth, ts;
        """,
        out=Csv("""
        "ts","dur","depth","id","sample_count"
        10,-1,0,100,4
        10,-1,1,101,4
        10,20,2,102,2
        30,-1,2,103,2
        """))

  def test_null_and_unresolvable_leaves_skipped(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.stack_sample.flamechart;

          CREATE PERFETTO TABLE stacks AS
          SELECT 100 AS id, NULL AS parent_id, 'A' AS name
          UNION ALL SELECT 101, 100, 'B';

          CREATE PERFETTO TABLE points AS
          SELECT 10 AS ts, 101 AS leaf_id
          UNION ALL SELECT 15, NULL
          UNION ALL SELECT 18, 999
          UNION ALL SELECT 20, 101;

          SELECT ts, dur, depth, id, sample_count
          FROM _stack_sample_flamechart_runs!(
            _tree_from_table!((SELECT * FROM stacks), (name)),
            (SELECT ts, leaf_id FROM points ORDER BY ts)
          )
          ORDER BY depth, ts;
        """,
        out=Csv("""
        "ts","dur","depth","id","sample_count"
        10,-1,0,100,2
        10,-1,1,101,2
        """))

  def test_empty_points(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.stack_sample.flamechart;

          CREATE PERFETTO TABLE stacks AS
          SELECT 100 AS id, NULL AS parent_id, 'A' AS name;

          SELECT count(*) AS cnt
          FROM _stack_sample_flamechart_runs!(
            _tree_from_table!((SELECT * FROM stacks), (name)),
            (SELECT 0 AS ts, 0 AS leaf_id WHERE FALSE)
          );
        """,
        out=Csv("""
        "cnt"
        0
        """))

  def test_supported_sampling_timebases(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.stack_sample.flamechart;
          WITH units(unit) AS (
            VALUES ('ns'), ('cycles'), ('instructions'), ('count'),
                   ('cache-misses'), ('custom-clock'), (''), (NULL)
          )
          SELECT unit, _stack_sample_flamechart_supported(unit) AS supported
          FROM units;
        """,
        out=Csv('''
        "unit","supported"
        "ns",1
        "cycles",1
        "instructions",1
        "count",0
        "cache-misses",0
        "custom-clock",0
        "",0
        "[NULL]",0
        '''))

  def test_mapping_categories(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.stack_sample.mapping;
          WITH mappings(name) AS (
            VALUES ('/out/trace_processor_shell'), ('trace_processor_shell'),
                   ('/out/trace_processor_shell (deleted)'),
                   ('/lib/libc.so.6'), ('/lib/libc.so (deleted)'),
                   ('/lib/libSystem.dylib'), ('C:' || char(92) || 'System32' || char(92) || 'KERNEL32.DLL'),
                   ('/app/base.apk'), ('/dir.so.name/trace_processor_shell'),
                   ('[kernel.kallsyms]'), ('/kernel'), ('/boot/vmlinux'),
                   ('/lib/modules/driver.ko.xz'), ('[vdso]'), ('[anon:jit]'),
                   ('/memfd:jit-cache (deleted)'), ('unknown'), (''), (NULL)
          )
          SELECT name, _stack_sample_mapping_category(name) AS category
          FROM mappings;
        """,
        out=Csv(r'''
        "name","category"
        "/out/trace_processor_shell",0
        "trace_processor_shell",0
        "/out/trace_processor_shell (deleted)",0
        "/lib/libc.so.6",1
        "/lib/libc.so (deleted)",1
        "/lib/libSystem.dylib",1
        "C:\System32\KERNEL32.DLL",1
        "/app/base.apk",1
        "/dir.so.name/trace_processor_shell",0
        "[kernel.kallsyms]",2
        "/kernel",2
        "/boot/vmlinux",2
        "/lib/modules/driver.ko.xz",2
        "[vdso]",3
        "[anon:jit]",3
        "/memfd:jit-cache (deleted)",3
        "unknown",3
        "",3
        "[NULL]",3
        '''))

  def test_invalid_tree_reports_error_without_crashing(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query='SELECT __intrinsic_flamechart(NULL, 10, 0);',
        out=ExpectedError('flamechart: first argument must be a TREE pointer'))

  def test_invalid_sample_type_reports_error(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          SELECT __intrinsic_flamechart(
            _tree_from_table!((SELECT 0 AS id, NULL AS parent_id, 'A' AS name), (name)),
            'invalid', 0
          );
        """,
        out=ExpectedError('flamechart: ts and leaf_id must be integers'))
