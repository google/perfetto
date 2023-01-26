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
from python.generators.diff_tests.testing import TestSuite


class Profiling(TestSuite):
  # Perf profiling  tests.
  def test_profiler_smaps(self):
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
          }
        }
        packet {
          trusted_packet_sequence_id: 999
          timestamp: 10
          smaps_packet {
            pid: 2
            entries {
              path: "/system/lib64/libc.so"
              size_kb: 20
              private_dirty_kb: 4
              swap_kb: 4
            }
            entries {
              path: "[anon: libc_malloc]"
              size_kb: 30
              private_dirty_kb: 10
              swap_kb: 10
            }
          }
        }
        """),
        query="""
        SELECT id, type, upid, ts, path, size_kb, private_dirty_kb, swap_kb
        FROM profiler_smaps;
        """,
        out=Csv("""
        "id","type","upid","ts","path","size_kb","private_dirty_kb","swap_kb"
        0,"profiler_smaps",2,10,"/system/lib64/libc.so",20,4,4
        1,"profiler_smaps",2,10,"[anon: libc_malloc]",30,10,10
        """))

  def test_profiler_smaps_metric(self):
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
          }
        }
        packet {
          trusted_packet_sequence_id: 999
          timestamp: 10
          smaps_packet {
            pid: 2
            entries {
              path: "/system/lib64/libc.so"
              size_kb: 20
              private_dirty_kb: 4
              swap_kb: 4
            }
            entries {
              path: "[anon: libc_malloc]"
              size_kb: 30
              private_dirty_kb: 10
              swap_kb: 10
            }
          }
        }
        """),
        query=Metric('profiler_smaps'),
        out=TextProto(r"""
        profiler_smaps {
          instance {
            process {
              name: "system_server"
              uid: 1000
            }
            mappings {
              path: "[anon: libc_malloc]"
              size_kb: 30
              private_dirty_kb: 10
              swap_kb: 10
            }
            mappings {
              path: "/system/lib64/libc.so"
              size_kb: 20
              private_dirty_kb: 4
              swap_kb: 4
            }
          }
        }
        """))

  # Regression test for b/222297079: when cumulative size in a flamegraph
  # a signed 32-bit integer.
  def test_heap_graph_flamegraph_matches_objects(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_huge_size.textproto'),
        query="""
        SELECT
          obj.upid AS upid,
          obj.graph_sample_ts AS ts,
          SUM(obj.self_size + obj.native_size) AS total_objects_size,
          (
            SELECT SUM(cumulative_size)
            FROM experimental_flamegraph
            WHERE experimental_flamegraph.upid = obj.upid
              AND experimental_flamegraph.ts = obj.graph_sample_ts
              AND profile_type = 'graph'
              AND depth = 0 -- only the roots
          ) AS total_flamegraph_size
        FROM
          heap_graph_object AS obj
        WHERE
          obj.reachable != 0
        GROUP BY obj.upid, obj.graph_sample_ts;
        """,
        out=Csv("""
        "upid","ts","total_objects_size","total_flamegraph_size"
        1,10,3000000036,3000000036
        """))

  # TODO(b/153552977): Stop supporting legacy heap graphs. These never made it
  # a public release, so we should eventually stop supporting workarounds for
  def test_heap_graph_flamegraph(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_legacy.textproto'),
        query="""
        SELECT
          id,
          depth,
          name,
          map_name,
          count,
          cumulative_count,
          size,
          cumulative_size,
          parent_id
        FROM experimental_flamegraph
        WHERE upid = (SELECT max(upid) FROM heap_graph_object)
          AND profile_type = 'graph'
          AND ts = (SELECT max(graph_sample_ts) FROM heap_graph_object)
        LIMIT 10;
        """,
        out=Path('heap_graph_flamegraph.out'))

  def test_stack_profile_tracker_empty_callstack(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          clock_snapshot {
            clocks: {
              clock_id: 6 # BOOTTIME
              timestamp: 0
            }
            clocks: {
              clock_id: 4 # MONOTONIC_COARSE
              timestamp: 0
            }
          }
        }

        packet {
          previous_packet_dropped: true
          incremental_state_cleared: true
          trusted_packet_sequence_id: 1
          timestamp: 0
          interned_data {
            callstacks {
              iid: 1
            }
          }
        }

        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          profile_packet {
            index: 0
            continued: false
            process_dumps {
              samples {
                callstack_id: 1
                self_allocated: 1
                alloc_count: 1
              }
              samples {
                callstack_id: 1
                self_allocated: 1
                alloc_count: 1
              }
            }
          }
        }
        """),
        query="""
        SELECT count(1) AS count FROM heap_profile_allocation;
        """,
        out=Csv("""
        "count"
        0
        """))

  # perf_sample table (traced_perf) with android R and S trace inputs.
  def test_perf_sample_rvc(self):
    return DiffTestBlueprint(
        trace=Path('../../data/perf_sample.pb'),
        query="""
        SELECT ps.ts, ps.cpu, ps.cpu_mode, ps.unwind_error, ps.perf_session_id,
               pct.name AS cntr_name, pct.is_timebase,
               thread.tid,
               spf.name
        FROM experimental_annotated_callstack eac
        JOIN perf_sample ps
          ON (eac.start_id = ps.callsite_id)
        JOIN perf_counter_track pct
          USING(perf_session_id, cpu)
        JOIN thread
          USING(utid)
        JOIN stack_profile_frame spf
          ON (eac.frame_id = spf.id)
        ORDER BY ps.ts ASC, eac.depth ASC;
        """,
        out=Path('perf_sample_rvc.out'))

  def test_perf_sample_sc(self):
    return DiffTestBlueprint(
        trace=Path('../../data/perf_sample_sc.pb'),
        query="""
        SELECT ps.ts, ps.cpu, ps.cpu_mode, ps.unwind_error, ps.perf_session_id,
               pct.name AS cntr_name, pct.is_timebase,
               thread.tid,
               spf.name
        FROM experimental_annotated_callstack eac
        JOIN perf_sample ps
          ON (eac.start_id = ps.callsite_id)
        JOIN perf_counter_track pct
          USING(perf_session_id, cpu)
        JOIN thread
          USING(utid)
        JOIN stack_profile_frame spf
          ON (eac.frame_id = spf.id)
        ORDER BY ps.ts ASC, eac.depth ASC;
        """,
        out=Path('perf_sample_sc.out'))
