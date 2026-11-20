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
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ChromeParser(TestSuite):
  # Log messages.
  def test_chrome_log_message(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 0
          incremental_state_cleared: true
          trusted_packet_sequence_id: 1
          track_descriptor {
            uuid: 12345
            thread {
              pid: 123
              tid: 345
            }
            parent_uuid: 0
            chrome_thread {
              thread_type: 4
            }
          }
        }

        packet {
          trusted_packet_sequence_id: 1
          timestamp: 10
          track_event {
            track_uuid: 12345
            categories: "cat1"
            type: TYPE_INSTANT
            name: "slice1"
            log_message {
                body_iid: 1
                source_location_iid: 3
            }
          }
          interned_data {
            log_message_body {
                iid: 1
                body: "log message"
            }
            source_locations {
                iid: 3
                function_name: "func"
                file_name: "foo.cc"
                line_number: 123
            }
          }
        }
        """),
        query="""
        SELECT utid, tag, msg, prio FROM android_logs;
        """,
        # If the log_message_body doesn't have any priority, a default 4 (i.e.
        # INFO) is assumed (otherwise the UI will not show the message).
        out=Csv("""
        "utid","tag","msg","prio"
        2,"foo.cc:123","log message",4
        """))

  def test_chrome_log_message_priority(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 0
          incremental_state_cleared: true
          trusted_packet_sequence_id: 1
          track_descriptor {
            uuid: 12345
            thread {
              pid: 123
              tid: 345
            }
            parent_uuid: 0
            chrome_thread {
              thread_type: 4
            }
          }
        }

        packet {
          trusted_packet_sequence_id: 1
          timestamp: 10
          track_event {
            track_uuid: 12345
            categories: "cat1"
            type: TYPE_INSTANT
            name: "slice1"
            log_message {
                body_iid: 1
                source_location_iid: 3
                prio: PRIO_WARN
            }
          }
          interned_data {
            log_message_body {
                iid: 1
                body: "log message"
            }
            source_locations {
                iid: 3
                function_name: "func"
                file_name: "foo.cc"
                line_number: 123
            }
          }
        }
        """),
        query="""
        SELECT utid, tag, msg, prio FROM android_logs;
        """,
        out=Csv("""
        "utid","tag","msg","prio"
        2,"foo.cc:123","log message",5
        """))

  def test_chrome_log_message_args(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 0
          incremental_state_cleared: true
          trusted_packet_sequence_id: 1
          track_descriptor {
            uuid: 12345
            thread {
              pid: 123
              tid: 345
            }
            parent_uuid: 0
            chrome_thread {
              thread_type: 4
            }
          }
        }

        packet {
          trusted_packet_sequence_id: 1
          timestamp: 10
          track_event {
            track_uuid: 12345
            categories: "cat1"
            type: TYPE_INSTANT
            name: "slice1"
            log_message {
                body_iid: 1
                source_location_iid: 3
            }
          }
          interned_data {
            log_message_body {
                iid: 1
                body: "log message"
            }
            source_locations {
                iid: 3
                function_name: "func"
                file_name: "foo.cc"
                line_number: 123
            }
          }
        }
        """),
        query=Path('chrome_log_message_args_test.sql'),
        out=Csv("""
        "log_message","function_name","file_name","line_number"
        "log message","func","foo.cc",123
        """))

  def test_chrome_log_message_args_to_json(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 0
          incremental_state_cleared: true
          trusted_packet_sequence_id: 1
          track_descriptor {
            uuid: 12345
            thread {
              pid: 123
              tid: 345
            }
            parent_uuid: 0
            chrome_thread {
              thread_type: 4
            }
          }
        }

        packet {
          trusted_packet_sequence_id: 1
          timestamp: 10
          track_event {
            track_uuid: 12345
            categories: "cat1"
            type: TYPE_INSTANT
            name: "slice1"
            log_message {
                body_iid: 1
                source_location_iid: 3
            }
          }
          interned_data {
            log_message_body {
                iid: 1
                body: "log message"
            }
            source_locations {
                iid: 3
                function_name: "func"
                file_name: "foo.cc"
                line_number: 123
            }
          }
        }
        """),
        query="""
        SELECT
          name,
          __intrinsic_arg_set_to_json(arg_set_id) AS args_json
        FROM slice;
        """,
        out=Csv("""
        "name","args_json"
        "slice1","{\"track_event\":{\"log_message\":{\"message\":\"log message\",\"file_name\":\"foo.cc\",\"function_name\":\"func\",\"line_number\":123}}}"
        """))

  def test_chrome_thread_is_sandboxed_tid(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 0
          incremental_state_cleared: true
          track_descriptor {
            uuid: 1234
            parent_uuid: 5678
            thread {
              pid: 1234
              tid: 1
              thread_name: "thread1"
            }
            chrome_thread {
              is_sandboxed_tid: true
            }
          }
          trusted_uid: 0
          trusted_packet_sequence_id: 1
        }
        packet {
          timestamp: 0
          track_descriptor {
            uuid: 5678
            process {
              pid: 1234
              process_name: "process1"
            }
            chrome_process {
              process_type: 10
            }
          }
          trusted_uid: 0
          trusted_packet_sequence_id: 1
        }
        packet {
          timestamp: 0
          sequence_flags: 2
          track_event {
            type: TYPE_SLICE_BEGIN
            extra_counter_values: 75275
            category_iids: 1
          }
          interned_data {
            event_categories {
              iid: 1
              name: "category1"
            }
          }
          trusted_uid: 0
          trusted_packet_sequence_id: 1
        }
        """),
        query="""
        SELECT utid, tid, thread.name, upid, pid, is_main_thread
        FROM thread LEFT JOIN process USING (upid);
        """,
        # Sandboxed TIDs should be preserved without synthetic (pid << 32) | tid
        # tagging; ProcessTracker disambiguates threads by (upid, tid).
        out=Csv("""
        "utid","tid","name","upid","pid","is_main_thread"
        0,0,"swapper",0,0,1
        1,1234,"[NULL]",1,1234,1
        2,1,"thread1",1,1234,0
        """))

  def test_chrome_sandboxed_tid_shared_with_stack_sample(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 0
          incremental_state_cleared: true
          track_descriptor {
            uuid: 100
            process {
              pid: 1234
              process_name: "process1"
            }
          }
          trusted_packet_sequence_id: 1
        }
        packet {
          timestamp: 0
          track_descriptor {
            uuid: 101
            parent_uuid: 100
            thread {
              pid: 1234
              tid: 1
              thread_name: "thread1"
            }
            chrome_thread {
              is_sandboxed_tid: true
            }
          }
          trusted_packet_sequence_id: 1
        }
        packet {
          timestamp: 0
          incremental_state_cleared: true
          track_descriptor {
            uuid: 200
            process {
              pid: 5678
              process_name: "process2"
            }
          }
          trusted_packet_sequence_id: 2
        }
        packet {
          timestamp: 0
          track_descriptor {
            uuid: 201
            parent_uuid: 200
            thread {
              pid: 5678
              tid: 1
              thread_name: "thread2"
            }
            chrome_thread {
              is_sandboxed_tid: true
            }
          }
          trusted_packet_sequence_id: 2
        }
        packet {
          timestamp: 500
          incremental_state_cleared: true
          trusted_packet_sequence_id: 3
          trace_packet_defaults {
            stack_sample_defaults {
              source: "org.chromium.native_heap_profiler"
              primary_descriptor {
                name: "allocations"
                unit: UNIT_BYTES
              }
            }
          }
        }
        packet {
          timestamp: 1000
          trusted_packet_sequence_id: 3
          stack_sample {
            task_context {
              pid: 1234
              tid: 1
            }
            callstack {
              frames {
                function_name: "alloc1"
              }
            }
            primary_weight: 64
          }
        }
        packet {
          timestamp: 2000
          trusted_packet_sequence_id: 3
          stack_sample {
            task_context {
              pid: 5678
              tid: 1
            }
            callstack {
              frames {
                function_name: "alloc2"
              }
            }
            primary_weight: 128
          }
        }
        """),
        query="""
        SELECT
          ss.ts,
          p.pid,
          p.name AS process_name,
          t.tid,
          t.name AS thread_name,
          (SELECT count(*) FROM thread WHERE tid = 1) AS tid1_thread_count
        FROM stack_sample ss
        JOIN stack_sample_task_context tc ON tc.id = ss.task_context_id
        JOIN thread t ON t.utid = tc.utid
        JOIN process p ON p.upid = t.upid
        ORDER BY ss.ts;
        """,
        out=Csv("""
        "ts","pid","process_name","tid","thread_name","tid1_thread_count"
        1000,1234,"process1",1,"thread1",2
        2000,5678,"process2",1,"thread2",2
        """))
