#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, SimpleperfProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class SimpleperfProtoParser(TestSuite):
  """Test simpleperf_proto format parsing."""

  def test_empty_file(self):
    """Test parsing file with just magic header and no records."""
    return DiffTestBlueprint(
        trace=SimpleperfProto(records=[]),
        query="""
        SELECT COUNT(*) as count
        FROM trace_bounds
        """,
        out=Csv('''
        "count"
        1
        '''))

  def test_thread_record(self):
    """Test parsing Thread record."""
    return DiffTestBlueprint(
        trace=SimpleperfProto(records=[
            """
            sample {
              time: 1000000000
              thread_id: 1234
              event_count: 1
            }
            """, """
            thread {
              thread_id: 1234
              process_id: 5678
              thread_name: "MyThread"
            }
            """
        ]),
        query="""
        SELECT tid, upid, name
        FROM thread
        WHERE tid = 1234
        """,
        out=Csv('''
        "tid","upid","name"
        1234,1,"MyThread"
        '''))

  def test_file_record(self):
    """Test parsing File record creates mapping."""
    return DiffTestBlueprint(
        trace=SimpleperfProto(records=[
            """
            file {
              id: 1
              path: "/system/lib64/libc.so"
            }
            """
        ]),
        query="""
        SELECT name
        FROM stack_profile_mapping
        WHERE name = '/system/lib64/libc.so'
        """,
        out=Csv('''
        "name"
        "/system/lib64/libc.so"
        '''))

  def test_sample_with_callchain(self):
    """Test parsing Sample record with callchain."""
    return DiffTestBlueprint(
        trace=SimpleperfProto(records=[
            """
            file {
              id: 1
              path: "/system/lib64/libc.so"
              symbol: "malloc"
              symbol: "free"
              symbol: "memcpy"
            }
            """, """
            sample {
              time: 1000000000
              thread_id: 1234
              event_count: 100
              callchain {
                vaddr_in_file: 0x1000
                file_id: 1
                symbol_id: 0
              }
              callchain {
                vaddr_in_file: 0x2000
                file_id: 1
                symbol_id: 1
              }
            }
            """
        ]),
        query="""
        SELECT
          s.ts,
          s.utid,
          spf1.name AS sample_frame,
          spf2.name AS parent_frame
        FROM cpu_profile_stack_sample s
        JOIN stack_profile_callsite spc1 ON s.callsite_id = spc1.id
        JOIN stack_profile_frame spf1 ON spc1.frame_id = spf1.id
        LEFT JOIN stack_profile_callsite spc2 ON spc1.parent_id = spc2.id
        LEFT JOIN stack_profile_frame spf2 ON spc2.frame_id = spf2.id
        """,
        out=Csv('''
        "ts","utid","sample_frame","parent_frame"
        1000000000,1,"malloc","free"
        '''))

  def test_multiple_samples_same_thread(self):
    """Test multiple samples from the same thread."""
    return DiffTestBlueprint(
        trace=SimpleperfProto(records=[
            """
            file {
              id: 1
              path: "/system/lib64/libc.so"
              symbol: "malloc"
            }
            """, """
            sample {
              time: 1000000000
              thread_id: 1234
              event_count: 100
              callchain {
                vaddr_in_file: 0x1000
                file_id: 1
                symbol_id: 0
              }
            }
            """, """
            sample {
              time: 2000000000
              thread_id: 1234
              event_count: 100
              callchain {
                vaddr_in_file: 0x1000
                file_id: 1
                symbol_id: 0
              }
            }
            """
        ]),
        query="""
        SELECT COUNT(*) AS sample_count, COUNT(DISTINCT utid) AS unique_threads
        FROM cpu_profile_stack_sample
        """,
        out=Csv('''
        "sample_count","unique_threads"
        2,1
        '''))

  def test_cpu_profiling_basic_aggregation(self):
    """Test basic aggregation of CPU profiling samples."""
    return DiffTestBlueprint(
        trace=SimpleperfProto(records=[
            """
            file {
              id: 1
              path: "/system/lib64/libc.so"
              symbol: "main"
              symbol: "foo"
              symbol: "bar"
            }
            """, """
            sample {
              time: 1000000000
              thread_id: 1234
              callchain {
                vaddr_in_file: 0x3000
                file_id: 1
                symbol_id: 2
              }
              callchain {
                vaddr_in_file: 0x2000
                file_id: 1
                symbol_id: 1
              }
              callchain {
                vaddr_in_file: 0x1000
                file_id: 1
                symbol_id: 0
              }
            }
            """, """
            sample {
              time: 2000000000
              thread_id: 1234
              callchain {
                vaddr_in_file: 0x2000
                file_id: 1
                symbol_id: 1
              }
              callchain {
                vaddr_in_file: 0x1000
                file_id: 1
                symbol_id: 0
              }
            }
            """
        ]),
        query="""
        SELECT spf.name, COUNT(*) as count
        FROM cpu_profile_stack_sample s
        JOIN stack_profile_callsite spc ON s.callsite_id = spc.id
        JOIN stack_profile_frame spf ON spc.frame_id = spf.id
        LEFT JOIN stack_profile_callsite child ON child.parent_id = spc.id
        WHERE child.id IS NULL
        GROUP BY spf.name
        ORDER BY spf.name
        """,
        out=Csv('''
        "name","count"
        "bar",1
        '''))

  def test_missing_symbol_handling(self):
    """Test handling of callchain entries with invalid symbol_id."""
    return DiffTestBlueprint(
        trace=SimpleperfProto(records=[
            """
            file {
              id: 1
              path: "/system/lib64/libc.so"
              symbol: "malloc"
            }
            """, """
            sample {
              time: 1000000000
              thread_id: 1234
              callchain {
                vaddr_in_file: 0x1000
                file_id: 1
                symbol_id: -1
              }
              callchain {
                vaddr_in_file: 0x2000
                file_id: 1
                symbol_id: 0
              }
            }
            """
        ]),
        query="""
        SELECT
          spf.name,
          CASE WHEN spf.name IS NULL THEN 1 ELSE 0 END AS is_null
        FROM cpu_profile_stack_sample s
        JOIN stack_profile_callsite spc ON s.callsite_id = spc.id
        JOIN stack_profile_frame spf ON spc.frame_id = spf.id
        ORDER BY spc.depth
        """,
        out=Csv('''
        "name","is_null"
        "[NULL]",1
        '''))

  def test_multiple_files_and_threads(self):
    """Test samples across multiple files and threads."""
    return DiffTestBlueprint(
        trace=SimpleperfProto(records=[
            """
            file {
              id: 1
              path: "/system/lib64/libc.so"
              symbol: "malloc"
            }
            """, """
            file {
              id: 2
              path: "/system/lib64/libm.so"
              symbol: "sqrt"
            }
            """, """
            sample {
              time: 1000000000
              thread_id: 1234
              callchain {
                vaddr_in_file: 0x1000
                file_id: 1
                symbol_id: 0
              }
            }
            """, """
            thread {
              thread_id: 5678
              process_id: 9999
              thread_name: "WorkerThread"
            }
            """, """
            sample {
              time: 2000000000
              thread_id: 5678
              callchain {
                vaddr_in_file: 0x2000
                file_id: 2
                symbol_id: 0
              }
            }
            """
        ]),
        query="""
        SELECT
          t.name AS thread_name,
          spf.name AS function_name,
          spm.name AS mapping_name
        FROM cpu_profile_stack_sample s
        JOIN thread t USING (utid)
        JOIN stack_profile_callsite spc ON s.callsite_id = spc.id
        JOIN stack_profile_frame spf ON spc.frame_id = spf.id
        JOIN stack_profile_mapping spm ON spf.mapping = spm.id
        ORDER BY s.ts
        """,
        out=Csv('''
        "thread_name","function_name","mapping_name"
        "[NULL]","malloc","/system/lib64/libc.so"
        "WorkerThread","sqrt","/system/lib64/libm.so"
        '''))

  def test_cpu_profiling_summary_tree(self):
    """Test CPU profiling summary tree."""
    return DiffTestBlueprint(
        trace=SimpleperfProto(records=[
            """
            file {
              id: 1
              path: "/system/lib64/libc.so"
              symbol: "main"
              symbol: "work"
            }
            """, """
            sample {
              time: 1000000000
              thread_id: 1234
              callchain {
                vaddr_in_file: 0x2000
                file_id: 1
                symbol_id: 1
              }
              callchain {
                vaddr_in_file: 0x1000
                file_id: 1
                symbol_id: 0
              }
            }
            """, """
            sample {
              time: 2000000000
              thread_id: 1234
              callchain {
                vaddr_in_file: 0x2000
                file_id: 1
                symbol_id: 1
              }
              callchain {
                vaddr_in_file: 0x1000
                file_id: 1
                symbol_id: 0
              }
            }
            """, """
            sample {
              time: 3000000000
              thread_id: 1234
              callchain {
                vaddr_in_file: 0x1000
                file_id: 1
                symbol_id: 0
              }
            }
            """
        ]),
        query="""
        INCLUDE PERFETTO MODULE stacks.cpu_profiling;
        SELECT name, self_count, cumulative_count
        FROM cpu_profiling_summary_tree
        ORDER BY id
        """,
        out=Csv('''
        "name","self_count","cumulative_count"
        "main",1,3
        "work",2,2
        '''))
