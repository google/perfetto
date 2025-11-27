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


class TrackEvent(TestSuite):
  # Contains tests on the parsing and ingestion of TrackEvent packets. Same
  # handling
  def test_track_event_same_tids_threads(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          incremental_state_cleared: true
          track_descriptor {
            uuid: 1
            thread {
              pid: 5
              tid: 1
              thread_name: "t1"
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          track_descriptor {
            uuid: 2
            thread {
              pid: 10
              tid: 1
              thread_name: "t2"
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          track_event {
            track_uuid: 1
            categories: "cat"
            name: "name1"
            type: 3
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          track_event {
            track_uuid: 2
            categories: "cat"
            name: "name2"
            type: 3
          }
        }
        """),
        query="""
        SELECT tid, pid, process.name AS pname, thread.name AS tname
        FROM thread
        LEFT JOIN process USING(upid)
        WHERE tid > 0
        ORDER BY tid;
        """,
        out=Csv("""
        "tid","pid","pname","tname"
        1,5,"[NULL]","t1"
        1,10,"[NULL]","t2"
        5,5,"[NULL]","[NULL]"
        10,10,"[NULL]","[NULL]"
        """))

  def test_track_event_same_tids_slices(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          incremental_state_cleared: true
          track_descriptor {
            uuid: 1
            thread {
              pid: 5
              tid: 1
              thread_name: "t1"
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          track_descriptor {
            uuid: 2
            thread {
              pid: 10
              tid: 1
              thread_name: "t2"
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          track_event {
            track_uuid: 1
            categories: "cat"
            name: "name1"
            type: 3
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          track_event {
            track_uuid: 2
            categories: "cat"
            name: "name2"
            type: 3
          }
        }
        """),
        query="""
        SELECT
          track.name AS track,
          process.name AS process,
          thread.name AS thread,
          thread_process.name AS thread_process,
          slice.ts,
          slice.dur,
          slice.category,
          slice.name
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN process_track ON slice.track_id = process_track.id
        LEFT JOIN process ON process_track.upid = process.upid
        LEFT JOIN thread_track ON slice.track_id = thread_track.id
        LEFT JOIN thread ON thread_track.utid = thread.utid
        LEFT JOIN process thread_process ON thread.upid = thread_process.upid
        ORDER BY ts ASC;
        """,
        out=Csv("""
        "track","process","thread","thread_process","ts","dur","category","name"
        "[NULL]","[NULL]","t1","[NULL]",1000,0,"cat","name1"
        "[NULL]","[NULL]","t2","[NULL]",2000,0,"cat","name2"
        """))

  # Typed args
  def test_track_event_typed_args_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_typed_args.textproto'),
        query="""
        SELECT
          track.name AS track,
          process.name AS process,
          thread.name AS thread,
          thread_process.name AS thread_process,
          slice.ts,
          slice.dur,
          slice.category,
          slice.name
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN process_track ON slice.track_id = process_track.id
        LEFT JOIN process ON process_track.upid = process.upid
        LEFT JOIN thread_track ON slice.track_id = thread_track.id
        LEFT JOIN thread ON thread_track.utid = thread.utid
        LEFT JOIN process thread_process ON thread.upid = thread_process.upid
        ORDER BY ts ASC;
        """,
        out=Csv("""
        "track","process","thread","thread_process","ts","dur","category","name"
        "[NULL]","[NULL]","t1","[NULL]",1000,0,"cat","name1"
        "[NULL]","[NULL]","t1","[NULL]",2000,0,"cat","name2"
        "[NULL]","[NULL]","t1","[NULL]",3000,0,"cat","name3"
        "[NULL]","[NULL]","t1","[NULL]",4000,0,"cat","name4"
        "[NULL]","[NULL]","t1","[NULL]",6000,0,"cat","name5"
        "[NULL]","[NULL]","t1","[NULL]",7000,0,"cat","name6"
        """))

  def test_track_event_typed_args_args(self):
    return DiffTestBlueprint(
        trace=Path('track_event_typed_args.textproto'),
        query="""
        SELECT
          flat_key,
          key,
          int_value,
          string_value
        FROM args
        ORDER BY key, display_value, arg_set_id, key ASC;
        """,
        out=Path('track_event_typed_args_args.out'))

  # Track handling
  def test_track_event_tracks_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks.textproto'),
        query="""
        SELECT
          track.name AS track,
          process.name AS process,
          thread.name AS thread,
          thread_process.name AS thread_process,
          slice.ts,
          slice.dur,
          slice.category,
          slice.name
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN process_track ON slice.track_id = process_track.id
        LEFT JOIN process ON process_track.upid = process.upid
        LEFT JOIN thread_track ON slice.track_id = thread_track.id
        LEFT JOIN thread ON thread_track.utid = thread.utid
        LEFT JOIN process thread_process ON thread.upid = thread_process.upid
        ORDER BY ts ASC;
        """,
        out=Csv("""
      "track","process","thread","thread_process","ts","dur","category","name"
      "[NULL]","[NULL]","t1","p1",1000,0,"cat","event1_on_t1"
      "[NULL]","[NULL]","t2","p1",2000,0,"cat","event1_on_t2"
      "[NULL]","[NULL]","t2","p1",3000,0,"cat","event2_on_t2"
      "[NULL]","p1","[NULL]","[NULL]",4000,0,"cat","event1_on_p1"
      "async","p1","[NULL]","[NULL]",5000,0,"cat","event1_on_async"
      "async2","p1","[NULL]","[NULL]",5100,100,"cat","event1_on_async2"
      "async3","[NULL]","t2","p1",6000,0,"cat","event1_on_async3"
      "[NULL]","[NULL]","t1","p1",6000,0,"cat","event3_on_t1"
      "[NULL]","[NULL]","t3","p1",11000,0,"cat","event1_on_t3"
      "[NULL]","p2","[NULL]","[NULL]",21000,0,"cat","event1_on_p2"
      "[NULL]","[NULL]","t4","p2",22000,0,"cat","event1_on_t4"
      "Default Track","[NULL]","[NULL]","[NULL]",30000,0,"cat","event1_on_t1"
      "[NULL]","p2","[NULL]","[NULL]",31000,0,"cat","event2_on_p2"
      "[NULL]","[NULL]","t4","p2",32000,0,"cat","event2_on_t4"
      "event_and_track_async3","p1","[NULL]","[NULL]",40000,0,"cat","event_and_track_async3"
        """))

  def test_track_event_tracks_processes(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks.textproto'),
        query="""
        SELECT
          id,
          name,
          extract_arg(arg_set_id, "chrome.host_app_package_name") AS host_app
        FROM process;
        """,
        out=Csv("""
        "id","name","host_app"
        0,"[NULL]","[NULL]"
        1,"p1","host_app"
        2,"p2","[NULL]"
        """))

  def test_track_event_tracks(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks.textproto'),
        query="""
        WITH track_with_name AS (
          SELECT
            COALESCE(
              t1.name,
              'thread=' || thread.name,
              'process=' || process.name,
              'tid=' || thread.tid,
              'pid=' || process.pid
            ) AS full_name,
            *
          FROM track t1
          LEFT JOIN thread_track t2 USING (id)
          LEFT JOIN thread USING (utid)
          LEFT JOIN process_track t3 USING (id)
          LEFT JOIN process ON t3.upid = process.id
          ORDER BY id
        )
        SELECT
        t1.full_name AS name,
        EXTRACT_ARG(t1.source_arg_set_id, 'has_first_packet_on_sequence')
               AS has_first_packet_on_sequence
        FROM track_with_name t1
        ORDER BY 1, 2;
        """,
        out=Csv("""
        "name","has_first_packet_on_sequence"
        "Default Track","[NULL]"
        "async",1
        "async2",1
        "async3",1
        "event_and_track_async3",1
        "process=p1",1
        "process=p2","[NULL]"
        "process=p2","[NULL]"
        "thread=t1",1
        "thread=t2",1
        "thread=t3",1
        "thread=t4","[NULL]"
        """))

  def test_track_event_descriptions(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks.textproto'),
        query="""
        WITH track_with_name AS (
          SELECT
            COALESCE(
              t1.name,
              'thread=' || thread.name,
              'process=' || process.name,
              'tid=' || thread.tid,
              'pid=' || process.pid
            ) AS full_name,
            *
          FROM track t1
          LEFT JOIN thread_track t2 USING (id)
          LEFT JOIN thread USING (utid)
          LEFT JOIN process_track t3 USING (id)
          LEFT JOIN process ON t3.upid = process.id
          ORDER BY id
        )
        SELECT
        t1.full_name AS name,
        EXTRACT_ARG(t1.source_arg_set_id, 'description') AS description
        FROM track_with_name t1
        ORDER BY 1, 2;
        """,
        out=Csv("""
        "name","description"
        "Default Track","[NULL]"
        "async","Async events for p1"
        "async2","[NULL]"
        "async3","Async events for t2"
        "event_and_track_async3","[NULL]"
        "process=p1","Chrome process: p1"
        "process=p2","[NULL]"
        "process=p2","[NULL]"
        "thread=t1","Thread t1"
        "thread=t2","Thread t2"
        "thread=t3","[NULL]"
        "thread=t4","[NULL]"
        """))

  # Instant events
  def test_track_event_instant_slices(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          incremental_state_cleared: true
          track_descriptor {
            uuid: 1
            thread {
              pid: 5
              tid: 1
              thread_name: "t1"
            }
          }
          trace_packet_defaults {
            track_event_defaults {
              track_uuid: 1
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          track_event {
            categories: "cat"
            name: "instant_on_t1"
            type: 3
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          track_event {
            categories: "cat"
            name: "legacy_instant_on_t1"
            legacy_event {
              phase: 73               # 'I'
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 3000
          track_event {
            categories: "cat"
            name: "legacy_mark_on_t1"
            legacy_event {
              phase: 82               # 'R'
            }
          }
        }
        """),
        query="""
        SELECT
          track.name AS track,
          process.name AS process,
          thread.name AS thread,
          thread_process.name AS thread_process,
          slice.ts,
          slice.dur,
          slice.category,
          slice.name
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN process_track ON slice.track_id = process_track.id
        LEFT JOIN process ON process_track.upid = process.upid
        LEFT JOIN thread_track ON slice.track_id = thread_track.id
        LEFT JOIN thread ON thread_track.utid = thread.utid
        LEFT JOIN process thread_process ON thread.upid = thread_process.upid
        ORDER BY ts ASC;
        """,
        out=Csv("""
        "track","process","thread","thread_process","ts","dur","category","name"
        "[NULL]","[NULL]","t1","[NULL]",1000,0,"cat","instant_on_t1"
        "[NULL]","[NULL]","t1","[NULL]",2000,0,"cat","legacy_instant_on_t1"
        "[NULL]","[NULL]","t1","[NULL]",3000,0,"cat","legacy_mark_on_t1"
        """))

  # Legacy async events
  def test_legacy_async_event(self):
    return DiffTestBlueprint(
        trace=Path('legacy_async_event.textproto'),
        query="""
        SELECT
          track.name AS track,
          process.name AS process,
          thread.name AS thread,
          thread_process.name AS thread_process,
          slice.ts,
          slice.dur,
          slice.category,
          slice.name,
          args.key,
          args.string_value,
          args.int_value
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN process_track ON slice.track_id = process_track.id
        LEFT JOIN process ON process_track.upid = process.upid
        LEFT JOIN thread_track ON slice.track_id = thread_track.id
        LEFT JOIN thread ON thread_track.utid = thread.utid
        LEFT JOIN process thread_process ON thread.upid = thread_process.upid
        LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
        ORDER BY slice.ts, args.id;
        """,
        out=Csv("""
        "track","process","thread","thread_process","ts","dur","category","name","key","string_value","int_value"
        "name1","[NULL]","[NULL]","[NULL]",1000,7000,"cat","name1","legacy_trace_source_id","[NULL]",1234
        "name1","[NULL]","[NULL]","[NULL]",1000,7000,"cat","name1","debug.arg1","value1","[NULL]"
        "name1","[NULL]","[NULL]","[NULL]",1000,7000,"cat","name1","legacy_event.passthrough_utid","[NULL]",2
        "name1","[NULL]","[NULL]","[NULL]",1000,7000,"cat","name1","legacy_event.phase","S","[NULL]"
        "name1","[NULL]","[NULL]","[NULL]",1000,7000,"cat","name1","debug.arg2","value2","[NULL]"
        "name1","[NULL]","[NULL]","[NULL]",2000,1000,"cat","name1","legacy_trace_source_id","[NULL]",1234
        "name1","[NULL]","[NULL]","[NULL]",2000,1000,"cat","name1","legacy_event.passthrough_utid","[NULL]",1
        "name1","[NULL]","[NULL]","[NULL]",2000,1000,"cat","name1","legacy_event.phase","S","[NULL]"
        "name1","[NULL]","[NULL]","[NULL]",3000,0,"cat","name1","legacy_trace_source_id","[NULL]",1234
        "name1","[NULL]","[NULL]","[NULL]",3000,0,"cat","name1","debug.arg3","value3","[NULL]"
        "name1","[NULL]","[NULL]","[NULL]",3000,0,"cat","name1","debug.step","Step1","[NULL]"
        "name1","[NULL]","[NULL]","[NULL]",3000,0,"cat","name1","legacy_event.passthrough_utid","[NULL]",2
        "name1","[NULL]","[NULL]","[NULL]",3000,0,"cat","name1","legacy_event.phase","T","[NULL]"
        "name1","[NULL]","[NULL]","[NULL]",5000,0,"cat","name1","legacy_trace_source_id","[NULL]",1234
        "name1","[NULL]","[NULL]","[NULL]",5000,0,"cat","name1","debug.arg4","value4","[NULL]"
        "name1","[NULL]","[NULL]","[NULL]",5000,0,"cat","name1","debug.step","Step2","[NULL]"
        "name1","[NULL]","[NULL]","[NULL]",5000,0,"cat","name1","legacy_event.passthrough_utid","[NULL]",2
        "name1","[NULL]","[NULL]","[NULL]",5000,0,"cat","name1","legacy_event.phase","p","[NULL]"
        """))

  # Legacy atrace
  def test_track_event_with_atrace(self):
    return DiffTestBlueprint(
        trace=Path('track_event_with_atrace.textproto'),
        query="""
        SELECT
          track.name AS track,
          process.name AS process,
          thread.name AS thread,
          thread_process.name AS thread_process,
          slice.ts,
          slice.dur,
          slice.category,
          slice.name
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN process_track ON slice.track_id = process_track.id
        LEFT JOIN process ON process_track.upid = process.upid
        LEFT JOIN thread_track ON slice.track_id = thread_track.id
        LEFT JOIN thread ON thread_track.utid = thread.utid
        LEFT JOIN process thread_process ON thread.upid = thread_process.upid
        ORDER BY ts ASC;
        """,
        out=Csv("""
        "track","process","thread","thread_process","ts","dur","category","name"
        "[NULL]","[NULL]","t1","[NULL]",10000,1000,"cat","event1"
        "[NULL]","[NULL]","t1","[NULL]",20000,8000,"cat","event2"
        "[NULL]","[NULL]","t1","[NULL]",21000,7000,"[NULL]","atrace"
        """))

  def test_track_event_with_atrace_separate_tracks(self):
    return DiffTestBlueprint(
        trace=Path('track_event_with_atrace_separate_tracks.textproto'),
        query="""
        SELECT
          track.name AS track,
          process.name AS process,
          thread.name AS thread,
          thread_process.name AS thread_process,
          slice.ts,
          slice.dur,
          slice.category,
          slice.name
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN process_track ON slice.track_id = process_track.id
        LEFT JOIN process ON process_track.upid = process.upid
        LEFT JOIN thread_track ON slice.track_id = thread_track.id
        LEFT JOIN thread ON thread_track.utid = thread.utid
        LEFT JOIN process thread_process ON thread.upid = thread_process.upid
        ORDER BY ts ASC;
        """,
        out=Csv("""
        "track","process","thread","thread_process","ts","dur","category","name"
        "[NULL]","[NULL]","t1","[NULL]",10000,1000,"cat","event1"
        "[NULL]","[NULL]","t1","[NULL]",20000,8000,"cat","event2"
        "[NULL]","[NULL]","t1","[NULL]",21000,8000,"[NULL]","atrace"
        """))

  # Debug annotations
  def test_track_event_merged_debug_annotations_args(self):
    return DiffTestBlueprint(
        trace=Path('track_event_merged_debug_annotations.textproto'),
        query="""
        SELECT
          flat_key,
          key,
          int_value,
          string_value
        FROM args
        ORDER BY key, display_value, arg_set_id, key ASC;
        """,
        out=Csv('''
          "flat_key","key","int_value","string_value"
          "debug.debug1.key1","debug.debug1.key1",10,"[NULL]"
          "debug.debug1.key2","debug.debug1.key2[0]",20,"[NULL]"
          "debug.debug1.key2","debug.debug1.key2[1]",21,"[NULL]"
          "debug.debug1.key2","debug.debug1.key2[2]",22,"[NULL]"
          "debug.debug1.key2","debug.debug1.key2[3]",23,"[NULL]"
          "debug.debug1.key3","debug.debug1.key3",30,"[NULL]"
          "debug.debug2.key1","debug.debug2.key1",10,"[NULL]"
          "debug.debug2.key2","debug.debug2.key2[0]",20,"[NULL]"
          "debug.debug2.key2","debug.debug2.key2[1]",21,"[NULL]"
          "debug.debug2.key2","debug.debug2.key2[2]",22,"[NULL]"
          "debug.debug2.key2","debug.debug2.key2[3]",23,"[NULL]"
          "debug.debug2.key3.key31","debug.debug2.key3.key31",31,"[NULL]"
          "debug.debug2.key3.key32","debug.debug2.key3.key32",32,"[NULL]"
          "debug.debug2.key4","debug.debug2.key4",40,"[NULL]"
          "debug.debug3","debug.debug3",32,"[NULL]"
          "debug.debug4.key1","debug.debug4.key1",10,"[NULL]"
          "debug.debug4.key2","debug.debug4.key2[0]",20,"[NULL]"
          "debug.debug4.key2","debug.debug4.key2[1]",21,"[NULL]"
          "event.category","event.category","[NULL]","cat"
          "event.category","event.category","[NULL]","cat"
          "event.name","event.name","[NULL]","[NULL]"
          "event.name","event.name","[NULL]","name1"
          "legacy_event.passthrough_utid","legacy_event.passthrough_utid",2,"[NULL]"
          "legacy_trace_source_id","legacy_trace_source_id",1234,"[NULL]"
          "name","name","[NULL]","name1"
          "scope","scope","[NULL]","cat"
          "source","source","[NULL]","chrome"
          "source_scope","source_scope","[NULL]","cat"
          "trace_id_is_process_scoped","trace_id_is_process_scoped",0,"[NULL]"
          "track_compressor_idx","track_compressor_idx",0,"[NULL]"
          "upid","upid",1,"[NULL]"
        '''))

  # Counters
  def test_track_event_counters_slices(self):
    return DiffTestBlueprint(
        trace=Path('track_event_counters.textproto'),
        query="""
        SELECT
          track.name AS track,
          process.name AS process,
          thread.name AS thread,
          thread_process.name AS thread_process,
          slice.ts,
          slice.dur,
          slice.category,
          slice.name
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN process_track ON slice.track_id = process_track.id
        LEFT JOIN process ON process_track.upid = process.upid
        LEFT JOIN thread_track ON slice.track_id = thread_track.id
        LEFT JOIN thread ON thread_track.utid = thread.utid
        LEFT JOIN process thread_process ON thread.upid = thread_process.upid
        ORDER BY ts ASC;
        """,
        out=Csv("""
        "track","process","thread","thread_process","ts","dur","category","name"
        "[NULL]","[NULL]","t1","Browser",1000,100,"cat","event1_on_t1"
        "[NULL]","[NULL]","t1","Browser",2000,200,"cat","event2_on_t1"
        "[NULL]","[NULL]","t1","Browser",2000,200,"cat","event3_on_t1"
        "[NULL]","[NULL]","t1","Browser",4000,0,"cat","event4_on_t1"
        "[NULL]","[NULL]","t4","Browser",4000,100,"cat","event1_on_t3"
        "[NULL]","[NULL]","t1","Browser",4300,0,"cat","float_counter_on_t1"
        "[NULL]","[NULL]","t1","Browser",4500,0,"cat","float_counter_on_t1"
        """))

  def test_track_event_counters_counters(self):
    return DiffTestBlueprint(
        trace=Path('track_event_counters.textproto'),
        query="""
        SELECT
          counter_track.name AS counter_name,
          process.name AS process,
          thread.name AS thread,
          thread_process.name AS thread_process,
          counter_track.unit AS unit,
          counter.ts,
          counter.value
        FROM counter
        LEFT JOIN counter_track ON counter.track_id = counter_track.id
        LEFT JOIN process_counter_track ON counter.track_id = process_counter_track.id
        LEFT JOIN process ON process_counter_track.upid = process.upid
        LEFT JOIN thread_counter_track ON counter.track_id = thread_counter_track.id
        LEFT JOIN thread ON thread_counter_track.utid = thread.utid
        LEFT JOIN process thread_process ON thread.upid = thread_process.upid
        ORDER BY ts ASC;
        """,
        out=Csv("""
        "counter_name","process","thread","thread_process","unit","ts","value"
        "thread_time","[NULL]","t1","Browser","ns",1000,1000000.000000
        "thread_time","[NULL]","t1","Browser","ns",1100,1010000.000000
        "thread_time","[NULL]","t1","Browser","ns",2000,2000000.000000
        "thread_time","[NULL]","t1","Browser","ns",2000,2010000.000000
        "thread_time","[NULL]","t1","Browser","ns",2200,2020000.000000
        "thread_time","[NULL]","t1","Browser","ns",2200,2030000.000000
        "MySizeCounter","[NULL]","[NULL]","[NULL]","bytes",3000,1024.000000
        "MySizeCounter","[NULL]","[NULL]","[NULL]","bytes",3100,2048.000000
        "thread_time","[NULL]","t1","Browser","ns",4000,2040000.000000
        "MySizeCounter","[NULL]","[NULL]","[NULL]","bytes",4000,1024.000000
        "thread_time","[NULL]","t4","Browser","[NULL]",4000,10000.000000
        "thread_instruction_count","[NULL]","t4","Browser","[NULL]",4000,20.000000
        "thread_time","[NULL]","t4","Browser","[NULL]",4100,15000.000000
        "thread_instruction_count","[NULL]","t4","Browser","[NULL]",4100,25.000000
        "MyDoubleCounter","[NULL]","[NULL]","[NULL]","[NULL]",4200,3.141593
        "MyDoubleCounter","[NULL]","[NULL]","[NULL]","[NULL]",4300,0.500000
        "MySizeCounter","[NULL]","[NULL]","[NULL]","bytes",4500,4096.000000
        "MyDoubleCounter","[NULL]","[NULL]","[NULL]","[NULL]",4500,2.718280
        """))

  def test_incremental_counter_sequences(self):
    return DiffTestBlueprint(
        trace=Path('incremental_counter_sequences.textproto'),
        query="""
        SELECT
          ts,
          value
        FROM counter
        JOIN track ON counter.track_id = track.id
        WHERE
          track.name = 'MyIncrementalCounter'
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","value"
        100,100.000000
        150,50.000000
        200,110.000000
        250,55.000000
        """))

  # Clock handling
  def test_track_event_monotonic_trace_clock_slices(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          clock_snapshot {
            primary_trace_clock: 3  # BUILTIN_CLOCK_MONOTONIC
            clocks {
              clock_id: 3  # BUILTIN_CLOCK_MONOTONIC
              timestamp: 1000
            }
            clocks {
              clock_id: 6  # BUILTIN_CLOCK_BOOTTIME
              timestamp: 11000
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          timestamp_clock_id: 3  # BUILTIN_CLOCK_MONOTONIC
          track_event {
            track_uuid: 1
            categories: "cat"
            name: "name1"
            type: 3
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 12000
          timestamp_clock_id: 6  # BUILTIN_CLOCK_BOOTTIME
          track_event {
            track_uuid: 1
            categories: "cat"
            name: "name2"
            type: 3
          }
        }
        """),
        query="""
        SELECT
          track.name AS track,
          process.name AS process,
          thread.name AS thread,
          thread_process.name AS thread_process,
          slice.ts,
          slice.dur,
          slice.category,
          slice.name
        FROM slice
        LEFT JOIN track ON slice.track_id = track.id
        LEFT JOIN process_track ON slice.track_id = process_track.id
        LEFT JOIN process ON process_track.upid = process.upid
        LEFT JOIN thread_track ON slice.track_id = thread_track.id
        LEFT JOIN thread ON thread_track.utid = thread.utid
        LEFT JOIN process thread_process ON thread.upid = thread_process.upid
        ORDER BY ts ASC;
        """,
        out=Csv("""
        "track","process","thread","thread_process","ts","dur","category","name"
        "name1","[NULL]","[NULL]","[NULL]",1000,0,"cat","name1"
        "name1","[NULL]","[NULL]","[NULL]",2000,0,"cat","name2"
        """))

  # HistogramName interning
  def test_track_event_chrome_histogram_sample_args(self):
    return DiffTestBlueprint(
        trace=Path('track_event_chrome_histogram_sample.textproto'),
        query="""
        SELECT
          flat_key,
          key,
          int_value,
          string_value
        FROM args
        ORDER BY key, display_value, arg_set_id, key ASC;
        """,
        out=Csv('''
          "flat_key","key","int_value","string_value"
          "chrome_histogram_sample.name","chrome_histogram_sample.name","[NULL]","Compositing.Display.DrawToSwapUs"
          "chrome_histogram_sample.name","chrome_histogram_sample.name","[NULL]","CompositorLatency.TotalLatency"
          "chrome_histogram_sample.name","chrome_histogram_sample.name","[NULL]","Graphics.Smoothness.Checkerboarding.MainThreadAnimation"
          "chrome_histogram_sample.name","chrome_histogram_sample.name","[NULL]","Memory.GPU.PeakMemoryUsage.PageLoad"
          "chrome_histogram_sample.name_hash","chrome_histogram_sample.name_hash",10,"[NULL]"
          "chrome_histogram_sample.name_hash","chrome_histogram_sample.name_hash",20,"[NULL]"
          "chrome_histogram_sample.name_hash","chrome_histogram_sample.name_hash",30,"[NULL]"
          "chrome_histogram_sample.name_hash","chrome_histogram_sample.name_hash",40,"[NULL]"
          "chrome_histogram_sample.name_hash","chrome_histogram_sample.name_hash",50,"[NULL]"
          "chrome_histogram_sample.name_hash","chrome_histogram_sample.name_hash",60,"[NULL]"
          "chrome_histogram_sample.name_iid","chrome_histogram_sample.name_iid",1,"[NULL]"
          "chrome_histogram_sample.name_iid","chrome_histogram_sample.name_iid",2,"[NULL]"
          "chrome_histogram_sample.name_iid","chrome_histogram_sample.name_iid",3,"[NULL]"
          "chrome_histogram_sample.name_iid","chrome_histogram_sample.name_iid",4,"[NULL]"
          "chrome_histogram_sample.sample","chrome_histogram_sample.sample",100,"[NULL]"
          "chrome_histogram_sample.sample","chrome_histogram_sample.sample",200,"[NULL]"
          "chrome_histogram_sample.sample","chrome_histogram_sample.sample",300,"[NULL]"
          "chrome_histogram_sample.sample","chrome_histogram_sample.sample",400,"[NULL]"
          "chrome_histogram_sample.sample","chrome_histogram_sample.sample",500,"[NULL]"
          "chrome_histogram_sample.sample","chrome_histogram_sample.sample",600,"[NULL]"
          "event.category","event.category","[NULL]","disabled-by-default-histogram_samples"
          "event.name","event.name","[NULL]","[NULL]"
          "is_root_in_scope","is_root_in_scope",1,"[NULL]"
          "merge_key_type","merge_key_type",0,"[NULL]"
          "merge_key_value","merge_key_value","[NULL]","Default Track"
          "parent_track_uuid","parent_track_uuid",0,"[NULL]"
          "source","source","[NULL]","descriptor"
          "trace_id","trace_id",0,"[NULL]"
          "track_compressor_idx","track_compressor_idx",0,"[NULL]"
        '''))

  # Flow events importing from proto
  def test_flow_events_track_event(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_track_event.textproto'),
        query="""
        SELECT t1.name AS slice_out, t2.name AS slice_in FROM flow t
        JOIN slice t1 ON t.slice_out = t1.slice_id
        JOIN slice t2 ON t.slice_in = t2.slice_id;
        """,
        out=Csv("""
        "slice_out","slice_in"
        "FlowSlice1Start","FlowSlice1End"
        "FlowSlice1Start2Start","FlowSlice1End"
        "FlowSlice1Start2Start","FlowSlice2End"
        "FlowSlice3Begin","FlowSlice3End4Begin"
        "FlowSlice3End4Begin","FlowSlice4Step"
        "FlowSlice4Step","FlowSlice4Step2_FlowIdOnAsyncEndEvent"
        "FlowSlice4Step2_FlowIdOnAsyncEndEvent","FlowSlice4Step2_FlowIdOnEndEvent"
        "FlowSlice4Step2_FlowIdOnEndEvent","FlowSlice4End"
        """))

  def test_flow_events_proto_v2(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_proto_v2.textproto'),
        query="""
        SELECT t1.name AS slice_out, t2.name AS slice_in FROM flow t
        JOIN slice t1 ON t.slice_out = t1.slice_id
        JOIN slice t2 ON t.slice_in = t2.slice_id;
        """,
        out=Csv("""
        "slice_out","slice_in"
        "FlowBeginSlice","FlowEndSlice_1"
        "FlowBeginSlice","FlowStepSlice"
        "FlowStepSlice","FlowEndSlice_2"
        """))

  def test_flow_events_proto_v1(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_proto_v1.textproto'),
        query="""
        SELECT t1.name AS slice_out, t2.name AS slice_in FROM flow t
        JOIN slice t1 ON t.slice_out = t1.slice_id
        JOIN slice t2 ON t.slice_in = t2.slice_id;
        """,
        out=Csv("""
        "slice_out","slice_in"
        "FlowBeginSlice","FlowEndSlice_1"
        "FlowEndSlice_1","FlowStepSlice"
        "FlowStepSlice","FlowEndSlice_2"
        """))

  # Async slices starting and ending at the same time
  def test_experimental_slice_layout_depth(self):
    return DiffTestBlueprint(
        trace=Path('experimental_slice_layout_depth.py'),
        query="""
        SELECT layout_depth
        FROM experimental_slice_layout((
          SELECT group_concat(track_id, ',')
          FROM slice
        ))
        """,
        out=Csv("""
        "layout_depth"
        0
        0
        0
        """))

  # Descriptor merging regression test (bug: b/197203390)
  def test_merging_regression(self):
    return DiffTestBlueprint(
        trace=DataPath('trace_with_descriptor.pftrace'),
        query="""
        SELECT ts FROM slice ORDER BY ts LIMIT 10;
        """,
        out=Csv("""
        "ts"
        605361018360000
        605361018360000
        605361028265000
        605361028265000
        605361028361000
        605361028878000
        605361033445000
        605361033445000
        605361034257000
        605361035040000
        """))

  # Range of interest
  def test_range_of_interest(self):
    return DiffTestBlueprint(
        trace=Path('range_of_interest.textproto'),
        query="""
        SELECT ts, name
        FROM slice
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","name"
        12000,"slice3"
        13000,"slice4"
        """))

  def test_track_event_tracks_ordering(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks_ordering.textproto'),
        query="""
        SELECT
          t.name,
          p.name AS parent_name,
          EXTRACT_ARG(t.source_arg_set_id, 'child_ordering') AS ordering,
          EXTRACT_ARG(t.source_arg_set_id, 'sibling_order_rank') AS rank
        FROM track t
        LEFT JOIN track p ON t.parent_id = p.id
        ORDER BY p.name, t.name
        """,
        out=Csv("""
        "name","parent_name","ordering","rank"
        "p1","[NULL]","explicit","[NULL]"
        "p1_child_1","[NULL]","[NULL]",-10
        "p1_child_2","[NULL]","[NULL]",-2
        "parent","[NULL]","explicit","[NULL]"
        "async3","child_2","[NULL]","[NULL]"
        "async","parent","[NULL]",1
        "async2","parent","[NULL]",2
        "child_1","parent","[NULL]",-10
        "child_2","parent","[NULL]",-2
        "child_3","parent","[NULL]","[NULL]"
        """))

  def test_track_event_tracks_machine_id(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks.textproto'),
        trace_modifier=TraceInjector(['track_descriptor', 'track_event'],
                                     {'machine_id': 1001}),
        query="""
        WITH track_with_name AS (
          SELECT
            COALESCE(
              t1.name,
              'thread=' || thread.name,
              'process=' || process.name,
              'tid=' || thread.tid,
              'pid=' || process.pid
            ) AS full_name,
            *
          FROM track t1
          LEFT JOIN thread_track t2 USING (id)
          LEFT JOIN thread USING (utid)
          LEFT JOIN process_track t3 USING (id)
          LEFT JOIN process ON t3.upid = process.id
          WHERE t1.machine_id IS NOT NULL
          ORDER BY id
        )
        SELECT
        t.full_name AS name,
        EXTRACT_ARG(t.source_arg_set_id, 'has_first_packet_on_sequence')
               AS has_first_packet_on_sequence
        FROM track_with_name t
        ORDER BY 1, 2;
        """,
        out=Csv("""
        "name","has_first_packet_on_sequence"
        "Default Track","[NULL]"
        "async",1
        "async2",1
        "async3",1
        "event_and_track_async3",1
        "process=p1",1
        "process=p2","[NULL]"
        "process=p2","[NULL]"
        "thread=t1",1
        "thread=t2",1
        "thread=t3",1
        "thread=t4","[NULL]"
        """))

  # Tests thread_counter_track.machine_id is not null.
  def test_track_event_counters_counters_machine_id(self):
    return DiffTestBlueprint(
        trace=Path('track_event_counters.textproto'),
        trace_modifier=TraceInjector(
            ['track_descriptor', 'track_event', 'trace_packet_defaults'],
            {'machine_id': 1001}),
        query="""
        SELECT name, machine_id
        FROM thread_counter_track
        WHERE machine_id IS NOT NULL
        """,
        out=Csv("""
        "name","machine_id"
        "thread_time",1
        "thread_time",1
        "thread_instruction_count",1
        """))

  def test_track_event_name_resolution(self):
    return DiffTestBlueprint(
        trace=Path('track_event_name_resolution.textproto'),
        query="""
        SELECT name
        FROM track
        ORDER BY name;
        """,
        out=Csv("""
        "name"
        "Before Event"
        "Event Name"
        "Second Name"
        """))

  def test_track_event_callstacks(self):
    return DiffTestBlueprint(
        trace=Path('track_event_callstacks.textproto'),
        query="""
        WITH inline_slices AS (
          SELECT
            callsite_id
          FROM slice
          JOIN __intrinsic_track_event_callstacks USING (slice_id)
          WHERE name GLOB 'Inline Slice *'
        ),
        inline_stats AS (
          SELECT
            COUNT(DISTINCT callsite_id) AS inline_unique_callstacks
          FROM inline_slices
          WHERE callsite_id IS NOT NULL
        )
        SELECT
          slice.name,
          COALESCE(spf.name, '[NULL]') AS leaf_frame,
          COALESCE(sps.source_file, '[NULL]') AS source_file,
          COALESCE(sps.line_number, 0) AS line_number,
          inline_stats.inline_unique_callstacks
        FROM slice
        CROSS JOIN inline_stats
        LEFT JOIN __intrinsic_track_event_callstacks tec
          ON tec.slice_id = slice.id AND tec.callsite_id IS NOT NULL
        LEFT JOIN stack_profile_callsite spc
          ON spc.id = tec.callsite_id
        LEFT JOIN stack_profile_frame spf
          ON spf.id = spc.frame_id
        LEFT JOIN stack_profile_symbol sps
          ON sps.symbol_set_id = spf.symbol_set_id AND sps.id = spf.symbol_set_id
        WHERE slice.name IN ('Inline Slice 1', 'Inline Slice 2', 'Interned Slice')
        ORDER BY slice.name;
        """,
        out=Csv("""
        "name","leaf_frame","source_file","line_number","inline_unique_callstacks"
        "Inline Slice 1","InlineLeaf","leaf.cc",42,1
        "Inline Slice 2","InlineLeaf","leaf.cc",42,1
        "Interned Slice","FuncB","[NULL]",0,1
        """))

  def test_track_event_name_resolution_extended(self):
    return DiffTestBlueprint(
        trace=Path('track_event_name_resolution_extended.textproto'),
        query="""
        SELECT t.name, t.parent_id is null as is_root
        FROM track t
        ORDER BY t.name;
        """,
        out=Csv("""
        "name","is_root"
        "After Event",1
        "Child Event",0
        "Event Name",1
        "Parent",1
        "Second Name",1
        """))

  def test_track_event_name_resolution_null_override(self):
    return DiffTestBlueprint(
        trace=Path('track_event_name_resolution_null_override.textproto'),
        query="""
        SELECT name
        FROM track
        WHERE name IS NOT NULL;
        """,
        out=Csv("""
        "name"
        "First Name"
        """))
