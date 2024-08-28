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
        "__intrinsic_cpu_track",0
        "__intrinsic_cpu_track",1
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
          ct.type,
          ct.ucpu,
          c.cpu,
          ct.machine_id
        FROM cpu_track AS ct
        JOIN cpu AS c ON ct.ucpu = c.id
        ORDER BY ct.type, c.cpu
        """,
        out=Csv("""
        "type","ucpu","cpu","machine_id"
        "__intrinsic_cpu_track",4096,0,1
        "__intrinsic_cpu_track",4097,1,1
        """))
