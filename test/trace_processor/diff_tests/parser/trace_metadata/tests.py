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

import json

from python.generators.diff_tests.testing import Csv, ExpectedError, RawText
from python.generators.diff_tests.testing import Tar, TextProto, Zip
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite

# Builtin clock ids used below: REALTIME=1, MONOTONIC=3, BOOTTIME=6.

# A proto trace with a clock snapshot tying the builtin clocks together
# (REALTIME - BOOTTIME = 1_700_000_000_000_000_000, BOOTTIME - MONOTONIC =
# 500_000_000) and one slice 'proto_slice' at BOOTTIME 1_100_000_000.
_SPINE_CLOCK_SNAPSHOT = '''
  packet {
    clock_snapshot {
      clocks { clock_id: 6 timestamp: 1000000000 }
      clocks { clock_id: 1 timestamp: 1700000001000000000 }
      clocks { clock_id: 3 timestamp: 500000000 }
    }
  }
'''

_PROTO_SLICE = '''
  packet {
    trusted_packet_sequence_id: 1
    track_descriptor { uuid: 12345 }
  }
  packet {
    trusted_packet_sequence_id: 1
    timestamp: 1100000000
    track_event {
      type: TYPE_SLICE_BEGIN
      track_uuid: 12345
      name: "proto_slice"
    }
  }
  packet {
    trusted_packet_sequence_id: 1
    timestamp: 1200000000
    track_event { type: TYPE_SLICE_END track_uuid: 12345 }
  }
'''

SPINE = TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE)

# The same proto trace without any clock snapshot: a single-clock proto trace.
SOLO_PROTO = TextProto(_PROTO_SLICE)

# Packets emitted by a remote machine (machine_id 7).
_M7_PROCESS = '''
  packet {
    machine_id: 7
    process_tree { processes { pid: 30 ppid: 0 cmdline: "m7_proc" } }
  }
'''

_M7_CLOCK_SNAPSHOT = '''
  packet {
    machine_id: 7
    clock_snapshot {
      clocks { clock_id: 6 timestamp: 50000000000 }
      clocks { clock_id: 3 timestamp: 1000000000 }
    }
  }
'''

# A systrace with one slice 'sys_slice' from 1.0s to 1.5s (MONOTONIC).
SYSTRACE = '''# tracer: nop
#
  app-100 (  100) [001] ...1  1.000000: tracing_mark_write: B|100|sys_slice
  app-100 (  100) [001] ...1  1.500000: tracing_mark_write: E|100
'''


# A Chrome JSON trace with one complete event at ts=2000us, dur=500us. With
# identity clock handling its slice lands at trace ts 2_000_000ns.
def _json_trace(name, pid=10):
  return json.dumps({
      'traceEvents': [{
          'pid': pid,
          'tid': pid,
          'ts': 2000,
          'dur': 500,
          'ph': 'X',
          'name': name,
      }]
  })


def _meta(payload):
  return json.dumps({'perfetto_metadata': payload})


class TraceMetadata(TestSuite):
  """Tests for the perfetto_metadata sidecar JSON.

  A perfetto_metadata file inside an archive (zip/tar) overrides clock and
  machine handling for the other files in the archive.
  """

  # --- Detection & envelope ---

  # The metadata file is recognized as its own trace type and is processed
  # before any other file in the archive (even proto). An entry with only a
  # path is a valid no-op.
  def test_detected_and_processed_first(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'app.json'
                }],
            }),
            'app.json': _json_trace('json_slice'),
        }),
        query='''
          SELECT name, trace_type, processing_order
          FROM __intrinsic_trace_file
          ORDER BY processing_order;
        ''',
        out=Csv('''
        "name","trace_type","processing_order"
        "[NULL]","zip",0
        "meta.json","perfetto_metadata",1
        "app.json","json",2
        '''))

  # --- trace_time_clock ---

  # Top-level trace_time_clock overrides the global trace time domain. The
  # proto spine's slice (BOOTTIME 1_100_000_000) is converted to REALTIME via
  # the snapshot.
  def test_trace_time_clock_realtime(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'trace_time_clock': 'REALTIME',
            }),
            'spine.pb': SPINE,
        }),
        query='''
          SELECT
            (SELECT int_value FROM metadata
             WHERE name = 'trace_time_clock_id') AS clock_id,
            (SELECT ts FROM slice WHERE name = 'proto_slice') AS ts;
        ''',
        out=Csv('''
        "clock_id","ts"
        1,1700000001100000000
        '''))

  # --- offset_ns ---

  # offset_ns shifts a file's events relative to where they would land by
  # default. Two JSON files (slices at 2000us = 2_000_000ns identity) get
  # different offsets; the proto spine is unaffected.
  def test_json_offsets_two_files(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [
                    {
                        'path': 'a.json',
                        'clocks': {
                            'offset_ns': 500000000
                        }
                    },
                    {
                        'path': 'b.json',
                        'clocks': {
                            'offset_ns': -1000000
                        }
                    },
                ],
            }),
            'a.json': _json_trace('a_slice', pid=10),
            'b.json': _json_trace('b_slice', pid=11),
            'spine.pb': SPINE,
        }),
        query='''
          SELECT name, ts, dur FROM slice
          WHERE name IN ('a_slice', 'b_slice', 'proto_slice')
          ORDER BY name;
        ''',
        out=Csv('''
        "name","ts","dur"
        "a_slice",502000000,500000
        "b_slice",1000000,500000
        "proto_slice",1100000000,100000000
        '''))

  # The metadata file works in tar archives too, not just zip.
  def test_offset_in_tar(self):
    return DiffTestBlueprint(
        trace=Tar({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'a.json',
                    'clocks': {
                        'offset_ns': 500000000
                    }
                }],
            }),
            'a.json': _json_trace('a_slice'),
            'spine.pb': SPINE,
        }),
        query='''
          SELECT name, ts FROM slice
          WHERE name IN ('a_slice', 'proto_slice')
          ORDER BY name;
        ''',
        out=Csv('''
        "name","ts"
        "a_slice",502000000
        "proto_slice",1100000000
        '''))

  # --- anchor ---

  # An anchor pins a timestamp as written in the file (file-native units, us
  # for JSON) to a value on a named builtin clock. ts=1000us corresponds to
  # BOOTTIME 1_500_000_000, so the slice at 2000us lands at
  # 1_500_000_000 + (2000 - 1000) * 1000 = 1_501_000_000.
  def test_json_anchor_to_boottime(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'app.json',
                    'clocks': {
                        'anchor': {
                            'ts': 1000,
                            'is': {
                                'clock': 'BOOTTIME',
                                'ts': 1500000000
                            },
                        },
                    },
                }],
            }),
            'app.json': _json_trace('json_slice'),
            'spine.pb': SPINE,
        }),
        query='''
          SELECT name, ts FROM slice
          WHERE name IN ('json_slice', 'proto_slice')
          ORDER BY name;
        ''',
        out=Csv('''
        "name","ts"
        "json_slice",1501000000
        "proto_slice",1100000000
        '''))

  # A utc anchor is sugar for clock=REALTIME. ts=0us corresponds to
  # 2023-11-14T22:13:21.5Z = REALTIME 1_700_000_001_500_000_000. Via the proto
  # spine's REALTIME<->BOOTTIME snapshot the slice at 2000us lands at
  # 1_500_000_000 + 2_000_000 = 1_502_000_000. This exercises routing the
  # anchor through the machine's shared clock graph (TraceFile -> REALTIME ->
  # BOOTTIME) rather than the file's isolated one.
  def test_json_anchor_to_utc(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'app.json',
                    'clocks': {
                        'anchor': {
                            'ts': 0,
                            'is': {
                                'utc': '2023-11-14T22:13:21.5Z'
                            },
                        },
                    },
                }],
            }),
            'app.json': _json_trace('json_slice'),
            'spine.pb': SPINE,
        }),
        query='''
          SELECT name, ts FROM slice
          WHERE name IN ('json_slice', 'proto_slice')
          ORDER BY name;
        ''',
        out=Csv('''
        "name","ts"
        "json_slice",1502000000
        "proto_slice",1100000000
        '''))

  # --- clocks.native ---

  # Baseline (no metadata file): systrace timestamps are MONOTONIC, so via the
  # spine's MONOTONIC<->BOOTTIME snapshot the 1.0s slice lands at
  # 1_000_000_000 + 500_000_000 = 1_500_000_000.
  def test_systrace_clock_baseline_no_config(self):
    return DiffTestBlueprint(
        trace=Zip({
            'sys.systrace': SYSTRACE,
            'spine.pb': SPINE,
        }),
        query='''
          SELECT name, ts, dur FROM slice WHERE name = 'sys_slice';
        ''',
        out=Csv('''
        "name","ts","dur"
        "sys_slice",1500000000,500000000
        '''))

  # clocks.native reinterprets the file's native clock: the same systrace with
  # native=BOOTTIME keeps its timestamps unconverted at 1_000_000_000.
  def test_systrace_native_clock_override(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'sys.systrace',
                    'clocks': {
                        'native': 'BOOTTIME'
                    }
                }],
            }),
            'sys.systrace': SYSTRACE,
            'spine.pb': SPINE,
        }),
        query='''
          SELECT name, ts, dur FROM slice WHERE name = 'sys_slice';
        ''',
        out=Csv('''
        "name","ts","dur"
        "sys_slice",1000000000,500000000
        '''))

  # --- No proto spine ---

  # A metadata file works without any proto trace: trace_time_clock is set
  # explicitly and the JSON file is shifted by offset_ns.
  def test_json_only_trace_time_and_offset(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'trace_time_clock': 'BOOTTIME',
                'files': [{
                    'path': 'app.json',
                    'clocks': {
                        'offset_ns': 1000000
                    }
                }],
            }),
            'app.json': _json_trace('json_slice'),
        }),
        query='''
          SELECT
            (SELECT int_value FROM metadata
             WHERE name = 'trace_time_clock_id') AS clock_id,
            (SELECT ts FROM slice WHERE name = 'json_slice') AS ts;
        ''',
        out=Csv('''
        "clock_id","ts"
        6,3000000
        '''))

  # --- Proto traces with a single clock ---

  # A proto trace which uses a single clock (no ClockSnapshot, no explicit
  # timestamp_clock_id, no remote machines) accepts clock overrides like any
  # other single-clock format.
  def test_proto_single_clock_offset(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'solo.pb',
                    'clocks': {
                        'offset_ns': 250
                    }
                }],
            }),
            'solo.pb': SOLO_PROTO,
        }),
        query='''
          SELECT name, ts FROM slice WHERE name = 'proto_slice';
        ''',
        out=Csv('''
        "name","ts"
        "proto_slice",1100000250
        '''))

  # --- Machines ---

  # Assigning a file to a machine id no other trace establishes creates that
  # machine. The JSON file's events land on machine 7; with no snapshots in
  # machine 7's clock graph the identity sync applies and ts is unchanged.
  def test_machine_assignment_fresh_machine(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'app.json',
                    'machine': {
                        'id': 7
                    }
                }],
            }),
            'app.json': _json_trace('json_slice'),
            'spine.pb': SPINE,
        }),
        query='''
          SELECT s.name, m.raw_id, s.ts
          FROM slice s
          JOIN thread_track tt ON s.track_id = tt.id
          JOIN thread t USING(utid)
          JOIN machine m ON t.machine_id = m.id
          WHERE s.name = 'json_slice';
        ''',
        out=Csv('''
        "name","raw_id","ts"
        "json_slice",7,2000000
        '''))

  # A JSON file assigned to machine 7 shares per-machine state with proto
  # packets from machine 7 in the same archive: pid 30 exists in the proto's
  # machine-7 process tree and in the JSON trace, and must resolve to a single
  # thread, and the JSON file's slice must be attributed to machine 7.
  def test_machine_assignment_merges_with_proto(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'app.json',
                    'machine': {
                        'id': 7
                    }
                }],
            }),
            'app.json': _json_trace('m7_json_slice', pid=30),
            'spine.pb': TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE +
                                  _M7_PROCESS),
        }),
        query='''
          SELECT
            (SELECT count(*) FROM thread WHERE tid = 30) AS threads,
            (SELECT m.raw_id
             FROM slice s
             JOIN thread_track tt ON s.track_id = tt.id
             JOIN thread t USING(utid)
             JOIN machine m ON t.machine_id = m.id
             WHERE s.name = 'm7_json_slice') AS slice_machine;
        ''',
        out=Csv('''
        "threads","slice_machine"
        1,7
        '''))

  # Anchor clock names resolve in the file's machine context: BOOTTIME means
  # machine 7's BOOTTIME (50_000_000_000 per its snapshot), not the host's.
  # The slice at 2000us lands at 50_000_000_000 + 2_000_000.
  def test_machine_anchor_in_machine_domain(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'app.json',
                    'machine': {
                        'id': 7
                    },
                    'clocks': {
                        'anchor': {
                            'ts': 0,
                            'is': {
                                'clock': 'BOOTTIME',
                                'ts': 50000000000
                            },
                        },
                    },
                }],
            }),
            'app.json': _json_trace('json_slice'),
            'spine.pb': TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE +
                                  _M7_CLOCK_SNAPSHOT),
        }),
        query='''
          SELECT name, ts FROM slice
          WHERE name IN ('json_slice', 'proto_slice')
          ORDER BY name;
        ''',
        out=Csv('''
        "name","ts"
        "json_slice",50002000000
        "proto_slice",1100000000
        '''))

  # --- Envelope errors ---

  def test_error_missing_version(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({}),
            'app.json': _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_metadata: missing required field: version'))

  def test_error_unsupported_version(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({'version': 99}),
            'app.json': _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError('perfetto_metadata: unsupported version: 99'))

  def test_error_unknown_field(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'bogus': True
            }),
            'app.json': _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError('perfetto_metadata: unknown field: bogus'))

  def test_error_unknown_clock_name(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'trace_time_clock': 'BOOTIME'
            }),
            'app.json': _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError('perfetto_metadata: unknown clock name: BOOTIME'))

  # A perfetto_metadata file fed to trace_processor on its own (not inside an
  # archive) is an error: it configures nothing.
  def test_error_standalone_config(self):
    return DiffTestBlueprint(
        trace=RawText('{"perfetto_metadata": {"version": 1}}'),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_metadata file must be inside an archive'))

  def test_error_multiple_configs(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta1.json': _meta({'version': 1}),
            'meta2.json': _meta({'version': 1}),
            'app.json': _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError('multiple perfetto_metadata files in archive'))

  # --- files entry errors ---

  def test_error_path_no_match(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'missing.json'
                }],
            }),
            'app.json': _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_metadata: no file in archive matches path: missing.json'
        ))

  def test_error_duplicate_path(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [
                    {
                        'path': 'app.json',
                        'clocks': {
                            'offset_ns': 1
                        }
                    },
                    {
                        'path': 'app.json',
                        'clocks': {
                            'offset_ns': 2
                        }
                    },
                ],
            }),
            'app.json': _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_metadata: duplicate entry for path: app.json'))

  def test_error_offset_and_anchor_exclusive(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'app.json',
                    'clocks': {
                        'offset_ns': 1,
                        'anchor': {
                            'ts': 0,
                            'is': {
                                'clock': 'BOOTTIME',
                                'ts': 1
                            }
                        },
                    },
                }],
            }),
            'app.json': _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_metadata: offset_ns and anchor are mutually exclusive'))

  def test_error_anchor_missing_is(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'app.json',
                    'clocks': {
                        'anchor': {
                            'ts': 0
                        }
                    }
                }],
            }),
            'app.json': _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_metadata: anchor: missing required field: is'))

  # --- Proto single-ness enforcement (optimistic + lazy) ---

  # A clocks override on a proto trace which emits a ClockSnapshot (proof of
  # multiple clocks) fails when the snapshot is parsed.
  def test_error_proto_multi_clock(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'spine.pb',
                    'clocks': {
                        'offset_ns': 1
                    }
                }],
            }),
            'spine.pb': SPINE,
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'clock overrides require the trace to use a single clock'))

  # A clocks override on a proto trace containing packets from a remote
  # machine (machine_id != 0) fails: anchors are ambiguous across machines.
  def test_error_proto_multi_machine_clock_override(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'spine.pb',
                    'clocks': {
                        'offset_ns': 1
                    }
                }],
            }),
            'spine.pb': TextProto(_PROTO_SLICE + _M7_PROCESS),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'clock overrides require the trace to come from a single machine')
    )

  # A machine override on a proto trace containing packets from a remote
  # machine fails: the trace manages its own machine identity.
  def test_error_proto_multi_machine_machine_override(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'files': [{
                    'path': 'spine.pb',
                    'machine': {
                        'id': 3
                    }
                }],
            }),
            'spine.pb': TextProto(_PROTO_SLICE + _M7_PROCESS),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'machine override requires the trace to come from a single machine'
        ))
