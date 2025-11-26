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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
from python.generators.diff_tests.testing import TestSuite


class Tables(TestSuite):
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
        SELECT cpu
        FROM cpu_track
        ORDER BY cpu;
        """,
        out=Csv("""
        "cpu"
        0
        1
        """))

  def test_thread_state_flattened_aggregated(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
          INCLUDE PERFETTO MODULE sched.thread_state_flattened;
          select *
          from _get_flattened_thread_state_aggregated(11155, NULL);
        """,
        out=Path('thread_state_flattened_aggregated_csv.out'))

  def test_thread_state_flattened(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
          INCLUDE PERFETTO MODULE sched.thread_state_flattened;
          SELECT
            ts,
            dur,
            utid,
            depth,
            name,
            slice_id,
            cpu,
            state,
            io_wait,
            blocked_function,
            waker_utid,
            irq_context
          FROM _get_flattened_thread_state(11155, NULL);
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
        query="SELECT id, slice_out, slice_in, trace_id, arg_set_id FROM flow;",
        out=Csv("""
          "id","slice_out","slice_in","trace_id","arg_set_id"
          0,0,1,57,"[NULL]"
          1,1,2,57,"[NULL]"
        """))

  def test_flow_direction_corrected_by_timestamp(self):
    return DiffTestBlueprint(
        trace=TextProto("""
          packet {
            timestamp: 50
            track_event {
              name: "Slice A"
              type: TYPE_SLICE_BEGIN
              track_uuid: 10
            }
            trusted_packet_sequence_id: 123
          }
          packet {
            timestamp: 100
            track_event {
              name: "Slice B"
              type: TYPE_SLICE_BEGIN
              track_uuid: 20
              flow_ids: 42
            }
            trusted_packet_sequence_id: 123
          }
          packet {
            timestamp: 200
            track_event {
              name: "Slice B"
              type: TYPE_SLICE_END
              track_uuid: 20
            }
            trusted_packet_sequence_id: 123
          }
          packet {
            timestamp: 400
            track_event {
              name: "Slice A"
              type: TYPE_SLICE_END
              track_uuid: 10
              flow_ids: 42
            }
            trusted_packet_sequence_id: 123
          }
        """),
        query="""
        SELECT 
          s_out.name as name_out,
          s_out.ts as ts_out,
          s_in.name as name_in,
          s_in.ts as ts_in,
          f.trace_id
        FROM flow f
        JOIN slice s_out ON f.slice_out = s_out.id  
        JOIN slice s_in ON f.slice_in = s_in.id
        ORDER BY f.id;
        """,
        out=Csv("""
          "name_out","ts_out","name_in","ts_in","trace_id"
          "Slice A",50,"Slice B",100,42
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
          c.ucpu,
          ct.cpu,
          c.machine_id
        FROM cpu_track AS ct
        JOIN cpu AS c ON ct.machine_id IS c.machine_id AND ct.cpu = c.cpu
        ORDER BY ct.cpu
        """,
        out=Csv("""
        "ucpu","cpu","machine_id"
        4096,0,1
        4097,1,1
        """))

  def test_async_slice_utid_arg_set_id(self):
    return DiffTestBlueprint(
        trace=DataPath('android_monitor_contention_trace.atr'),
        query="""
        SELECT COUNT(DISTINCT extract_arg(arg_set_id, 'utid')) AS utid_count,
        COUNT(DISTINCT extract_arg(arg_set_id, 'end_utid')) AS end_utid_count
        FROM counter
        """,
        out=Csv("""
        "utid_count","end_utid_count"
        89,0
        """))

  def test_machine(self):
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
            num_cpus: 4
          }
          trusted_uid: 158158
          trusted_packet_sequence_id: 1
        }
        packet {
          system_info {
            utsname {
              sysname: "Linux"
              version: "#1 SMP PREEMPT Wed Apr  2 01:42:00 UTC 2025"
              release: "6.6.82-android15-8-g1a7680db913a-ab13304129"
              machine: "x86_64"
            }
            android_build_fingerprint: "android_test_fingerprint"
            android_device_manufacturer: "Android"
            android_soc_model: "some_soc_model"
            tracing_service_version: "Perfetto v50.1 (N/A)"
            android_sdk_version: 33
            page_size: 4096
            num_cpus: 8
            timezone_off_mins: 0
          }
          machine_id: 2420838448
          trusted_uid: 158158
          trusted_packet_sequence_id: 1
        }
        """),
        query="""
        SELECT * FROM machine
        """,
        out=Csv("""
        "id","raw_id","sysname","release","version","arch","num_cpus","android_build_fingerprint","android_device_manufacturer","android_sdk_version"
        0,0,"Darwin","22.6.0","Foobar","x86_64",4,"[NULL]","[NULL]","[NULL]"
        1,2420838448,"Linux","6.6.82-android15-8-g1a7680db913a-ab13304129","#1 SMP PREEMPT Wed Apr  2 01:42:00 UTC 2025","x86_64",8,"android_test_fingerprint","Android",33
        """))

  # user list table
  def test_android_user_list(self):
    return DiffTestBlueprint(
        trace=DataPath('trace_user_list.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE android.user_list;


        SELECT
          android_user_id,
          type
        FROM android_user_list
        ORDER BY android_user_id;
        """,
        out=Csv("""
        "android_user_id","type"
        0,"android.os.usertype.system.HEADLESS" 
        10,"android.os.usertype.full.SECONDARY" 
        11,"android.os.usertype.full.GUEST"
        """))