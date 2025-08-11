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


class ChromeProcesses(TestSuite):

  def test_chrome_processes(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_scroll_without_vsync.pftrace'),
        query="""
        SELECT RUN_METRIC('chrome/chrome_processes.sql');
        SELECT pid, name, process_type FROM chrome_process;
        """,
        out=Csv("""
        "pid","name","process_type"
        18250,"Renderer","Renderer"
        17547,"Browser","Browser"
        18277,"GPU Process","Gpu"
        17578,"Browser","Browser"
        """))

  def test_chrome_processes_android_systrace(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_android_systrace.pftrace'),
        query="""
        SELECT RUN_METRIC('chrome/chrome_processes.sql');
        SELECT pid, name, process_type
        FROM chrome_process
        ORDER BY pid;
        """,
        out=Path('chrome_processes_android_systrace.out'))

  def test_chrome_threads(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_scroll_without_vsync.pftrace'),
        query="""
        SELECT RUN_METRIC('chrome/chrome_processes.sql');
        SELECT tid, name, is_main_thread, canonical_name
        FROM chrome_thread
        ORDER BY tid, name;
        """,
        out=Path('chrome_threads.out'))

  def test_chrome_threads_skip_swapper_tid_override(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 543825535000
          sequence_flags: 1
          track_descriptor {
            uuid: 12845869133155057863
            process {
              pid: 17547
              process_name: "Browser"
            }
            [perfetto.protos.ChromeTrackDescriptor.chrome_process] {
              process_type: PROCESS_BROWSER
            }
          }
          trusted_packet_sequence_id: 5
        }
        # Need to parse at least one ThreadDescriptor packet
        # to have valid pid and tid in the sequence.
        packet {
          timestamp: 545825535000
          sequence_flags: 1
          track_descriptor {
            uuid: 12845869133155043208
            parent_uuid: 12845869133155057863
            thread {
              pid: 17547
              tid: 18255
              thread_name: "SomeThread"
            }
          }
          trusted_packet_sequence_id: 5
        }
        packet {
          timestamp: 546825535000
          track_event {
            track_uuid: 0
            legacy_event {
              phase: 77
              tid_override: 0 # Swapper tid should be ignored
            }
          }
          trusted_packet_sequence_id: 5
        }
        """),
        query="""
        SELECT RUN_METRIC('chrome/chrome_processes.sql');
        SELECT tid, name, is_main_thread, canonical_name
        FROM chrome_thread
        ORDER BY tid, name;
        """,
        out=Csv("""
        "tid","name","is_main_thread","canonical_name"
        17547,"[NULL]",1,"Unknown"
        18255,"SomeThread",0,"SomeThread"
        """))

  def test_chrome_threads_android_systrace(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_android_systrace.pftrace'),
        query="""
        SELECT RUN_METRIC('chrome/chrome_processes.sql');
        SELECT tid, name, is_main_thread, canonical_name
        FROM chrome_thread
        ORDER BY tid, name;
        """,
        out=Path('chrome_threads_android_systrace.out'))

  def test_chrome_processes_type(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_scroll_without_vsync.pftrace'),
        query="""
        SELECT pid, name, string_value AS chrome_process_type
        FROM
          process
        JOIN
          (SELECT * FROM args WHERE key = "chrome.process_type") chrome_process_args
          ON
            process.arg_set_id = chrome_process_args.arg_set_id
        ORDER BY pid;
        """,
        out=Csv("""
        "pid","name","chrome_process_type"
        17547,"Browser","Browser"
        17578,"Browser","Browser"
        18250,"Renderer","Renderer"
        18277,"GPU Process","Gpu"
        """))

  def test_chrome_processes_type_android_systrace(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_android_systrace.pftrace'),
        query="""
        SELECT pid, name, string_value AS chrome_process_type
        FROM
          process
        JOIN
          (SELECT * FROM args WHERE key = "chrome.process_type") chrome_process_args
          ON
            process.arg_set_id = chrome_process_args.arg_set_id
        ORDER BY pid;
        """,
        out=Path('chrome_processes_type_android_systrace.out'))

  def test_track_with_chrome_process(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          timestamp: 0
          track_descriptor {
            uuid: 10
            process {
              pid: 5
              process_name: "p5"
            }
            # Empty Chrome process. This is similar to a process descriptor emitted by
            # Chrome for a process with an unknown Chrome process_type. This process
            # should still receive a "chrome_process_type" arg in the args table, but
            # with a NULL value.
            [perfetto.protos.ChromeTrackDescriptor.chrome_process] {}
          }
        }
        """),
        query="""
        SELECT pid, name, string_value AS chrome_process_type
        FROM
          process
        JOIN
          (SELECT * FROM args WHERE key = "chrome.process_type") chrome_process_args
          ON
            process.arg_set_id = chrome_process_args.arg_set_id
        ORDER BY pid;
        """,
        out=Csv("""
        "pid","name","chrome_process_type"
        5,"p5","[NULL]"
        """))

  # Missing processes.
  def test_chrome_missing_processes_default_trace(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_scroll_without_vsync.pftrace'),
        query="""
        SELECT pid, reliable_from
        FROM
          experimental_missing_chrome_processes
        JOIN
          process
          USING(upid)
        ORDER BY upid;
        """,
        out=Csv("""
        "pid","reliable_from"
        """))

  def test_chrome_missing_processes(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1
          incremental_state_cleared: true
          trusted_packet_sequence_id: 1
          track_event {
            type: TYPE_INSTANT
            name: "ActiveProcesses"
            chrome_active_processes {
              pid: 10
              pid: 100
              pid: 1000
            }
          }
        }
        packet {
          timestamp: 1
          trusted_packet_sequence_id: 2
          track_descriptor {
            uuid: 1
            process {
              pid: 10
            }
            parent_uuid: 0
          }
        }
        packet {
          timestamp: 1000000000
          trusted_packet_sequence_id: 3
          track_descriptor {
            uuid: 2
            process {
              pid: 100
            }
            parent_uuid: 0
          }
        }
        """),
        query="""
        SELECT pid, reliable_from
        FROM
          experimental_missing_chrome_processes
        JOIN
          process
          USING(upid)
        ORDER BY upid;
        """,
        out=Csv("""
        "pid","reliable_from"
        100,1000000000
        1000,"[NULL]"
        """))

  def test_chrome_missing_processes_args(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1
          incremental_state_cleared: true
          trusted_packet_sequence_id: 1
          track_event {
            type: TYPE_INSTANT
            name: "ActiveProcesses"
            chrome_active_processes {
              pid: 10
              pid: 100
              pid: 1000
            }
          }
        }
        packet {
          timestamp: 1
          trusted_packet_sequence_id: 2
          track_descriptor {
            uuid: 1
            process {
              pid: 10
            }
            parent_uuid: 0
          }
        }
        packet {
          timestamp: 1000000000
          trusted_packet_sequence_id: 3
          track_descriptor {
            uuid: 2
            process {
              pid: 100
            }
            parent_uuid: 0
          }
        }
        """),
        query="""
        SELECT slice.name, key, int_value
        FROM
          slice
        JOIN
          args
          USING(arg_set_id)
        ORDER BY arg_set_id, key;
        """,
        out=Csv("""
        "name","key","int_value"
        "ActiveProcesses","chrome_active_processes.pid[0]",10
        "ActiveProcesses","chrome_active_processes.pid[1]",100
        "ActiveProcesses","chrome_active_processes.pid[2]",1000
        """))

  def test_chrome_missing_processes_2(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1
          incremental_state_cleared: true
          trusted_packet_sequence_id: 1
          track_event {
            type: TYPE_INSTANT
            name: "ActiveProcesses"
            [perfetto.protos.ChromeTrackEvent.active_processes]: {
              pid: 10
              pid: 100
              pid: 1000
            }
          }
        }
        packet {
          timestamp: 1
          trusted_packet_sequence_id: 2
          track_descriptor {
            uuid: 1
            process {
              pid: 10
            }
            parent_uuid: 0
          }
        }
        packet {
          timestamp: 1000000000
          trusted_packet_sequence_id: 3
          track_descriptor {
            uuid: 2
            process {
              pid: 100
            }
            parent_uuid: 0
          }
        }
        """),
        query="""
        SELECT pid, reliable_from
        FROM
          experimental_missing_chrome_processes
        JOIN
          process
          USING(upid)
        ORDER BY upid;
        """,
        out=Csv("""
        "pid","reliable_from"
        100,1000000000
        1000,"[NULL]"
        """))

  def test_chrome_missing_processes_extension_args(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1
          incremental_state_cleared: true
          trusted_packet_sequence_id: 1
          track_event {
            type: TYPE_INSTANT
            name: "ActiveProcesses"
            [perfetto.protos.ChromeTrackEvent.active_processes]: {
              pid: 10
              pid: 100
              pid: 1000
            }
          }
        }
        packet {
          timestamp: 1
          trusted_packet_sequence_id: 2
          track_descriptor {
            uuid: 1
            process {
              pid: 10
            }
            parent_uuid: 0
          }
        }
        packet {
          timestamp: 1000000000
          trusted_packet_sequence_id: 3
          track_descriptor {
            uuid: 2
            process {
              pid: 100
            }
            parent_uuid: 0
          }
        }
        """),
        query="""
        SELECT slice.name, key, int_value
        FROM
          slice
        JOIN
          args
          USING(arg_set_id)
        ORDER BY arg_set_id, key;
        """,
        out=Csv("""
        "name","key","int_value"
        "ActiveProcesses","active_processes.pid[0]",10
        "ActiveProcesses","active_processes.pid[1]",100
        "ActiveProcesses","active_processes.pid[2]",1000
        """))
