#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite

# Synthetic V8 CPU profile trace shared by all tests in this file. Emits:
#   * a clock_snapshot anchoring MONOTONIC == BOOTTIME at zero so the
#     thread-descriptor-anchored streaming samples and the BOOTTIME-stamped
#     session packets land in the same domain;
#   * a thread descriptor + interned mappings/frames/callstacks;
#   * a V8CpuProfileSession PHASE_START at ts=1ms;
#   * two streaming profile samples 5us and 15us into the session;
#   * a V8CpuProfileSession PHASE_END at ts=2ms.
_V8_TRACE = TextProto(r"""
packet {
  clock_snapshot {
    clocks: { clock_id: 6 timestamp: 0 }
    clocks: { clock_id: 3 timestamp: 0 }
  }
}
packet {
  trusted_packet_sequence_id: 1
  incremental_state_cleared: true
  thread_descriptor {
    pid: 100
    tid: 200
    reference_timestamp_us: 1000
    reference_thread_time_us: 0
  }
  interned_data {
    mappings { iid: 1 build_id: 1 }
    build_ids { iid: 1 str: "abcd" }
    function_names { iid: 1 str: "fnA" }
    function_names { iid: 2 str: "fnB" }
    frames {
      iid: 1
      function_name_id: 1
      mapping_id: 1
      [perfetto.protos.V8FrameExtensions.v8_tier]: V8_TIER_TURBOFAN
      [perfetto.protos.V8FrameExtensions.v8_script_id]: 42
    }
    frames { iid: 2 function_name_id: 2 mapping_id: 1 }
    callstacks { iid: 1 frame_ids: 1 }
    callstacks { iid: 2 frame_ids: 1 frame_ids: 2 }
  }
}
packet {
  trusted_packet_sequence_id: 2
  incremental_state_cleared: true
  timestamp: 1000000
  thread_descriptor {
    pid: 100
    tid: 200
    reference_timestamp_us: 1000
  }
}
packet {
  trusted_packet_sequence_id: 2
  timestamp: 1000000
  v8_cpu_profile_session {
    phase: PHASE_START
    session_id: 7
    source: "renderer"
    wall_time_us: 50000
    thread_time_us: 10000
  }
}
packet {
  trusted_packet_sequence_id: 1
  streaming_profile_packet {
    callstack_iid: 1
    timestamp_delta_us: 5
    [perfetto.protos.V8StreamingProfileExtensions.v8_sample_kind]:
        V8_SAMPLE_KIND_NORMAL
    [perfetto.protos.V8StreamingProfileExtensions.v8_leaf_line]: 7
    [perfetto.protos.V8StreamingProfileExtensions.v8_leaf_column]: 8
    [perfetto.protos.V8StreamingProfileExtensions.v8_session_id]: 7
  }
}
packet {
  trusted_packet_sequence_id: 1
  streaming_profile_packet {
    callstack_iid: 2
    timestamp_delta_us: 10
    [perfetto.protos.V8StreamingProfileExtensions.v8_sample_kind]:
        V8_SAMPLE_KIND_IDLE
    [perfetto.protos.V8StreamingProfileExtensions.v8_leaf_line]: 0
    [perfetto.protos.V8StreamingProfileExtensions.v8_leaf_column]: 0
    [perfetto.protos.V8StreamingProfileExtensions.v8_session_id]: 7
  }
}
packet {
  # A generic stack sample on the same thread and within the V8 session's
  # timestamp range. It must not enter the V8 legacy export without a V8
  # session-id extension.
  trusted_packet_sequence_id: 1
  streaming_profile_packet {
    callstack_iid: 1
    timestamp_delta_us: 10
  }
}
packet {
  trusted_packet_sequence_id: 2
  timestamp: 2000000
  v8_cpu_profile_session {
    phase: PHASE_END
    session_id: 7
    wall_time_us: 60000
    thread_time_us: 11000
  }
}
""")


class V8CpuProfile(TestSuite):

  def test_v8_cpu_profile_internal_session_view(self):
    return DiffTestBlueprint(
        trace=_V8_TRACE,
        query="""
        INCLUDE PERFETTO MODULE v8.cpu_profile;
        SELECT
          session_id,
          source,
          start_ts,
          end_ts,
          start_time_us,
          end_time_us,
          start_thread_ts,
          end_thread_ts
        FROM _v8_cpu_profile_session
        ORDER BY session_id;
        """,
        out=Csv("""
        "session_id","source","start_ts","end_ts","start_time_us","end_time_us","start_thread_ts","end_thread_ts"
        7,"renderer",1000000,2000000,50000,60000,10000000,11000000
        """))

  # The internal export view feeding the legacy DevTools `Profile`/
  # `ProfileChunk` head and tail events. One row per session.
  def test_v8_cpu_profile_legacy_export_session(self):
    return DiffTestBlueprint(
        trace=_V8_TRACE,
        query="""
        INCLUDE PERFETTO MODULE v8.cpu_profile;
        SELECT
          session_id,
          start_ts,
          end_ts,
          start_time_us,
          end_time_us,
          start_thread_ts,
          end_thread_ts
        FROM _v8_cpu_profile_legacy_export_session
        ORDER BY session_id;
        """,
        out=Csv("""
        "session_id","start_ts","end_ts","start_time_us","end_time_us","start_thread_ts","end_thread_ts"
        7,1000000,2000000,50000,60000,10000000,11000000
        """))

  # The node view feeds the legacy `cpuProfile.nodes[]` array. node_id is the
  # monotonic per-session integer DevTools references; parent_node_id forms a
  # tree. Closure of all callsites reachable from any sample, ordered by
  # callsite_id.
  def test_v8_cpu_profile_legacy_export_node(self):
    return DiffTestBlueprint(
        trace=_V8_TRACE,
        query="""
        INCLUDE PERFETTO MODULE v8.cpu_profile;
        SELECT
          node_id,
          parent_node_id,
          function_name,
          url,
          line_number,
          column_number,
          code_type,
          deopt_reason,
          script_id
        FROM _v8_cpu_profile_legacy_export_node
        ORDER BY node_id;
        """,
        out=Csv("""
        "node_id","parent_node_id","function_name","url","line_number","column_number","code_type","deopt_reason","script_id"
        1,"[NULL]","fnA","[NULL]","[NULL]","[NULL]","JS","[NULL]",42
        2,1,"fnB","[NULL]","[NULL]","[NULL]","other","[NULL]","[NULL]"
        """))

  # The sample view feeds `cpuProfile.samples[]`/`timeDeltas[]`. delta_us is
  # the per-sample delta in microseconds: first sample relative to start_ts,
  # then per-sample deltas relative to the previous sample.
  def test_v8_cpu_profile_legacy_export_sample(self):
    return DiffTestBlueprint(
        trace=_V8_TRACE,
        query="""
        INCLUDE PERFETTO MODULE v8.cpu_profile;
        SELECT
          node_id,
          delta_us,
          leaf_line,
          leaf_column,
          sample_kind
        FROM _v8_cpu_profile_legacy_export_sample
        ORDER BY delta_us;
        """,
        out=Csv("""
        "node_id","delta_us","leaf_line","leaf_column","sample_kind"
        1,5,7,8,"NORMAL"
        2,10,0,0,"IDLE"
        """))
