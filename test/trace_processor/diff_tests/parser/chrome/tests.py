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
              thread_type: THREAD_POOL_FG_WORKER
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
        1,"foo.cc:123","log message",4
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
              thread_type: THREAD_POOL_FG_WORKER
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
        1,"foo.cc:123","log message",5
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
              thread_type: THREAD_POOL_FG_WORKER
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
              process_type: PROCESS_SERVICE_TRACING
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
        packet {
          timestamp: 0
          incremental_state_cleared: true
          track_descriptor {
            uuid: 12345
            parent_uuid: 5678
            thread {
              pid: 1234
              tid: 12345
              thread_name: "thread2"
            }
          }
          trusted_uid: 0
          trusted_packet_sequence_id: 2
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
              name: "category2"
            }
          }
          trusted_uid: 0
          trusted_packet_sequence_id: 2
        }
        """),
        query="""
        SELECT utid, tid, thread.name, upid, pid, is_main_thread
        FROM thread LEFT JOIN process USING (upid);
        """,
        # A synthetic TID should be used for the sandboxed tid, but not for the
        # non-sandboxed one.
        out=Csv("""
        "utid","tid","name","upid","pid","is_main_thread"
        0,0,"swapper",0,0,1
        1,1234,"[NULL]",1,1234,1
        2,5299989643265,"thread1",1,1234,0
        3,12345,"thread2",1,1234,0
        """))

  def _get_chrome_thread_event_trace_textproto(self, extra_textproto):
    return TextProto(r"""
        packet {
          timestamp: 0
          incremental_state_cleared: true
          track_descriptor {
            uuid: 1234
            parent_uuid: 5678
            thread {
              pid: 1234
              tid: 12345
              thread_name: "thread1"
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
              process_type: PROCESS_SERVICE_TRACING
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
        """ + extra_textproto)

  def test_synthetic_tids_workaround_no_metadata(self):
    return DiffTestBlueprint(
        trace=self._get_chrome_thread_event_trace_textproto(""),
        query="""
        SELECT utid, tid, thread.name, upid, pid, is_main_thread
        FROM thread LEFT JOIN process USING (upid);
        """,
        # No metadata -- tids are preserved.
        out=Csv("""
        "utid","tid","name","upid","pid","is_main_thread"
        0,0,"swapper",0,0,1
        1,1234,"[NULL]",1,1234,1
        2,12345,"thread1",1,1234,0
        """))

  def test_synthetic_tids_workaround_new_linux_chrome(self):
    return DiffTestBlueprint(
        trace=self._get_chrome_thread_event_trace_textproto(r"""
        packet {
          timestamp: 0
          sequence_flags: 2
          chrome_events {
            metadata {
              name: "product-version"
              string_value: "Chrome/140.0.7326.0"
            }
            metadata {
              name: "os-name"
              string_value: "Linux"  
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
        # New Chrome sets the is_sandboxed_tid field, so we respect tids for
        # threads that don't set it.
        out=Csv("""
        "utid","tid","name","upid","pid","is_main_thread"
        0,0,"swapper",0,0,1
        1,1234,"[NULL]",1,1234,1
        2,12345,"thread1",1,1234,0
        """))

  def test_synthetic_tids_workaround_newer_linux_chrome(self):
    return DiffTestBlueprint(
        trace=self._get_chrome_thread_event_trace_textproto(r"""
        packet {
          timestamp: 0
          sequence_flags: 2
          chrome_events {
            metadata {
              name: "product-version"
              string_value: "141.0.8888.0-64"  
            }
            metadata {
              name: "os-name"
              string_value: "Linux"  
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
        # New Chrome sets the is_sandboxed_tid field, so we respect tids for
        # threads that don't set it.
        out=Csv("""
        "utid","tid","name","upid","pid","is_main_thread"
        0,0,"swapper",0,0,1
        1,1234,"[NULL]",1,1234,1
        2,12345,"thread1",1,1234,0
        """))

  def test_synthetic_tids_workaround_android_chrome(self):
    return DiffTestBlueprint(
        trace=self._get_chrome_thread_event_trace_textproto(r"""
        packet {
          timestamp: 0
          sequence_flags: 2
          chrome_events {
            metadata {
              name: "product-version"
              string_value: "Chrome/139.0.7000.0"  
            }
            metadata {
              name: "os-name"
              string_value: "Android"  
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
        # Chrome on non-Linux platforms doesn't use sandboxed tids.
        out=Csv("""
        "utid","tid","name","upid","pid","is_main_thread"
        0,0,"swapper",0,0,1
        1,1234,"[NULL]",1,1234,1
        2,12345,"thread1",1,1234,0
        """))

  def test_synthetic_tids_workaround_old_linux_chrome(self):
    return DiffTestBlueprint(
        trace=self._get_chrome_thread_event_trace_textproto(r"""
        packet {
          timestamp: 0
          sequence_flags: 2
          chrome_events {
            metadata {
              name: "product-version"
              string_value: "139.0.7000.0"  
            }
            metadata {
              name: "os-name"
              string_value: "Linux"  
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
        # All non-pid tids from older Linux Chrome versions are synthetic.
        out=Csv("""
        "utid","tid","name","upid","pid","is_main_thread"
        0,0,"swapper",0,0,1
        1,1234,"[NULL]",1,1234,1
        2,5299989655609,"thread1",1,1234,0
        """))

  def test_synthetic_tids_workaround_old_linux_chrome_alternative_product_version(
      self):
    return DiffTestBlueprint(
        trace=self._get_chrome_thread_event_trace_textproto(r"""
        packet {
          timestamp: 0
          sequence_flags: 2
          chrome_events {
            metadata {
              name: "product-version"
              string_value: "Chrome/139.0.7000.0-64"  
            }
            metadata {
              name: "os-name"
              string_value: "Linux"  
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
        # All non-pid tids from older Linux Chrome versions are synthetic.
        out=Csv("""
        "utid","tid","name","upid","pid","is_main_thread"
        0,0,"swapper",0,0,1
        1,1234,"[NULL]",1,1234,1
        2,5299989655609,"thread1",1,1234,0
        """))

  def _get_streaming_profile_textproto(self):
    return r"""
    packet {
      timestamp: 0
      incremental_state_cleared: true
      track_descriptor {
        uuid: 42
        parent_uuid: 5678
        thread {
          pid: 1234
          tid: 42
          thread_name: "thread2"
          reference_timestamp_us: 1000
        }
      }
      trusted_uid: 0
      trusted_packet_sequence_id: 2
    }
    packet {
      timestamp: 0
      sequence_flags: 2
      interned_data {
        mappings {
          iid: 1
          build_id: 1
        }
        build_ids {
          iid: 1
          str: "3BBCFBD372448A727265C3E7C4D954F91"
        }
        frames {
          iid: 1
          rel_pc: 0x42
          mapping_id: 1
        }
        frames {
          iid: 2
          rel_pc: 0x4242
          mapping_id: 1
        }
        callstacks {
          iid: 1
          frame_ids: 1
        }
        callstacks {
          iid: 42
          frame_ids: 2
        }
      }
      streaming_profile_packet {
        callstack_iid: 42
        timestamp_delta_us: 10
        callstack_iid: 1
        timestamp_delta_us: 15
        process_priority: 20
      }
      trusted_uid: 0
      trusted_packet_sequence_id: 2
    }
    packet {
      timestamp: 0
      sequence_flags: 2
      streaming_profile_packet {
        callstack_iid: 42
        timestamp_delta_us: 42
        process_priority: 30
      }
      trusted_uid: 0
      trusted_packet_sequence_id: 2
    }
    """

  def test_synthetic_tids_workaround_for_profiling_new_linux(self):
    return DiffTestBlueprint(
        trace=self._get_chrome_thread_event_trace_textproto(r"""
        packet {
          timestamp: 0
          sequence_flags: 2
          chrome_events {
            metadata {
              name: "product-version"
              string_value: "Chrome/140.0.7326.0"
            }
            metadata {
              name: "os-name"
              string_value: "Linux"  
            }
          }
          trusted_uid: 0
          trusted_packet_sequence_id: 1
        }
        """ + self._get_streaming_profile_textproto()),
        query="""
        SELECT utid, tid, thread.name, upid, pid, is_main_thread
        FROM thread LEFT JOIN process USING (upid);
        """,
        # New Chrome sets the is_sandboxed_tid field, so we respect tids for
        # threads that don't set it.
        out=Csv("""
        "utid","tid","name","upid","pid","is_main_thread"
        0,0,"swapper",0,0,1
        1,1234,"[NULL]",1,1234,1
        2,12345,"thread1",1,1234,0
        3,42,"thread2",1,1234,0
        """))

  def test_synthetic_tids_workaround_for_profiling_old_linux(self):
    return DiffTestBlueprint(
        trace=self._get_chrome_thread_event_trace_textproto(r"""
        packet {
          timestamp: 0
          sequence_flags: 2
          chrome_events {
            metadata {
              name: "product-version"
              string_value: "Chrome/139.0.7000.0"
            }
            metadata {
              name: "os-name"
              string_value: "Linux"  
            }
          }
          trusted_uid: 0
          trusted_packet_sequence_id: 1
        }
        """ + self._get_streaming_profile_textproto()),
        query="""
        SELECT utid, tid, thread.name, upid, pid, is_main_thread
        FROM thread LEFT JOIN process USING (upid);
        """,
        # All non-pid tids from older Linux Chrome versions are synthetic.
        out=Csv("""
        "utid","tid","name","upid","pid","is_main_thread"
        0,0,"swapper",0,0,1
        1,1234,"[NULL]",1,1234,1
        2,5299989655609,"thread1",1,1234,0
        3,5299989643306,"thread2",1,1234,0
        """))
