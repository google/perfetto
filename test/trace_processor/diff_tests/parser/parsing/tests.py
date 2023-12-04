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
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Parsing(TestSuite):
  # Contains tests for parsing events which are applicable to more than one
  # "area". Generally, events here are of high importance (e.g. sched_switch
  # tested here is and is used by every embedder of trace processor)  Note:
  # generally *not* advisable to add tests here. Check the guidance provided
  # http://perfetto/dev/docs/analysis/trace-processor#diff-tests for choosing
  # folder to add a new test to. TODO(lalitm): some tests here should be moved
  # of here and into the area folders; they are only here because they predate
  # modularisation of diff tests. Sched
  def test_ts_desc_filter_android_sched_and_ps(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        query="""
        SELECT ts
        FROM sched
        JOIN thread USING(utid)
        WHERE tid = 23850
        ORDER BY ts DESC
        LIMIT 10;
        """,
        out=Csv("""
        "ts"
        81492536383477
        81491101817952
        81491101296858
        81491101029618
        81491099541806
        81491099514618
        81491099495504
        81491099477014
        81491098894566
        81491096076181
        """))

  # Sched reason
  def test_android_sched_and_ps_end_reason_eq(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        query="""
        SELECT end_state, count(*)
        FROM sched
        WHERE end_state = 'D'
        GROUP BY end_state;
        """,
        out=Csv("""
        "end_state","count(*)"
        "D",10503
        """))

  def test_android_sched_and_ps_end_reason_neq(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        query="""
        SELECT end_state, count(*)
        FROM sched
        WHERE end_state != 'D'
        GROUP BY end_state;
        """,
        out=Csv("""
        "end_state","count(*)"
        "DK",30
        "R",91189
        "R+",9428
        "S",110560
        "x",82
        """))

  # CPU Frequency
  def test_cpu_counters_b120487929(self):
    return DiffTestBlueprint(
        trace=DataPath('cpu_counters.pb'),
        query=Path('b120487929_test.sql'),
        out=Path('cpu_counters_b120487929.out'))

  # Test the filtering of ftrace events before tracing_start.
  def test_ftrace_with_tracing_start_list_sched_slice_spans(self):
    return DiffTestBlueprint(
        trace=Path('ftrace_with_tracing_start.py'),
        query="""
        SELECT ts, dur, tid
        FROM sched
        JOIN thread USING(utid)
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","tid"
        100,10,1
        110,-1,2
        """))

  # Scheduling slices from sched_switch events. There are two tests, one for
  # typical encoding of sched_switch events, and one for the same trace
  # in the compact format. The output should be identical apart from the
  # having one slice fewer for each cpu (the first compact sched_switch event
  # start a slice). Six slices in this case.
  def test_sched_slices_sched_switch_original(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_switch_original.pb'),
        query="""
        SELECT ts, cpu, dur, ts_end, end_state, priority, tid, name
        FROM sched JOIN thread ON sched.utid = thread.utid
        ORDER BY cpu, sched.ts ASC;
        """,
        out=Path('sched_slices_sched_switch_original.out'))

  def test_sched_slices_sched_switch_compact(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_switch_compact.pb'),
        query="""
        SELECT ts, cpu, dur, ts_end, end_state, priority, tid, name
        FROM sched JOIN thread ON sched.utid = thread.utid
        ORDER BY cpu, sched.ts ASC;
        """,
        out=Path('sched_slices_sched_switch_compact.out'))

  # Decoding of sched_waking events from a trace with compact scheduling
  # Verifies the contents of raw & instants tables.
  def test_sched_waking_raw_compact_sched(self):
    return DiffTestBlueprint(
        trace=DataPath('compact_sched.pb'),
        query=Path('sched_waking_raw_test.sql'),
        out=Path('sched_waking_raw_compact_sched.out'))

  def test_sched_waking_instants_compact_sched(self):
    return DiffTestBlueprint(
        trace=DataPath('compact_sched.pb'),
        query="""
        SELECT ts, thread.name, thread.tid
        FROM thread_state
        JOIN thread USING (utid)
        WHERE state = 'R'
        ORDER BY ts;
        """,
        out=Path('sched_waking_instants_compact_sched.out'))

  # Mm Event
  def test_mm_event(self):
    return DiffTestBlueprint(
        trace=DataPath('mm_event.pb'),
        query="""
        SELECT ts, name, value
        FROM counter
        JOIN counter_track
          ON counter.track_id = counter_track.id
        WHERE name GLOB 'mem.mm.*'
        ORDER BY ts
        LIMIT 40;
        """,
        out=Path('mm_event.out'))

  # Check the systrace conversion code in the raw table. Print events
  def test_print_systrace_lmk_userspace(self):
    return DiffTestBlueprint(
        trace=DataPath('lmk_userspace.pb'),
        query="""
        SELECT to_ftrace(id)
        FROM ftrace_event;
        """,
        out=Path('print_systrace_lmk_userspace.out'))

  def test_kernel_tmw_counter_process_counter_and_track(self):
    return DiffTestBlueprint(
        trace=Path('kernel_tmw_counter.textproto'),
        query="""
        SELECT ts, pct.name, value, pid
        FROM counter c
        JOIN process_counter_track pct ON c.track_id = pct.id
        JOIN process USING (upid)
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","name","value","pid"
        795572805481,"g2d_frame_hw#15",0.000000,237
        795572870504,"g2d_frame_sw#15",0.000000,237
        795620516581,"g2d_frame_sw#15",1.000000,237
        795620943421,"g2d_frame_hw#15",1.000000,237
        795623633810,"g2d_frame_hw#15",0.000000,237
        795623633810,"g2d_frame_hw#15",0.000000,237
        795623739848,"g2d_frame_sw#15",0.000000,237
        """))

  def test_kernel_dpu_tmw_counter_process_counter_and_track(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 2
            event {
              timestamp: 795572805481
              pid: 237
              dpu_tracing_mark_write {
                pid: 237
                name: "dpu_vote_clock"
                type: 67
                value: 123
              }
            }
            event {
              timestamp: 795572870504
              pid: 515
              dpu_tracing_mark_write {
                pid: 237
                name: "dpu_vote_clock"
                type: 67
                value: 100
              }
            }
            event {
              timestamp: 795620516581
              pid: 237
              dpu_tracing_mark_write {
                pid: 237
                name: "dpu_vote_clock"
                type: 67
                value: 125
              }
            }
            event {
              timestamp: 795620943421
              pid: 515
              dpu_tracing_mark_write {
                pid: 237
                name: "dpu_vote_clock"
                type: 67
                value: 100
              }
            }
          }
          trusted_uid: 9999
          trusted_packet_sequence_id: 3
        }
        """),
        query="""
        SELECT ts, pct.name, value, pid
        FROM counter c
        JOIN process_counter_track pct ON c.track_id = pct.id
        JOIN process USING (upid)
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","name","value","pid"
        795572805481,"dpu_vote_clock",123.000000,237
        795572870504,"dpu_vote_clock",100.000000,237
        795620516581,"dpu_vote_clock",125.000000,237
        795620943421,"dpu_vote_clock",100.000000,237
        """))

  # Unsigned integers
  def test_print_systrace_unsigned(self):
    return DiffTestBlueprint(
        trace=Path('print_systrace_unsigned.py'),
        query="""
        SELECT to_ftrace(id)
        FROM ftrace_event;
        """,
        out=Path('print_systrace_unsigned.out'))

  # cgroup_attach_task systrace conversion.
  def test_cgroup_attach_task_pre_s_print_systrace(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
            ftrace_events {
              cpu: 3
              event {
                timestamp: 74289018336
                pid: 1
                cgroup_attach_task {
                  dst_root: 1
                  dst_id: 2
                  pid: 3
                  comm: "foo"
                  cname: "bar"
                }
              }
            }
          }
        """),
        query="""
        SELECT to_ftrace(id)
        FROM ftrace_event;
        """,
        out=Path('cgroup_attach_task_pre_s_print_systrace.out'))

  def test_cgroup_attach_task_post_s_print_systrace(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
            ftrace_events {
              cpu: 3
              event {
                timestamp: 74289018336
                pid: 1
                cgroup_attach_task {
                  dst_root: 1
                  dst_id: 2
                  pid: 3
                  comm: "foo"
                  dst_level: 4
                  dst_path: "bar"
                }
              }
            }
          }
        """),
        query="""
        SELECT to_ftrace(id)
        FROM ftrace_event;
        """,
        out=Path('cgroup_attach_task_post_s_print_systrace.out'))

  # Parsing systrace files
  def test_systrace_html(self):
    return DiffTestBlueprint(
        trace=DataPath('systrace.html'),
        query="""
        SELECT ts, cpu, dur, ts_end, utid, end_state, priority, upid, name, tid
        FROM sched
        JOIN thread USING(utid)
        ORDER BY ts;
        """,
        out=Path('systrace_html.out'))

  def test_sched_smoke_trailing_empty(self):
    return DiffTestBlueprint(
        trace=DataPath('trailing_empty.systrace'),
        query="""
        SELECT COUNT(1)
        FROM sched;
        """,
        out=Csv("""
        "COUNT(1)"
        2
        """))

  # LMK handling
  def test_lmk_userspace_lmk(self):
    return DiffTestBlueprint(
        trace=DataPath('lmk_userspace.pb'),
        query="""
        SELECT ts, process.pid
        FROM instant
        JOIN process_track ON instant.track_id = process_track.id
        JOIN process USING (upid);
        """,
        out=Csv("""
        "ts","pid"
        732246100696424,17924
        732246180149452,21090
        732246388596557,21120
        732246415955101,21151
        """))

  def test_oom_kill(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          process_tree {
            processes {
              pid: 1000
              ppid: 1
              cmdline: "com.google.android.gm"
            }
            threads {
              tid: 1001
              tgid: 1000
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 4
            event {
              timestamp: 1234
              pid: 4321
              mark_victim {
                pid: 1001
              }
            }
          }
        }
        """),
        query="""
        SELECT ts, instant.name, process.pid, process.name
        FROM instant
        JOIN thread_track ON instant.track_id = thread_track.id
        JOIN thread USING (utid)
        JOIN process USING (upid);
        """,
        out=Csv("""
        "ts","name","pid","name"
        1234,"mem.oom_kill",1000,"com.google.android.gm"
        """))

  # Logcat
  def test_android_log_counts(self):
    return DiffTestBlueprint(
        trace=DataPath('android_log.pb'),
        query=Path('android_log_counts_test.sql'),
        out=Csv("""
        "cnt"
        2249
        431
        264
        2
        4
        31
        246
        """))

  def test_android_log_msgs(self):
    return DiffTestBlueprint(
        trace=DataPath('android_log.pb'),
        query=Path('android_log_msgs_test.sql'),
        out=Path('android_log_msgs.out'))

  def test_android_log_ring_buffer_mode(self):
    return DiffTestBlueprint(
        trace=DataPath('android_log_ring_buffer_mode.pb'),
        query="""
        SELECT count(*) FROM android_logs;
        """,
        out=Csv("""
        "count(*)"
        26
        """))

  # Oom Score
  def test_synth_oom_oom_query(self):
    return DiffTestBlueprint(
        trace=Path('synth_oom.py'),
        query=Path('oom_query_test.sql'),
        out=Path('synth_oom_oom_query.out'))

  def test_process_stats_poll_oom_score(self):
    return DiffTestBlueprint(
        trace=DataPath('process_stats_poll.pb'),
        query="""
        SELECT ts, name, value, upid
        FROM counter c
        JOIN process_counter_track t
          ON c.track_id = t.id
        WHERE name = "oom_score_adj"
        ORDER BY ts
        LIMIT 20;
        """,
        out=Path('process_stats_poll_oom_score.out'))

  # Stats
  def test_android_sched_and_ps_stats(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        query="""
        SELECT name, idx, severity, source, value
        FROM stats WHERE name GLOB 'ftrace_cpu_*' OR name GLOB 'traced_buf_*';
        """,
        out=Path('android_sched_and_ps_stats.out'))

  # Syscalls
  def test_sys_syscall(self):
    return DiffTestBlueprint(
        trace=Path('syscall.py'),
        query="""
        SELECT ts, dur, name
        FROM slices
        LIMIT 10;
        """,
        out=Csv("""
        "ts","dur","name"
        100,6,"sys_io_setup"
        105,5,"sys_io_destroy"
        """))

  # thread_slice tables.
  def test_thread_time_in_thread_slice(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_json_v2.json'),
        query="""
        SELECT
          name, thread_ts, thread_dur
        FROM slice;
        """,
        out=Csv("""
        "name","thread_ts","thread_dur"
        "SenderB",1000,5000
        "Blergh","[NULL]","[NULL]"
        "SenderA",3005000,7000
        "OtherSlice",3204000,100000
        "SomeSlice",3335000,340000
        "SomeOtherSlice",3335000,996000
        "SomeOtherSliceInstant","[NULL]","[NULL]"
        """))

  # Initial display state
  def test_initial_display_state(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet: {
          timestamp: 1
          initial_display_state: {
            display_state: 2
            brightness: 0.5
          }
        }
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 1234
              print {
                buf: "C|5678|ScreenState|0\n"
              }
            }
          }
        }
        """),
        query="""
        SELECT t.name,
               c.ts,
               c.value
        FROM counter_track t
        JOIN counter c ON t.id = c.track_id
        WHERE t.name = 'ScreenState';
        """,
        out=Csv("""
        "name","ts","value"
        "ScreenState",1,2.000000
        "ScreenState",1000,0.000000
        """))

  # Config & metadata
  def test_config_metadata(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          clock_snapshot {
            clocks {
              clock_id: 6
              timestamp: 101000002
            }
            clocks {
              clock_id: 128
              timestamp: 2
            }
          }
          timestamp: 101000002
        }
        packet {
          trace_config {
            trace_uuid_msb: 1314564453825188563
            trace_uuid_lsb: -6605018796207623390
          }
        }
        packet {
          system_info {
            android_build_fingerprint: "the fingerprint"
          }
        }
        """),
        query="""
        SELECT name, str_value FROM metadata WHERE str_value IS NOT NULL ORDER BY name;
        """,
        out=Csv("""
        "name","str_value"
        "android_build_fingerprint","the fingerprint"
        "trace_config_pbtxt","trace_uuid_msb: 1314564453825188563
        trace_uuid_lsb: -6605018796207623390"
        "trace_type","proto"
        "trace_uuid","123e4567-e89b-12d3-a456-426655443322"
        """))

  def test_triggers_packets_trigger_packet_trace(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trigger {
            trigger_name: "test1"
            trusted_producer_uid: 3
            producer_name: "producer1"
          }
          timestamp: 101000002
        }
        packet {
          trigger {
            trigger_name: "test2"
            trusted_producer_uid: 4
            producer_name: "producer2"
          }
          timestamp: 101000004
        }
        """),
        query=Path('triggers_packets_test.sql'),
        out=Csv("""
        "ts","name","string_value","int_value"
        101000002,"test1","producer1",3
        101000004,"test2","producer2",4
        """))

  def test_chrome_metadata(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          clock_snapshot {
            clocks {
              clock_id: 6
              timestamp: 101000002
            }
          }
          trusted_packet_sequence_id: 1
          timestamp: 101000002
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 101000002
          chrome_metadata {
            background_tracing_metadata {
              triggered_rule {
                name_hash: 1595654158
              }
              scenario_name_hash: 3005533841
            }
            chrome_version_code: 101
            enabled_categories: "cat1,cat2,cat3"
          }
        }
        """),
        query="""
        SELECT * FROM metadata;
        """,
        out=Path('chrome_metadata.out'))

  # CPU info
  def test_cpu(self):
    return DiffTestBlueprint(
        trace=Path('cpu_info.textproto'),
        query="""
        SELECT
          id,
          cluster_id,
          processor
        FROM cpu;
        """,
        out=Csv("""
        "id","cluster_id","processor"
        0,0,"AArch64 Processor rev 13 (aarch64)"
        1,0,"AArch64 Processor rev 13 (aarch64)"
        2,0,"AArch64 Processor rev 13 (aarch64)"
        3,0,"AArch64 Processor rev 13 (aarch64)"
        4,0,"AArch64 Processor rev 13 (aarch64)"
        5,0,"AArch64 Processor rev 13 (aarch64)"
        6,1,"AArch64 Processor rev 13 (aarch64)"
        7,1,"AArch64 Processor rev 13 (aarch64)"
        """))

  def test_cpu_freq(self):
    return DiffTestBlueprint(
        trace=Path('cpu_info.textproto'),
        query="""
        SELECT
          freq,
          GROUP_CONCAT(cpu_id) AS cpus
        FROM cpu_freq
        GROUP BY freq
        ORDER BY freq;
        """,
        out=Path('cpu_freq.out'))

  # Trace size
  def test_android_sched_and_ps_trace_size(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        query="""
        SELECT int_value FROM metadata WHERE name = 'trace_size_bytes';
        """,
        out=Csv("""
        "int_value"
        18761615
        """))

  # Package list handling
  def test_android_package_list(self):
    return DiffTestBlueprint(
        trace=Path('android_package_list.py'),
        query=Metric('android_package_list'),
        out=TextProto(r"""
        android_package_list {
          packages {
            package_name: "com.my.pkg"
            uid: 123
            version_code: 456000
          }
        }
        """))

  # Ensures process -> package matching works as expected.
  def test_process_metadata_matching(self):
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
              cmdline: "system_server"
              uid: 1000
            }
            processes {
              pid: 3
              ppid: 1
              cmdline: "com.google.android.gms"
              uid: 10100
            }
            processes {
              pid: 4
              ppid: 1
              cmdline: "com.google.android.gms.persistent"
              uid: 10100
            }
            processes {
              pid: 5
              ppid: 1
              cmdline: "com.google.android.gms"
              uid: 1010100
            }
          }
        }
        packet {
          packages_list {
            packages {
              name: "com.google.android.gms"
              uid: 10100
              version_code: 1234
            }
            packages {
              name: "com.google.android.gsf"
              uid: 10100
              version_code: 1
            }
          }
        }
        """),
        query="""
        SELECT RUN_METRIC('android/process_metadata.sql');

        SELECT upid, process_name, uid, shared_uid, package_name, version_code
        FROM process_metadata_table
        WHERE upid != 0;
        """,
        out=Csv("""
        "upid","process_name","uid","shared_uid","package_name","version_code"
        1,"init",0,"[NULL]","[NULL]","[NULL]"
        2,"system_server",1000,"[NULL]","[NULL]","[NULL]"
        3,"com.google.android.gms",10100,1,"com.google.android.gms",1234
        4,"com.google.android.gms.persistent",10100,1,"com.google.android.gms",1234
        5,"com.google.android.gms",10100,1,"com.google.android.gms",1234
        """))

  # Flow events importing from json
  def test_flow_events_json_v1(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_json_v1.json'),
        query="""
        SELECT t1.name AS slice_out, t2.name AS slice_in FROM flow t
        JOIN slice t1 ON t.slice_out = t1.slice_id
        JOIN slice t2 ON t.slice_in = t2.slice_id;
        """,
        out=Csv("""
        "slice_out","slice_in"
        "SenderB","Blergh"
        "SenderA","OtherSlice"
        "OtherSlice","SomeSlice"
        """))

  def test_flow_events_json_v2(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_json_v2.json'),
        query="""
        SELECT t1.name AS slice_out, t2.name AS slice_in FROM flow t
        JOIN slice t1 ON t.slice_out = t1.slice_id
        JOIN slice t2 ON t.slice_in = t2.slice_id;
        """,
        out=Csv("""
        "slice_out","slice_in"
        "SenderB","Blergh"
        "SenderA","OtherSlice"
        "OtherSlice","SomeSlice"
        "OtherSlice","SomeOtherSlice"
        """))

  # Importing displayTimeUnit
  def test_display_time_unit_slices(self):
    return DiffTestBlueprint(
        trace=Json(r"""
        {"displayTimeUnit":"ns","traceEvents":[
          {
            "name": "process_name",
            "pid": 1,
            "ph": "M",
            "args": {
              "name": "api-service-65fc94b8c7-68w9w"
            }
          },
          {
            "name": "add_graph",
            "pid": 1,
            "tid": 1,
            "ph": "B",
            "ts": 1597071955492308
          },
          {
            "name": "add_graph",
            "pid": 1,
            "tid": 1,
            "ph": "E",
            "ts": 1597071955703771
          }
        ]
        }
        """),
        query="""
        SELECT ts, dur, name FROM slice ORDER BY ts DESC;
        """,
        out=Csv("""
        "ts","dur","name"
        1597071955492308000,211463000,"add_graph"
        """))

  # Parsing sched_blocked_reason
  def test_sched_blocked_proto_sched_blocked_reason(self):
    return DiffTestBlueprint(
        trace=Path('sched_blocked_proto.py'),
        query="""
        SELECT ts, tid, io_wait
        FROM thread_state
        JOIN thread USING (utid)
        WHERE state = 'D'
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","tid","io_wait"
        100,1,0
        110,2,1
        """))

  def test_sched_blocked_systrace_sched_blocked_reason(self):
    return DiffTestBlueprint(
        trace=Path('sched_blocked_systrace.systrace'),
        query="""
        SELECT ts, tid, io_wait
        FROM thread_state
        JOIN thread USING (utid)
        WHERE state = 'D'
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","tid","io_wait"
        20258854000,269,0
        21123838000,2172,1
        """))

  # Kernel symbolization
  def test_sched_blocked_reason_symbolized_sched_blocked_reason_function(self):
    return DiffTestBlueprint(
        trace=Path('sched_blocked_reason_symbolized.textproto'),
        query="""
        SELECT
          ts,
          thread.tid AS pid,
          blocked_function AS func
        FROM thread_state
        JOIN thread USING (utid)
        WHERE state = 'D'
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","pid","func"
        999000,105,"some_fn"
        999000,102,"filemap_fault"
        1000000,100,"filemap_fault"
        1001000,101,"[NULL]"
        1002000,103,"[NULL]"
        1003000,100,"some_other_fn"
        1005000,104,"filemap_fault"
        """))

  def test_sched_blocked_reason_symbolized_to_systrace(self):
    return DiffTestBlueprint(
        trace=Path('sched_blocked_reason_symbolized.textproto'),
        query="""
        SELECT to_ftrace(id) AS line
        FROM ftrace_event;
        """,
        out=Path('sched_blocked_reason_symbolized_to_systrace.out'))

  # Floating point numbers
  def test_decimal_timestamp_slices(self):
    return DiffTestBlueprint(
        trace=Json(r"""
        {
          "traceEvents": [{
            "pid": 1234,
            "tid": 1234,
            "ts": 5.1,
            "dur": 500.1,
            "name": "name.exec",
            "ph": "XXX",
            "cat": "aaa"
          }]
        }
        """),
        query="""
        SELECT ts, dur, name FROM slice ORDER BY ts DESC;
        """,
        out=Csv("""
        "ts","dur","name"
        5100,500100,"name.exec"
        """))

  # JSON instants and counters
  def test_counters_json_counters(self):
    return DiffTestBlueprint(
        trace=Json(r"""

        [
            {"pid": "1000", "name": "ctr", "ph": "C", "ts":  0, "args": {"cats":  0}},
            {"pid": "1000", "name": "ctr", "ph": "C", "ts": 10, "args": {"cats": 10}},
            {"pid": "1000", "name": "ctr", "ph": "C", "ts": 20, "args": {"cats":  0}}
        ]
        """),
        query="""
        SELECT
          process_counter_track.name,
          counter.ts,
          counter.value
        FROM counter
        JOIN process_counter_track ON (counter.track_id = process_counter_track.id);
        """,
        out=Csv("""
        "name","ts","value"
        "ctr cats",0,0.000000
        "ctr cats",10000,10.000000
        "ctr cats",20000,0.000000
        """))

  def test_instants_json_instants(self):
    return DiffTestBlueprint(
        trace=DataPath('instants.json'),
        query="""
        SELECT
          slice.ts,
          slice.name AS slice_name,
          thread.tid,
          process.pid
        FROM slice
        JOIN track ON (slice.track_id = track.id)
        LEFT JOIN thread_track ON (slice.track_id = thread_track.id)
        LEFT JOIN thread ON (thread_track.utid = thread.utid)
        LEFT JOIN process_track ON (slice.track_id = process_track.id)
        LEFT JOIN process ON (process_track.upid = process.upid)
        WHERE dur = 0;
        """,
        out=Csv("""
        "ts","slice_name","tid","pid"
        1234523300,"Thread",2347,"[NULL]"
        1235523300,"Global","[NULL]","[NULL]"
        1236523300,"Process","[NULL]",2320
        1237523300,"Nonei",6790,"[NULL]"
        1238523300,"NoneI",6790,"[NULL]"
        1239523300,"NoneR",6790,"[NULL]"
        """))

  # Trace quality metric
  def test_very_long_sched_android_trace_quality(self):
    return DiffTestBlueprint(
        trace=Path('very_long_sched.py'),
        query=Metric('android_trace_quality'),
        out=TextProto(r"""
        android_trace_quality {
          failures {
            name: "sched_slice_too_long"
          }
        }
        """))

  # Regression test for b/193721088 (infra prepending " done\n" to atrace)
  def test_sched_smoke_trailing_empty_2(self):
    return DiffTestBlueprint(
        trace=DataPath('atrace_b_193721088.atr'),
        query="""
        SELECT COUNT(1)
        FROM sched;
        """,
        out=Csv("""
        "COUNT(1)"
        2
        """))

  # Multiuser
  def test_android_multiuser_switch(self):
    return DiffTestBlueprint(
        trace=Path('android_multiuser_switch.textproto'),
        query=Metric('android_multiuser'),
        out=TextProto(r"""
        android_multiuser: {
          user_switch: {
            duration_ms: 4900
          }
        }
        """))

  # Output of atrace -z.
  def test_atrace_compressed_sched_count(self):
    return DiffTestBlueprint(
        trace=DataPath('atrace_compressed.ctrace'),
        query="""
        SELECT COUNT(1)
        FROM sched;
        """,
        out=Csv("""
        "COUNT(1)"
        1120
        """))

  # Output of adb shell "atrace -t 1 sched" > out.txt". It has extra garbage
  # from stderr before the TRACE: marker. See b/208691037.
  def test_atrace_uncompressed_sched_count(self):
    return DiffTestBlueprint(
        trace=DataPath('atrace_uncompressed_b_208691037'),
        query="""
        SELECT COUNT(1)
        FROM sched;
        """,
        out=Csv("""
        "COUNT(1)"
        9
        """))

  def test_otheruuids_android_other_traces(self):
    return DiffTestBlueprint(
        trace=Path('otheruuids.textproto'),
        query=Metric('android_other_traces'),
        out=TextProto(r"""
        android_other_traces {
          finalized_traces_uuid: "75e4c6d0-d8f6-4f82-fa4b-9e09c5512288"
          finalized_traces_uuid: "ad836701-3113-3fb1-be4f-f7731e23fbbf"
          finalized_traces_uuid: "0de1a010-efa1-a081-2345-969b1186a6ab"
        }
        """))

  # Per-process Binder transaction metrics
  def test_android_binder(self):
    return DiffTestBlueprint(
        trace=Path('android_binder.py'),
        query=Metric('android_binder'),
        out=TextProto(r"""
        android_binder {
          process_breakdown {
            process_name: "test_process_a"
            pid: 1
            slice_name: "binder transaction"
            count: 2
          }
          process_breakdown {
            process_name: "test_process_b"
            pid: 2
            slice_name: "binder reply"
            count: 1
          }
          process_breakdown {
            process_name: "test_process_c"
            pid: 3
            slice_name: "binder reply"
            count: 1
          }
        }
        """))

  # Statsd Atoms
  def test_statsd_atoms_all_atoms(self):
    return DiffTestBlueprint(
        trace=DataPath('statsd_atoms.pb'),
        query=Path('all_atoms_test.sql'),
        out=Path('statsd_atoms_all_atoms.out'))

  # Kernel function tracing.
  def test_funcgraph_trace_funcgraph(self):
    return DiffTestBlueprint(
        trace=Path('funcgraph_trace.textproto'),
        query="""
        SELECT ts, dur, tid, s.name, depth
        FROM slices s
        JOIN thread_track tt ON (s.track_id = tt.id)
        JOIN thread USING (utid)
        WHERE tid = 385482;
        """,
        out=Csv("""
        "ts","dur","tid","name","depth"
        679375600673065,3797,385482,"__handle_mm_fault",0
        679375600673769,1726,385482,"alloc_pages_vma",1
        """))

  # Kernel task_newtask waker_utid parsing
  def test_task_newtask_waker_utid(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          first_packet_on_sequence: true
          ftrace_events {
            cpu: 1
            event {
              timestamp: 201315132677
              pid: 518
              task_newtask {
                pid: 3294
                comm: "adbd"
                clone_flags: 18874368
                oom_score_adj: -1000
              }
            }
            event {
              timestamp: 201319417828
              pid: 518
              task_newtask {
                pid: 3295
                comm: "adbd"
                clone_flags: 4001536
                oom_score_adj: -1000
              }
            }
          }
          trusted_uid: 9999
          trusted_packet_sequence_id: 2
          trusted_pid: 521
          previous_packet_dropped: true
        }
        """),
        query="""
        SELECT waker_utid FROM thread_state
        """,
        out=Csv("""
        "waker_utid"
        1
        1
        """))

  # Parsing of mdss/tracing_mark_write events
  def test_slice_mdss_tracing_mark_write(self):
    # Note that tracing_mark_write below is an ftrace event from the mdss
    # group (see mdss.proto and aosp/2622569). Events with the same name
    # from other groups (e.g. sde) listed after mdss in ftrace_proto_gen
    # are prefixed with the group name to avoid conflicts.
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 100
              pid: 584
              tracing_mark_write {
                pid: 584
                trace_name: "test_event"
                trace_begin: 1
              }
            }
            event {
              timestamp: 200
              pid: 584
              tracing_mark_write {
                pid: 584
                trace_name: "test_event"
                trace_begin: 0
              }
            }
          }
        }
        """),
        query="""
        SELECT s.name, dur, tid
        FROM slice s
        JOIN thread_track t ON s.track_id = t.id
        JOIN thread USING(utid)
        """,
        out=Csv("""
        "name","dur","tid"
        "test_event",100,584
        """))

  def test_all_data_source_flushed_metadata(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 12344
          service_event {
            all_data_sources_flushed: true
          }
        }
        packet {
          timestamp: 12345
          service_event {
            all_data_sources_flushed: true
          }
        }
        """),
        query="""
        SELECT name, int_value FROM metadata WHERE name = 'all_data_source_flushed_ns'""",
        out=Csv("""
        "name","int_value"
        "all_data_source_flushed_ns",12344
        "all_data_source_flushed_ns",12345
        """))
