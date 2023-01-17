#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Path, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Tables(DiffTestModule):

  def test_android_sched_and_ps_smoke_window(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query="""
SELECT * FROM "window";
""",
        out=Csv("""
"ts","dur","quantum_ts"
0,9223372036854775807,0
"""))

  def test_synth_1_filter_sched(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query="""
SELECT ts, cpu, dur FROM sched
WHERE
  cpu = 1
  AND dur > 50
  AND dur <= 100
  AND ts >= 100
  AND ts <= 400;
""",
        out=Csv("""
"ts","cpu","dur"
170,1,80
"""))

  def test_android_sched_and_ps_b119496959(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query="""
SELECT ts, cpu FROM sched WHERE ts >= 81473797418963 LIMIT 10;
""",
        out=Csv("""
"ts","cpu"
81473797824982,3
81473797942847,3
81473798135399,0
81473798786857,2
81473798875451,3
81473799019930,2
81473799079982,0
81473800089357,3
81473800144461,3
81473800441805,3
"""))

  def test_android_sched_and_ps_b119301023(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query="""
SELECT ts FROM sched
WHERE ts > 0.1 + 1e9
LIMIT 10;
""",
        out=Csv("""
"ts"
81473010031230
81473010109251
81473010121751
81473010179772
81473010203886
81473010234720
81473010278522
81473010308470
81473010341386
81473010352792
"""))

  def test_synth_1_filter_counter(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query="""
SELECT COUNT(*)
FROM counter
WHERE
  track_id = 0;
""",
        out=Csv("""
"COUNT(*)"
2
"""))

  def test_memory_counters_b120278869_neg_ts_end(self):
    return DiffTestBlueprint(
        trace=Path('../../data/memory_counters.pb'),
        query="""
SELECT count(*) FROM counters WHERE -1 < ts;
""",
        out=Csv("""
"count(*)"
98688
"""))

  def test_counters_where_cpu_counters_where_cpu(self):
    return DiffTestBlueprint(
        trace=Path('counters_where_cpu.py'),
        query="""
SELECT
  ts,
  lead(ts, 1, ts) OVER (PARTITION BY name ORDER BY ts) - ts AS dur,
  value
FROM counter c
JOIN cpu_counter_track t ON t.id = c.track_id
WHERE cpu = 1;
""",
        out=Csv("""
"ts","dur","value"
1000,1,3000.000000
1001,0,4000.000000
"""))

  def test_counters_group_by_freq_counters_group_by_freq(self):
    return DiffTestBlueprint(
        trace=Path('counters_group_by_freq.py'),
        query="""
SELECT
  value,
  sum(dur) AS dur_sum
FROM (
  SELECT value,
    lead(ts) OVER (PARTITION BY name, track_id ORDER BY ts) - ts AS dur
  FROM counter
  JOIN counter_track ON counter.track_id = counter_track.id
)
WHERE value > 0
GROUP BY value
ORDER BY dur_sum DESC;
""",
        out=Csv("""
"value","dur_sum"
4000.000000,2
3000.000000,1
"""))

  def test_filter_row_vector_example_android_trace_30s(self):
    return DiffTestBlueprint(
        trace=Path('../../data/example_android_trace_30s.pb'),
        query="""
SELECT ts
FROM counter
WHERE
  ts > 72563651549
  AND track_id = (
    SELECT t.id
    FROM process_counter_track t
    JOIN process p USING (upid)
    WHERE
      t.name = 'Heap size (KB)'
      AND p.pid = 1204
  )
  AND value != 17952.000000
LIMIT 20;
""",
        out=Path('filter_row_vector_example_android_trace_30s.out'))

  def test_counter_dur_example_android_trace_30s(self):
    return DiffTestBlueprint(
        trace=Path('../../data/example_android_trace_30s.pb'),
        query=Path('counter_dur_test.sql'),
        out=Csv("""
"ts","dur"
100351738640,-1
100351738640,-1
100351738640,-1
70731059648,19510835
70731059648,19510835
70731059648,19510835
73727335051,23522762
73727335051,23522762
73727335051,23522762
86726132752,24487554
"""))

  def test_nulls(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query="""
CREATE TABLE null_test (
  primary_key INTEGER PRIMARY KEY,
  int_nulls INTEGER,
  string_nulls STRING,
  double_nulls DOUBLE,
  start_int_nulls INTEGER,
  start_string_nulls STRING,
  start_double_nulls DOUBLE,
  all_nulls INTEGER
);

INSERT INTO null_test(
  int_nulls,
  string_nulls,
  double_nulls,
  start_int_nulls,
  start_string_nulls,
  start_double_nulls
)
VALUES
(1, "test", 2.0, NULL, NULL, NULL),
(2, NULL, NULL, NULL, "test", NULL),
(1, "other", NULL, NULL, NULL, NULL),
(4, NULL, NULL, NULL, NULL, 1.0),
(NULL, "test", 1.0, 1, NULL, NULL);

SELECT * FROM null_test;
""",
        out=Path('nulls.out'))

  def test_thread_main_thread(self):
    return DiffTestBlueprint(
        trace=Path('thread_main_thread.textproto'),
        query="""
SELECT
  tid,
  is_main_thread
FROM thread
WHERE tid IN (5, 7, 11, 12, 99)
ORDER BY tid;
""",
        out=Csv("""
"tid","is_main_thread"
5,1
7,0
11,1
12,0
99,"[NULL]"
"""))

  def test_trace_metadata(self):
    return DiffTestBlueprint(
        trace=Path('../../data/memory_counters.pb'),
        query=Metric('trace_metadata'),
        out=Path('trace_metadata.json.out'))

  def test_android_task_names(self):
    return DiffTestBlueprint(
        trace=Path('process_uids.textproto'),
        query=Metric('android_task_names'),
        out=TextProto(r"""
android_task_names {
  process {
    pid: 1
    process_name: "init"
    uid: 0
  }
  process {
    pid: 2
    process_name: "com.google.android.gm:process"
    uid: 10001
    uid_package_name: "com.google.android.gm"
  }
}
"""))

  def test_ftrace_setup_errors(self):
    return DiffTestBlueprint(
        trace=Path('../../data/ftrace_error_stats.pftrace'),
        query="""
SELECT value FROM stats WHERE name = 'ftrace_setup_errors'
UNION ALL
SELECT str_value FROM metadata WHERE name = 'ftrace_setup_errors';
""",
        out=Csv("""
"value"
3
"Ftrace event unknown: foo/bar
Ftrace event unknown: sched/foobar
Atrace failures: error: unknown tracing category "bar"
error enabling tracing category "bar"
"
"""))
