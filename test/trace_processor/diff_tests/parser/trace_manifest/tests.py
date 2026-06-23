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

import gzip
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


# A systrace with one slice 'sys_slice' from 1.0s to 1.5s (MONOTONIC).
SYSTRACE = '''# tracer: nop
#
  app-100 (  100) [001] ...1  1.000000: tracing_mark_write: B|100|sys_slice
  app-100 (  100) [001] ...1  1.500000: tracing_mark_write: E|100
'''

# The same proto trace without any clock snapshot: a single-clock proto trace.
SOLO_PROTO = TextProto(_PROTO_SLICE)

# A second single-clock proto trace whose slice is named 'm7_slice'.
M7_PROTO = TextProto(_PROTO_SLICE.replace('proto_slice', 'm7_slice'))

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


# A Chrome JSON trace with one complete event at ts=2000us, dur=500us and an
# embedded systrace ('systemTraceEvents') with one slice 'sys_in_json' from
# 1.0s to 1.5s.
def _json_trace_with_systrace(name, pid=10):
  return json.dumps({
      'traceEvents': [{
          'pid': pid,
          'tid': pid,
          'ts': 2000,
          'dur': 500,
          'ph': 'X',
          'name': name,
      }],
      'systemTraceEvents':
          '# tracer: nop\n'
          '  app-100 (  100) [001] ...1  1.000000: '
          'tracing_mark_write: B|100|sys_in_json\n'
          '  app-100 (  100) [001] ...1  1.500000: tracing_mark_write: E|100\n',
  })


def _meta(payload):
  return json.dumps({'perfetto_manifest': payload})


# Query used by the cross-machine alignment tests: each slice with its
# trace-time ts and the machine it was attributed to.
_ALIGN_QUERY = '''
  SELECT s.name, s.ts, m.name AS machine
  FROM slice s
  JOIN track t ON s.track_id = t.id
  JOIN machine m ON t.machine_id = m.id
  WHERE s.name GLOB '*_slice'
  ORDER BY s.ts;
'''


# A proto trace with a BOOTTIME<->REALTIME clock snapshot and one slice |name|
# at BOOTTIME |at|. |boot|/|realtime| are the correlated snapshot readings.
def _proto_rt(name, seq, uuid, boot, realtime, at):
  return TextProto(
      'packet { clock_snapshot {\n'
      '  clocks { clock_id: 6 timestamp: %d }\n'
      '  clocks { clock_id: 1 timestamp: %d }\n'
      '} }\n'
      'packet { trusted_packet_sequence_id: %d track_descriptor { uuid: %d } }\n'
      'packet { trusted_packet_sequence_id: %d timestamp: %d\n'
      '  track_event { type: TYPE_SLICE_BEGIN track_uuid: %d name: "%s" } }\n'
      'packet { trusted_packet_sequence_id: %d timestamp: %d\n'
      '  track_event { type: TYPE_SLICE_END track_uuid: %d } }\n' %
      (boot, realtime, seq, uuid, seq, at, uuid, name, seq, at + 100000000, uuid))


# A proto trace with a BOOTTIME-only clock snapshot (no REALTIME): its events
# are on a real BOOTTIME clock. One slice |name| at BOOTTIME |at|.
def _proto_boot_snap(name, seq, uuid, boot, at):
  return TextProto(
      'packet { clock_snapshot { clocks { clock_id: 6 timestamp: %d } } }\n'
      'packet { trusted_packet_sequence_id: %d track_descriptor { uuid: %d } }\n'
      'packet { trusted_packet_sequence_id: %d timestamp: %d\n'
      '  track_event { type: TYPE_SLICE_BEGIN track_uuid: %d name: "%s" } }\n'
      'packet { trusted_packet_sequence_id: %d timestamp: %d\n'
      '  track_event { type: TYPE_SLICE_END track_uuid: %d } }\n' %
      (boot, seq, uuid, seq, at, uuid, name, seq, at + 100000000, uuid))


# A perfetto_manifest entry attributing |path| to machine |name|, optionally
# with a REALTIME anchor mapping the file's clock ts 0 to REALTIME |realtime|.
def _machine_file(path, name, realtime=None):
  entry = {'path': path, 'machine': {'name': name}}
  if realtime is not None:
    entry['clocks'] = {'ts': 0, 'is': {'clock': 'REALTIME', 'ts': realtime}}
  return entry


class TraceManifest(TestSuite):
  """Tests for the perfetto_manifest sidecar JSON.

  A perfetto_manifest file, as the first file of the trace (typically inside
  an archive, where sorting puts it first), overrides clock and machine
  handling for the files that follow.
  """

  # --- Detection & envelope ---

  # The metadata file is recognized as its own trace type and is processed
  # before any other file in the archive (even proto). An entry with only a
  # path is a valid no-op.
  def test_detected_and_processed_first(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [{
                        'path': 'app.json'
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='''
          SELECT name, trace_type, processing_order
          FROM __intrinsic_trace_file
          ORDER BY processing_order;
        ''',
        out=Csv('''
        "name","trace_type","processing_order"
        "[NULL]","zip",0
        "meta.json","perfetto_manifest",1
        "app.json","json",2
        '''))

  # The metadata file works in tar archives too, not just zip.
  def test_detected_in_tar(self):
    return DiffTestBlueprint(
        trace=Tar({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [{
                        'path': 'app.json'
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='''
          SELECT name, trace_type, processing_order
          FROM __intrinsic_trace_file
          ORDER BY processing_order;
        ''',
        out=Csv('''
        "name","trace_type","processing_order"
        "[NULL]","tar",0
        "meta.json","perfetto_manifest",1
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

  # A metadata file works without any proto trace: trace_time_clock is set
  # explicitly and the JSON file keeps its identity timestamps.
  def test_json_only_trace_time_clock(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({
                'version': 1,
                'trace_time_clock': 'BOOTTIME',
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
        6,2000000
        '''))

  # --- Envelope errors ---

  def test_error_missing_version(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({}),
            'app.json': _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError('perfetto_manifest: missing required field: version'))

  def test_error_unsupported_version(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({'version': 99}),
            'app.json': _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError('perfetto_manifest: unsupported version: 99'))

  def test_error_version_not_integer(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json': _meta({'version': 1.5}),
            'app.json': _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError('perfetto_manifest: version must be an integer'))

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
        out=ExpectedError('perfetto_manifest: unknown clock name: BOOTIME'))

  # A perfetto_manifest file fed to trace_processor on its own is trivially
  # the first file of the trace, so it parses fine (and configures nothing).
  def test_standalone_config(self):
    return DiffTestBlueprint(
        trace=RawText(_meta({
            'version': 1,
            'trace_time_clock': 'REALTIME'
        })),
        query='''
          SELECT int_value FROM metadata WHERE name = 'trace_time_clock_id';
        ''',
        out=Csv('''
        "int_value"
        1
        '''))

  # A gzip-wrapped metadata file sorts as a container, after proto traces:
  # by the time it is parsed it is no longer the first trace file.
  def test_error_gzipped_config_after_proto(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json.gz': gzip.compress(_meta({
                'version': 1
            }).encode()),
            'spine.pb': SPINE,
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_manifest file must be the first trace file'))

  def test_error_multiple_configs(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta1.json': _meta({'version': 1}),
            'meta2.json': _meta({'version': 1}),
            'app.json': _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError('multiple perfetto_manifest files in archive'))

  # --- offset_ns ---

  # offset_ns shifts a file's events relative to where they would land by
  # default. Two JSON files (slices at 2000us = 2_000_000ns identity) get
  # different offsets; the proto spine is unaffected.
  def test_json_offsets_two_files(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
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
            'a.json':
                _json_trace('a_slice', pid=10),
            'b.json':
                _json_trace('b_slice', pid=11),
            'spine.pb':
                SPINE,
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

  # The offset also applies in tar archives, and composes with an explicit
  # trace_time_clock.
  def test_offset_in_tar_with_trace_time_clock(self):
    return DiffTestBlueprint(
        trace=Tar({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'trace_time_clock':
                        'BOOTTIME',
                    'files': [{
                        'path': 'a.json',
                        'clocks': {
                            'offset_ns': 1000000
                        }
                    }],
                }),
            'a.json':
                _json_trace('a_slice'),
        }),
        query='''
          SELECT
            (SELECT int_value FROM metadata
             WHERE name = 'trace_time_clock_id') AS clock_id,
            (SELECT ts FROM slice WHERE name = 'a_slice') AS ts;
        ''',
        out=Csv('''
        "clock_id","ts"
        6,3000000
        '''))

  # Clock overrides on a JSON file also apply to the sched/slice rows derived
  # from its embedded systrace (systemTraceEvents), not just to traceEvents.
  # The proto spine provides the trace time domain the offset is relative to:
  # a lone file's private clock is itself the trace time, so an offset there
  # would be a no-op.
  def test_json_embedded_systrace_offset(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'app.json',
                        'clocks': {
                            'offset_ns': 500000000
                        }
                    }],
                }),
            'app.json':
                _json_trace_with_systrace('json_slice'),
            'spine.pb':
                SPINE,
        }),
        query='''
          SELECT name, ts, dur FROM slice
          WHERE name IN ('json_slice', 'sys_in_json')
          ORDER BY name;
        ''',
        out=Csv('''
        "name","ts","dur"
        "json_slice",502000000,500000
        "sys_in_json",1500000000,500000000
        '''))

  # --- anchor ---

  # An anchor maps a file timestamp (always nanoseconds, the unit every
  # tokenizer normalizes to) to a value on a named builtin clock.
  # ts=1_000_000 (the file's 1000us point) corresponds to BOOTTIME
  # 1_500_000_000, so the slice at 2000us lands at
  # 1_500_000_000 + 1_000_000 = 1_501_000_000.
  def test_json_anchor_to_boottime(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'app.json',
                        'clocks': {
                            'ts': 1000000,
                            'is': {
                                'clock': 'BOOTTIME',
                                'ts': 1500000000
                            },
                        },
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
            'spine.pb':
                SPINE,
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

  # A utc anchor is sugar for clock=REALTIME. ts=0 (ns) corresponds to
  # 2023-11-14T22:13:21.5Z = REALTIME 1_700_000_001_500_000_000. Via the proto
  # spine's REALTIME<->BOOTTIME snapshot the slice at 2000us lands at
  # 1_500_000_000 + 2_000_000 = 1_502_000_000. This exercises routing the
  # anchor through the machine's shared clock graph (TraceFile -> REALTIME ->
  # BOOTTIME) rather than the file's isolated one.
  def test_json_anchor_to_utc(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'app.json',
                        'clocks': {
                            'is': {
                                'utc': '2023-11-14T22:13:21.5Z'
                            },
                        },
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
            'spine.pb':
                SPINE,
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

  # The anchor correlation is mirrored into the clock_snapshot table (one row
  # per clock in the override, like proto ClockSnapshots) so that ClockConverter
  # (to_realtime, abs_time_str, the UI wall-clock axis) can see it even when
  # no proto trace provides snapshots.
  def test_utc_anchor_visible_in_clock_snapshot_table(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'app.json',
                        'clocks': {
                            'is': {
                                'utc': '2023-11-14T22:13:21.5Z'
                            },
                        },
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='''
          SELECT clock_id, clock_name, ts, clock_value FROM clock_snapshot
          ORDER BY clock_id;
        ''',
        out=Csv('''
        "clock_id","clock_name","ts","clock_value"
        1,"REALTIME",0,1700000001500000000
        11,"[NULL]",0,0
        '''))

  # --- clocks: identity / native ---

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

  # an is-with-clock identity reinterprets the file's native clock: the same systrace with
  # native=BOOTTIME keeps its timestamps unconverted at 1_000_000_000.
  def test_systrace_native_clock_override(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'sys.systrace',
                        'clocks': {
                            'is': {'clock': 'BOOTTIME'}
                        }
                    }],
                }),
            'sys.systrace':
                SYSTRACE,
            'spine.pb':
                SPINE,
        }),
        query='''
          SELECT name, ts, dur FROM slice WHERE name = 'sys_slice';
        ''',
        out=Csv('''
        "name","ts","dur"
        "sys_slice",1000000000,500000000
        '''))

  # native and offset_ns compose: the file's timeline is mapped onto the named
  # clock at the offset (file time 0 == BOOTTIME 100_000_000), so the 1.0s
  # slice lands at 1_100_000_000 instead of converting through the spine's
  # MONOTONIC<->BOOTTIME snapshot.
  def test_systrace_native_clock_with_offset(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'sys.systrace',
                        'clocks': {
                            'is': {'clock': 'BOOTTIME'},
                            'offset_ns': 100000000
                        }
                    }],
                }),
            'sys.systrace':
                SYSTRACE,
            'spine.pb':
                SPINE,
        }),
        query='''
          SELECT name, ts, dur FROM slice WHERE name = 'sys_slice';
        ''',
        out=Csv('''
        "name","ts","dur"
        "sys_slice",1100000000,500000000
        '''))

  # An anchor overrides the weak per-format clock guess (MONOTONIC for
  # systrace): the anchored file is moved onto its own private TraceFile
  # clock, so a sibling systrace without an override still converts through
  # the spine's real MONOTONIC<->BOOTTIME snapshot, unaffected by the anchor.
  def test_systrace_anchor_overrides_weak_clock(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'sys.systrace',
                        'clocks': {
                            'ts': 1000000000,
                            'is': {
                                'clock': 'BOOTTIME',
                                'ts': 5000000000
                            },
                        },
                    }],
                }),
            'sys.systrace':
                SYSTRACE,
            'sys2.systrace':
                SYSTRACE,
            'spine.pb':
                SPINE,
        }),
        query='''
          SELECT name, ts, dur FROM slice
          WHERE name = 'sys_slice'
          ORDER BY ts;
        ''',
        out=Csv('''
        "name","ts","dur"
        "sys_slice",1500000000,500000000
        "sys_slice",5000000000,500000000
        '''))

  # --- Proto traces with a single clock ---

  # A proto trace which uses a single clock (no ClockSnapshot, no explicit
  # timestamp_clock_id, no remote machines) accepts clock overrides like any
  # other single-clock format. The offset is negative to exercise a
  # backwards shift: the reader rebases it onto the file timestamp so the
  # injected edge keeps non-negative timestamps.
  def test_proto_single_clock_negative_offset(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'solo.pb',
                        'clocks': {
                            'offset_ns': -250
                        }
                    }],
                }),
            'solo.pb':
                SOLO_PROTO,
        }),
        query='''
          SELECT name, ts FROM slice WHERE name = 'proto_slice';
        ''',
        out=Csv('''
        "name","ts"
        "proto_slice",1099999750
        '''))

  # --- Clock override errors ---

  def test_error_offset_and_reading_exclusive(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'app.json',
                        'clocks': {
                            'offset_ns': 1,
                            'ts': 0,
                            'is': {
                                'clock': 'BOOTTIME',
                                'ts': 1
                            },
                        },
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'offset_ns and a reading (ts) are mutually exclusive'))

  def test_error_utc_impossible_date(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'app.json',
                        'clocks': {
                            'is': {
                                'utc': '2026-13-40T99:00:00Z'
                            },
                        },
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_manifest: clocks: invalid is.utc timestamp'))

  # A clock override on a perfetto_manifest or archive member is rejected:
  # such files have no per-trace clock state to override.
  def test_error_clock_override_on_metadata_file(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta1.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'meta2.json',
                        'clocks': {
                            'offset_ns': 1
                        }
                    }],
                }),
            'meta2.json':
                _meta({'version': 1}),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'overrides are not supported for trace files which are '
            'themselves archives or perfetto_manifest files'))

  # --- Proto single-ness enforcement (optimistic + lazy) ---

  # A clocks override on a proto trace which emits a ClockSnapshot (proof of
  # multiple clocks) fails when the snapshot is parsed.
  def test_error_proto_multi_clock(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [{
                        'path': 'spine.pb',
                        'clocks': {
                            'offset_ns': 1
                        }
                    }],
                }),
            'spine.pb':
                SPINE,
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'clock overrides require the trace to use a single clock'))

  # A clocks override on a proto trace containing packets from a remote
  # machine (machine_id != 0) fails: anchors/offsets are ambiguous across
  # machines.
  def test_error_proto_multi_machine_clock_override(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [{
                        'path': 'spine.pb',
                        'clocks': {
                            'offset_ns': 1
                        }
                    }],
                }),
            'spine.pb':
                TextProto(_PROTO_SLICE + _M7_PROCESS),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'clock overrides require the trace to come from a single machine'))

  # --- Machines ---

  # Assigning a file to a machine id no other trace establishes creates that
  # machine. The JSON file's events land on machine 7; with no snapshots in
  # machine 7's clock graph the identity sync applies and ts is unchanged.
  def test_machine_assignment_fresh_machine(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [{
                        'path': 'app.json',
                        'machine': {
                            'id': 7
                        }
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
            'spine.pb':
                SPINE,
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
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [{
                        'path': 'app.json',
                        'machine': {
                            'id': 7
                        }
                    }],
                }),
            'app.json':
                _json_trace('m7_json_slice', pid=30),
            'spine.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE + _M7_PROCESS),
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
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'app.json',
                        'machine': {
                            'id': 7
                        },
                        'clocks': {
                            'ts': 0,
                            'is': {
                                'clock': 'BOOTTIME',
                                'ts': 50000000000
                            },
                        },
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
            'spine.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE +
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

  # A machine override works alongside a sibling proto trace on the host
  # machine: each proto keeps its own machine attribution.
  def test_machine_override_with_sibling_proto(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [{
                        'path': 'm7.pb',
                        'machine': {
                            'id': 7
                        }
                    }],
                }),
            'm7.pb':
                M7_PROTO,
            'solo.pb':
                SOLO_PROTO,
        }),
        query='''
          SELECT
            (SELECT count(*) FROM slice) AS slices,
            (SELECT m.raw_id
             FROM slice s
             JOIN track t ON s.track_id = t.id
             JOIN machine m ON t.machine_id = m.id
             WHERE s.name = 'm7_slice') AS m7_machine;
        ''',
        out=Csv('''
        "slices","m7_machine"
        2,7
        '''))

  # A name-keyed override attributes the file to a machine labelled with that
  # name; trace_processor allocates the raw id.
  def test_machine_name_assignment(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [{
                        'path': 'app.json',
                        'machine': {
                            'name': 'phone'
                        }
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
            'spine.pb':
                SPINE,
        }),
        query='''
          SELECT s.name, m.name AS machine_name, s.ts
          FROM slice s
          JOIN thread_track tt ON s.track_id = tt.id
          JOIN thread t USING(utid)
          JOIN machine m ON t.machine_id = m.id
          WHERE s.name = 'json_slice';
        ''',
        out=Csv('''
        "name","machine_name","ts"
        "json_slice","phone",2000000
        '''))

  # Files sharing a name resolve to one machine; distinct names to distinct
  # machines.
  def test_machine_name_dedup(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'a.json',
                        'machine': {
                            'name': 'phone'
                        }
                    }, {
                        'path': 'b.json',
                        'machine': {
                            'name': 'phone'
                        }
                    }, {
                        'path': 'c.json',
                        'machine': {
                            'name': 'watch'
                        }
                    }],
                }),
            'a.json':
                _json_trace('slice_a'),
            'b.json':
                _json_trace('slice_b'),
            'c.json':
                _json_trace('slice_c'),
            'spine.pb':
                SPINE,
        }),
        query='''
          SELECT count(DISTINCT t.machine_id) AS machines
          FROM slice s
          JOIN thread_track tt ON s.track_id = tt.id
          JOIN thread t USING(utid)
          WHERE s.name IN ('slice_a', 'slice_b', 'slice_c');
        ''',
        out=Csv('''
        "machines"
        2
        '''))

  # Two separate single-machine captures combined into one multi-machine trace:
  # each is named and pinned onto REALTIME, so their timelines align on a
  # common absolute axis with no remote_clock_sync.
  def test_machine_multi_machine_realtime_alignment(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'trace_time_clock':
                        'REALTIME',
                    'files': [{
                        'path': 'phone.json',
                        'machine': {
                            'name': 'phone'
                        },
                        'clocks': {
                            'ts': 0,
                            'is': {
                                'clock': 'REALTIME',
                                'ts': 1000000000000
                            },
                        },
                    }, {
                        'path': 'server.json',
                        'machine': {
                            'name': 'server'
                        },
                        'clocks': {
                            'ts': 0,
                            'is': {
                                'clock': 'REALTIME',
                                'ts': 2000000000000
                            },
                        },
                    }],
                }),
            'phone.json':
                _json_trace('phone_slice'),
            'server.json':
                _json_trace('server_slice'),
        }),
        query='''
          SELECT s.name, m.name AS machine, s.ts
          FROM slice s
          JOIN thread_track tt ON s.track_id = tt.id
          JOIN thread t USING(utid)
          JOIN machine m ON t.machine_id = m.id
          WHERE s.name IN ('phone_slice', 'server_slice')
          ORDER BY s.ts;
        ''',
        out=Csv('''
        "name","machine","ts"
        "phone_slice","phone",1000002000000
        "server_slice","server",2000002000000
        '''))

  # --- Machine override errors ---

  def test_error_machine_id_out_of_range(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'app.json',
                        'machine': {
                            'id': 4294967296
                        }
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_manifest: machine: id must be in [1, 4294967295]'))

  # A machine override on a proto trace containing packets from a remote
  # machine fails: the trace manages its own machine identity.
  def test_error_proto_multi_machine_machine_override(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [{
                        'path': 'spine.pb',
                        'machine': {
                            'id': 3
                        }
                    }],
                }),
            'spine.pb':
                TextProto(_PROTO_SLICE + _M7_PROCESS),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'machine override requires the trace to come from a single machine')
    )

  def test_error_machine_empty(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [{
                        'path': 'app.json',
                        'machine': {}
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_manifest: machine must have a name and/or an id'))

  # --- Cross-machine REALTIME alignment ---
  #
  # When several single-machine files share no clock sync, their per-machine
  # clocks are aligned via REALTIME if a path through it exists (REALTIME is the
  # same absolute wall clock everywhere), else assumed BOOTTIME-aligned.

  # Two protos on different machines, no remote sync. Same BOOTTIME reading but
  # wall clocks 1s apart: they align on REALTIME, so 'b' lands 1s after 'a' in
  # the (BOOTTIME) trace-time domain owned by the first machine.
  def test_realtime_two_machines(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.pb', 'server'),
                    ],
                }),
            'a.pb':
                _proto_rt('a_slice', 1, 111, 1000000000,
                          1700000001000000000, 1100000000),
            'b.pb':
                _proto_rt('b_slice', 2, 222, 1000000000,
                          1700000002000000000, 1100000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "a_slice",1100000000,"phone"
        "b_slice",2100000000,"server"
        '''))

  # Three machines, wall clocks 1s apart each: all align on REALTIME, evenly
  # spaced in trace time.
  def test_realtime_three_machines(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.pb', 'server'),
                        _machine_file('c.pb', 'watch'),
                    ],
                }),
            'a.pb':
                _proto_rt('a_slice', 1, 111, 1000000000,
                          1700000001000000000, 1100000000),
            'b.pb':
                _proto_rt('b_slice', 2, 222, 1000000000,
                          1700000002000000000, 1100000000),
            'c.pb':
                _proto_rt('c_slice', 3, 333, 1000000000,
                          1700000003000000000, 1100000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "a_slice",1100000000,"phone"
        "b_slice",2100000000,"server"
        "c_slice",3100000000,"watch"
        '''))

  # REALTIME wins over BOOTTIME: the two machines have very different BOOTTIME
  # bases but their slices are at the same wall-clock instant. Aligning on
  # REALTIME places both at the same trace ts (1_100_000_000); a BOOTTIME
  # overlay would instead put 'b' at 5_100_000_000.
  def test_realtime_beats_boottime(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.pb', 'server'),
                    ],
                }),
            'a.pb':
                _proto_rt('a_slice', 1, 111, 1000000000,
                          1700000001000000000, 1100000000),
            'b.pb':
                _proto_rt('b_slice', 2, 222, 5000000000,
                          1700000001000000000, 5100000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "a_slice",1100000000,"phone"
        "b_slice",1100000000,"server"
        '''))

  # A peer machine with no REALTIME (BOOTTIME only) cannot align on wall clock,
  # so it falls back to the same-domain BOOTTIME overlay with the trace time
  # clock (here BOOTTIME, owned by the first machine), keeping its raw
  # timestamp. The REALTIME machine is unaffected.
  def test_no_realtime_on_peer_falls_back(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.pb', 'server'),
                    ],
                }),
            'a.pb':
                _proto_rt('a_slice', 1, 111, 1000000000,
                          1700000001000000000, 1100000000),
            'b.pb':
                _proto_boot_snap('b_slice', 2, 222, 1000000000, 3000000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "a_slice",1100000000,"phone"
        "b_slice",3000000000,"server"
        '''))

  # When the trace-time machine itself has no REALTIME, there is no rendezvous
  # node reaching trace time, so even a peer that has REALTIME falls back to the
  # same-domain BOOTTIME overlay: its slice keeps its raw BOOTTIME,
  # 1_100_000_000, rather than the 2_100_000_000 a REALTIME alignment would
  # give.
  def test_no_realtime_on_trace_time_machine_falls_back(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.pb', 'server'),
                    ],
                }),
            'a.pb':
                _proto_boot_snap('a_slice', 1, 111, 1000000000, 1100000000),
            'b.pb':
                _proto_rt('b_slice', 2, 222, 1000000000,
                          1700000002000000000, 1100000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "a_slice",1100000000,"phone"
        "b_slice",1100000000,"server"
        '''))

  # Non-proto: a JSON file (REALTIME only, via a manifest anchor) on one machine
  # aligns to a proto's BOOTTIME trace time on another machine, routed through
  # REALTIME. The JSON event at wall clock 1_700_000_001_502_000_000 maps to the
  # proto machine's BOOTTIME 1_502_000_000.
  def test_realtime_json_aligns_to_proto_boottime(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.json', 'server',
                                      realtime=1700000001500000000),
                    ],
                }),
            'a.pb':
                _proto_rt('proto_slice', 1, 111, 1000000000,
                          1700000001000000000, 1100000000),
            'b.json':
                _json_trace('json_slice'),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "proto_slice",1100000000,"phone"
        "json_slice",1502000000,"server"
        '''))

  # Non-proto: two JSON machines (REALTIME anchors 1s apart) both align to a
  # proto's BOOTTIME trace time through REALTIME, landing 1s apart.
  def test_realtime_two_json_align_to_proto(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.json', 'server',
                                      realtime=1700000001500000000),
                        _machine_file('c.json', 'watch',
                                      realtime=1700000002500000000),
                    ],
                }),
            'a.pb':
                _proto_rt('proto_slice', 1, 111, 1000000000,
                          1700000001000000000, 1100000000),
            'b.json':
                _json_trace('server_slice'),
            'c.json':
                _json_trace('watch_slice'),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "proto_slice",1100000000,"phone"
        "server_slice",1502000000,"server"
        "watch_slice",2502000000,"watch"
        '''))

  # Different real clock domains are not assumed aligned. trace_time_clock is
  # REALTIME; a machine whose events are on a real BOOTTIME with no path to
  # REALTIME cannot be related to trace time, so its events are dropped rather
  # than misplaced. The REALTIME-anchored peer resolves normally.
  def test_cross_domain_clocks_not_assumed(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'trace_time_clock': 'REALTIME',
                    'files': [
                        _machine_file('a.json', 'phone',
                                      realtime=1700000001500000000),
                        _machine_file('b.pb', 'server'),
                    ],
                }),
            'a.json':
                _json_trace('phone_slice'),
            'b.pb':
                _proto_boot_snap('boot_slice', 1, 111, 1000000000, 1100000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "phone_slice",1700000001502000000,"phone"
        '''))

  # Dropping events because of unrelatable clock domains is surfaced by a
  # dedicated import log / stat (not just the generic no-path error), so the
  # cause is discoverable.
  def test_cross_domain_drop_is_logged(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'trace_time_clock': 'REALTIME',
                    'files': [
                        _machine_file('a.json', 'phone',
                                      realtime=1700000001500000000),
                        _machine_file('b.pb', 'server'),
                    ],
                }),
            'a.json':
                _json_trace('phone_slice'),
            'b.pb':
                _proto_boot_snap('boot_slice', 1, 111, 1000000000, 1100000000),
        }),
        query='''
          SELECT name, sum(value) AS value FROM stats
          WHERE name = 'clock_sync_unrelatable_clock_domains';
        ''',
        out=Csv('''
        "name","value"
        "clock_sync_unrelatable_clock_domains",1
        '''))

  # The same physical clock on different machines IS assumed aligned: two
  # machines with only BOOTTIME (no REALTIME) are taken to share a boot instant
  # and overlay on the BOOTTIME trace time.
  def test_same_domain_boottime_overlay(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.pb', 'server'),
                    ],
                }),
            'a.pb':
                _proto_boot_snap('a_slice', 1, 111, 1000000000, 1100000000),
            'b.pb':
                _proto_boot_snap('b_slice', 2, 222, 1000000000, 3000000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "a_slice",1100000000,"phone"
        "b_slice",3000000000,"server"
        '''))
