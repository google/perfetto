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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
from python.generators.diff_tests.testing import TestSuite


class Tables(TestSuite):
  # Contains tests for the handling of tables by trace processor. The focus of
  # here is to check that trace processor is correctly returning and handling
  # on the really important tables in trace processor.  Note: It's generally
  # advisable to add tests here. Check the guidance provided by
  # for choosing which folder to add a new test to. Window table
  def test_android_sched_and_ps_smoke_window(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        query="""
        SELECT * FROM "window";
        """,
        out=Csv("""
        "ts","dur","quantum_ts"
        0,9223372036854775807,0
        """))

  def test_simple_interval_intersect_rev(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""

        CREATE PERFETTO TABLE A AS
          WITH data(id, ts, ts_end, c0, c1) AS (
            VALUES
            (0, 1, 7, 10, 3)
          )
          SELECT * FROM data;

        CREATE PERFETTO TABLE B AS
          WITH data(id, ts, ts_end, c0, c2) AS (
            VALUES
            (0, 0, 2, 10, 100),
            (1, 3, 5, 10, 200),
            (2, 6, 8, 20, 300)
          )
          SELECT * FROM data;

        SELECT a.id AS a_id, b.id AS b_id
        FROM __intrinsic_ii_with_interval_tree('A', 'c0, c1') a
        JOIN __intrinsic_ii_with_interval_tree('B', 'c0, c2') b
        WHERE a.ts < b.ts_end AND a.ts_end > b.ts
        """,
        out=Csv("""
        "a_id","b_id"
        0,1
        0,0
        0,2
        """))

  def test_compare_with_ii_macro(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE big_foo AS
        SELECT
          ts,
          ts + dur as ts_end,
          id * 10 AS id
        FROM sched
        WHERE utid == 1;

        CREATE PERFETTO TABLE small_foo AS
        SELECT
        ts + 1000 AS ts,
        ts + dur + 1000 AS ts_end,
        id
        FROM sched
        WHERE utid == 1;

        CREATE PERFETTO TABLE small_foo_for_ii AS
        SELECT id, ts, ts_end - ts AS dur
        FROM small_foo;

        CREATE PERFETTO TABLE big_foo_for_ii AS
        SELECT id, ts, ts_end - ts AS dur
        FROM big_foo;

        CREATE PERFETTO TABLE both AS
        SELECT
          left_id,
          right_id,
          cat,
          count() AS c,
          MAX(ts) AS max_ts, MAX(dur) AS max_dur
        FROM (
          SELECT a.id AS left_id, b.id AS right_id, 0 AS ts, 0 AS dur, "it" AS cat
          FROM __intrinsic_ii_with_interval_tree('big_foo', '') a
          JOIN __intrinsic_ii_with_interval_tree('small_foo', '') b
          WHERE a.ts < b.ts_end AND a.ts_end > b.ts
          UNION
          SELECT left_id, right_id, ts, dur, "ii" AS cat
          FROM _interval_intersect!(big_foo_for_ii, small_foo_for_ii)
          WHERE dur != 0
        )
          GROUP BY left_id, right_id;

        SELECT
          SUM(c) FILTER (WHERE c == 2) AS good,
          SUM(c) FILTER (WHERE c != 2) AS bad
        FROM both;
        """,
        out=Csv("""
          "good","bad"
          314,"[NULL]"
        """))

  def test_compare_with_span_join(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE big_foo AS
        SELECT
          ts,
          ts + dur as ts_end,
          id * 10 AS id,
          cpu AS c0
        FROM sched
        WHERE dur != -1;

        CREATE PERFETTO TABLE small_foo AS
        SELECT
          ts + 1000 AS ts,
          ts + dur + 1000 AS ts_end,
          id,
          cpu AS c0
        FROM sched
        WHERE dur != -1;

        CREATE PERFETTO TABLE small_foo_for_sj AS
        SELECT 
          id AS small_id, 
          ts, 
          ts_end - ts AS dur, 
          c0
        FROM small_foo
        WHERE dur != 0;

        CREATE PERFETTO TABLE big_foo_for_sj AS
        SELECT 
          id AS big_id, 
          ts, 
          ts_end - ts AS dur, 
          c0
        FROM big_foo
        WHERE dur != 0;

        CREATE VIRTUAL TABLE sj_res
        USING SPAN_JOIN(
          small_foo_for_sj PARTITIONED c0, 
          big_foo_for_sj PARTITIONED c0);
        
        CREATE PERFETTO TABLE both AS
        SELECT
          left_id,
          right_id,
          cat,
          count() AS c,
          MAX(ts) AS max_ts, MAX(dur) AS max_dur
        FROM (
          SELECT a.id AS left_id, b.id AS right_id, 0 AS ts, 0 AS dur, "it" AS cat
          FROM __intrinsic_ii_with_interval_tree('big_foo', 'c0') a
          JOIN __intrinsic_ii_with_interval_tree('small_foo', 'c0') b
          USING (c0)
          WHERE a.ts < b.ts_end AND a.ts_end > b.ts
          UNION
          SELECT big_id AS left_id, small_id AS right_id, ts, dur, "sj" AS cat FROM sj_res
        )
          GROUP BY left_id, right_id;

        SELECT
          SUM(c) FILTER (WHERE c == 2) AS good,
          SUM(c) FILTER (WHERE c != 2) AS bad
        FROM both;
        """,
        out=Csv("""
          "good","bad"
          1538288,"[NULL]"
        """))
  
  def test_ii_partitioned_big(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE big_foo AS
        SELECT
          ts,
          ts + dur as ts_end,
          id * 10 AS id,
          cpu AS c0
        FROM sched
        WHERE dur != -1;

        CREATE PERFETTO TABLE small_foo AS
        SELECT
          ts + 1000 AS ts,
          ts + dur + 1000 AS ts_end,
          id,
          cpu AS c0
        FROM sched
        WHERE dur != -1;
        
        CREATE PERFETTO TABLE res AS
        SELECT a.id AS a_id, b.id AS b_id
        FROM __intrinsic_ii_with_interval_tree('small_foo', 'c0') a
        JOIN __intrinsic_ii_with_interval_tree('big_foo', 'c0') b
        USING (c0)
        WHERE a.ts < b.ts_end AND a.ts_end > b.ts;

        SELECT * FROM res
        ORDER BY a_id, b_id
        LIMIT 10;
        """,
        out=Csv("""
        "a_id","b_id"
        0,0
        0,10
        1,10
        1,430
        2,20
        2,30
        3,30
        3,40
        4,40
        4,50
        """))


  def test_ii_operator_big(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO TABLE big_foo AS
        SELECT
          id,
          ts,
          ts+dur AS ts_end
        FROM sched
        WHERE dur != -1
        ORDER BY ts;

        CREATE PERFETTO TABLE small_foo AS
        SELECT
        id * 10 AS id,
        ts + 1000 AS ts,
        ts_end + 1000 AS ts_end
        FROM big_foo
        LIMIT 10
        OFFSET 5;

        CREATE PERFETTO TABLE res AS
        SELECT a.id AS a_id, b.id AS b_id
        FROM __intrinsic_ii_with_interval_tree('small_foo', '') a
        JOIN __intrinsic_ii_with_interval_tree('big_foo', '') b
        WHERE a.ts < b.ts_end AND a.ts_end > b.ts;

        SELECT * FROM res
        ORDER BY a_id, b_id
        LIMIT 10;
        """,
        out=Csv("""
        "a_id","b_id"
        50,1
        50,5
        50,6
        60,1
        60,6
        60,7
        60,8
        70,1
        70,6
        70,7
        """))

  def test_ii_wrong_partition(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        CREATE PERFETTO TABLE A
        AS
        WITH x(id, ts, ts_end, c0) AS (VALUES(1, 1, 2, 1), (2, 3, 4, 2))
        SELECT * FROM x;

        CREATE PERFETTO TABLE B
        AS
        WITH x(id, ts, ts_end, c0) AS (VALUES(1, 5, 6, 3))
        SELECT * FROM x;

        SELECT
        a.id AS a_id,
        b.id AS b_id
        FROM __intrinsic_ii_with_interval_tree('A', 'c0') a
        JOIN __intrinsic_ii_with_interval_tree('B', 'c0') b
        USING (c0)
        WHERE a.ts < b.ts_end AND a.ts_end > b.ts;
        """,
        out=Csv("""
        "a_id","b_id"
        """))

  # Null printing
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

  # Thread table
  def test_thread_main_thread(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1
          process_tree {
            processes {
              pid: 5
              ppid: 1
              cmdline: "com.google.pid5"
            }
            threads {
              tid: 5
              tgid: 5
            }
            threads {
              tid: 7
              tgid: 5
              name: "tid7"
            }
            processes {
              pid: 11
              ppid: 1
              cmdline: "com.google.pid11"
            }
            threads {
              tid: 11
              tgid: 11
              name: "tid11"
            }
            threads {
              tid: 12
              tgid: 11
              name: "tid12"
            }
          }
        }
        packet {
          timestamp: 2
          ftrace_events {
            cpu: 0
            event {
              timestamp: 2
              pid: 99
              lowmemory_kill {
                pid: 99
              }
            }
          }
        }
        """),
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

  # Json output
  def test_trace_metadata(self):
    return DiffTestBlueprint(
        trace=DataPath('memory_counters.pb'),
        query=Metric('trace_metadata'),
        out=Path('trace_metadata.json.out'))

  # Processes as a metric
  def test_android_task_names(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          process_tree {
            processes {
              pid: 1
              ppid: 0
              cmdline: "init"
              uid: 0
            }
            processes {
              pid: 2
              ppid: 1
              cmdline: "com.google.android.gm:process"
              uid: 10001
            }
          }
        }
        packet {
          packages_list {
            packages {
              name: "com.google.android.gm"
              uid: 10001
            }
          }
        }
        """),
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

  # Ftrace stats imports in metadata and stats tables
  def test_ftrace_setup_errors(self):
    return DiffTestBlueprint(
        trace=DataPath('ftrace_error_stats.pftrace'),
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

  # Ftrace stats imports in metadata and stats tables
  def test_filter_stats(self):
    return DiffTestBlueprint(
        trace=TextProto("""
          packet { trace_stats{ filter_stats {
            input_packets: 836
            input_bytes: 25689644
            output_bytes: 24826981
            errors: 12
            time_taken_ns: 1228178548
            bytes_discarded_per_buffer: 1
            bytes_discarded_per_buffer: 34
            bytes_discarded_per_buffer: 29
            bytes_discarded_per_buffer: 0
            bytes_discarded_per_buffer: 862588
          }}}"""),
        query="""
        SELECT name, value FROM stats
        WHERE name like 'filter_%' OR name = 'traced_buf_bytes_filtered_out'
        ORDER by name ASC
        """,
        out=Csv("""
        "name","value"
        "filter_errors",12
        "filter_input_bytes",25689644
        "filter_input_packets",836
        "filter_output_bytes",24826981
        "filter_time_taken_ns",1228178548
        "traced_buf_bytes_filtered_out",1
        "traced_buf_bytes_filtered_out",34
        "traced_buf_bytes_filtered_out",29
        "traced_buf_bytes_filtered_out",0
        "traced_buf_bytes_filtered_out",862588
        """))

  # cpu_track table
  def test_cpu_track_table(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 1
            event {
              timestamp: 100001000000
              pid: 10
              irq_handler_entry {
                irq: 100
                name : "resource1"
              }
            }
            event {
              timestamp: 100002000000
              pid: 10
              irq_handler_exit {
                irq: 100
                ret: 1
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 100003000000
              pid: 15
              irq_handler_entry {
                irq: 100
                name : "resource1"
              }
            }
          }
        }
        """),
        query="""
        SELECT
          type,
          cpu
        FROM cpu_track
        ORDER BY type, cpu;
        """,
        out=Csv("""
        "type","cpu"
        "cpu_track",0
        "cpu_track",1
        """))

  def test_thread_state_flattened_aggregated(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
      INCLUDE PERFETTO MODULE sched.thread_state_flattened;
      select * from _get_flattened_thread_state_aggregated(11155, NULL);
      """,
        out=Path('thread_state_flattened_aggregated_csv.out'))

  def test_thread_state_flattened(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
      INCLUDE PERFETTO MODULE sched.thread_state_flattened;
      select * from _get_flattened_thread_state(11155, NULL);
      """,
        out=Path('thread_state_flattened_csv.out'))

  def test_metadata(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          system_info {
            tracing_service_version: "Perfetto v38.0-0bb49ab54 (0bb49ab54dbe55ce5b9dfea3a2ada68b87aecb65)"
            timezone_off_mins: 60
            utsname {
              sysname: "Darwin"
              version: "Foobar"
              machine: "x86_64"
              release: "22.6.0"
            }
          }
          trusted_uid: 158158
          trusted_packet_sequence_id: 1
        }
        """),
        query=r"""SELECT name, COALESCE(str_value, int_value) as val
              FROM metadata
              WHERE name IN (
                  "system_name", "system_version", "system_machine",
                  "system_release", "timezone_off_mins")
              ORDER BY name
        """,
        out=Csv(r"""
                "name","val"
                "system_machine","x86_64"
                "system_name","Darwin"
                "system_release","22.6.0"
                "system_version","Foobar"
                "timezone_off_mins",60
                """))

  def test_flow_table_trace_id(self):
    return DiffTestBlueprint(
        trace=TextProto("""
          packet {
            timestamp: 0
            track_event {
              name: "Track 0 Event"
              type: TYPE_SLICE_BEGIN
              track_uuid: 10
              flow_ids: 57
            }
            trusted_packet_sequence_id: 123
          }
          packet {
            timestamp: 10
            track_event {
              name: "Track 0 Nested Event"
              type: TYPE_SLICE_BEGIN
              track_uuid: 10
              flow_ids: 57
            }
            trusted_packet_sequence_id: 123
          }
          packet {
            timestamp: 50
            track_event {
              name: "Track 0 Short Event"
              type: TYPE_SLICE_BEGIN
              track_uuid: 10
              terminating_flow_ids: 57
            }
            trusted_packet_sequence_id: 123
          }
        """),
        query="SELECT * FROM flow;",
        out=Csv("""
          "id","type","slice_out","slice_in","trace_id","arg_set_id"
          0,"flow",0,1,57,0
          1,"flow",1,2,57,0
                """))

  def test_clock_snapshot_table_multiplier(self):
    return DiffTestBlueprint(
        trace=TextProto("""
          packet {
            clock_snapshot {
              clocks {
                clock_id: 1
                timestamp: 42
                unit_multiplier_ns: 10
              }
              clocks {
                clock_id: 6
                timestamp: 0
              }
            }
          }
        """),
        query="SELECT TO_REALTIME(0);",
        out=Csv("""
          "TO_REALTIME(0)"
          420
        """))

  # Test cpu_track with machine_id ID.
  def test_cpu_track_table_machine_id(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 1
            event {
              timestamp: 100001000000
              pid: 10
              irq_handler_entry {
                irq: 100
                name : "resource1"
              }
            }
            event {
              timestamp: 100002000000
              pid: 10
              irq_handler_exit {
                irq: 100
                ret: 1
              }
            }
          }
          machine_id: 1001
        }
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 100003000000
              pid: 15
              irq_handler_entry {
                irq: 100
                name : "resource1"
              }
            }
          }
          machine_id: 1001
        }
        """),
        query="""
        SELECT
          type,
          cpu,
          machine_id
        FROM cpu_track
        ORDER BY type, cpu
        """,
        out=Csv("""
        "type","cpu","machine_id"
        "cpu_track",0,1
        "cpu_track",1,1
        """))
