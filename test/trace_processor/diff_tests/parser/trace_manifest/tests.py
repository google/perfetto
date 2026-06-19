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
                            'anchor': {
                                'ts': 1000000,
                                'is': {
                                    'clock': 'BOOTTIME',
                                    'ts': 1500000000
                                },
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
                            'anchor': {
                                'ts': 0,
                                'is': {
                                    'utc': '2023-11-14T22:13:21.5Z'
                                },
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
                            'anchor': {
                                'ts': 0,
                                'is': {
                                    'utc': '2023-11-14T22:13:21.5Z'
                                },
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
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'sys.systrace',
                        'clocks': {
                            'native': 'BOOTTIME'
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
                            'native': 'BOOTTIME',
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
                            'anchor': {
                                'ts': 1000000000,
                                'is': {
                                    'clock': 'BOOTTIME',
                                    'ts': 5000000000
                                },
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

  def test_error_offset_and_anchor_exclusive(self):
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
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_manifest: offset_ns and anchor are mutually exclusive'))

  def test_error_native_and_anchor_exclusive(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'app.json',
                        'clocks': {
                            'native': 'BOOTTIME',
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
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_manifest: native and anchor are mutually exclusive'))

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
                            'anchor': {
                                'ts': 0,
                                'is': {
                                    'utc': '2026-13-40T99:00:00Z'
                                }
                            },
                        },
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError('perfetto_manifest: anchor: invalid utc timestamp'))

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
                TextProto(_PROTO_SLICE + '''
  packet {
    machine_id: 7
    process_tree { processes { pid: 30 ppid: 0 cmdline: "m7_proc" } }
  }
'''),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'clock overrides require the trace to come from a single machine'))
