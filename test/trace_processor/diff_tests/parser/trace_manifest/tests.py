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

# Packets from remote machine 7 with a BOOTTIME snapshot and a slice 'vm_slice'
# at BOOTTIME 50_100_000_000.
_M7_SLICE = '''
  packet { machine_id: 7
    clock_snapshot { clocks { clock_id: 6 timestamp: 50000000000 } } }
  packet { machine_id: 7 trusted_packet_sequence_id: 2
    track_descriptor { uuid: 77 } }
  packet { machine_id: 7 trusted_packet_sequence_id: 2 timestamp: 50100000000
    track_event { type: TYPE_SLICE_BEGIN track_uuid: 77 name: "vm_slice" } }
  packet { machine_id: 7 trusted_packet_sequence_id: 2 timestamp: 50200000000
    track_event { type: TYPE_SLICE_END track_uuid: 77 } }
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
      (boot, realtime, seq, uuid, seq, at, uuid, name, seq, at + 100000000,
       uuid))


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


# A perfetto_manifest entry attributing |path| to machine |name|.
def _machine_file(path, name):
  return {'path': path, 'machine': {'name': name}}


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

  # --- trace_time ---

  # Top-level trace_time.clock overrides the global trace time domain. The
  # proto spine's slice (BOOTTIME 1_100_000_000) is converted to REALTIME via
  # the snapshot.
  def test_trace_time_clock_realtime(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'trace_time': {
                        'clock': 'REALTIME'
                    },
                }),
            'spine.pb':
                SPINE,
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
            'meta.json':
                _meta({
                    'version': 1,
                    'trace_time': {
                        'clock': 'BOOTTIME'
                    },
                }),
            'app.json':
                _json_trace('json_slice'),
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
            'meta.json':
                _meta({
                    'version': 1,
                    'trace_time': {
                        'clock': 'BOOTIME'
                    }
                }),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError('perfetto_manifest: unknown clock name: BOOTIME'))

  # A perfetto_manifest file fed to trace_processor on its own is trivially
  # the first file of the trace, so it parses fine (and configures nothing).
  def test_standalone_config(self):
    return DiffTestBlueprint(
        trace=RawText(
            _meta({
                'version': 1,
                'trace_time': {
                    'clock': 'REALTIME'
                }
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

  # --- Baseline (no override) ---

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

  # --- Clock override errors ---

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
                            'sync_to': {
                                'file': 'app.json',
                                'clock': 'BOOTTIME'
                            }
                        }
                    }, {
                        'path': 'app.json'
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

  # A pinning clocks override on a proto trace which emits a ClockSnapshot
  # (proof of multiple clocks) fails when the snapshot is parsed.
  def test_error_proto_multi_clock(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'spine.pb',
                        'clocks': {
                            'sync_to': {
                                'file': 'app.json',
                                'clock': 'BOOTTIME'
                            }
                        }
                    }, {
                        'path': 'app.json'
                    }],
                }),
            'spine.pb':
                SPINE,
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'clock overrides require the trace to use a single clock'))

  # A pinning clocks override on a proto trace containing packets from a remote
  # machine (machine_id != 0) fails: the override applies to a single machine.
  def test_error_proto_multi_machine_clock_override(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'spine.pb',
                        'clocks': {
                            'sync_to': {
                                'file': 'app.json',
                                'clock': 'BOOTTIME'
                            }
                        }
                    }, {
                        'path': 'app.json'
                    }],
                }),
            'spine.pb':
                TextProto(_PROTO_SLICE + _M7_PROCESS),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'a `clocks` override applies only to a single-machine file'))

  # --- Machines ---

  # A JSON file and a proto's remapped machine that share a name land on one
  # machine: pid 30 is in both the proto's remapped machine 7 (named 'vm') and
  # the JSON trace, so it must resolve to a single thread on 'vm'.
  def test_machine_assignment_merges_with_proto(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [
                        {
                            'path': 'app.json',
                            'machine': {
                                'name': 'vm'
                            }
                        },
                        {
                            'path': 'spine.pb',
                            'machines': [{
                                'id': 7,
                                'name': 'vm'
                            }]
                        },
                    ],
                }),
            'app.json':
                _json_trace('m7_json_slice', pid=30),
            'spine.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE + _M7_PROCESS),
        }),
        query='''
          SELECT
            (SELECT count(*) FROM thread WHERE tid = 30) AS threads,
            (SELECT m.name
             FROM slice s
             JOIN thread_track tt ON s.track_id = tt.id
             JOIN thread t USING(utid)
             JOIN machine m ON t.machine_id = m.id
             WHERE s.name = 'm7_json_slice') AS slice_machine;
        ''',
        out=Csv('''
        "threads","slice_machine"
        1,"vm"
        '''))

  # Two separate captures merged onto two different embedded machines of one
  # multi-machine proto: the host json lands on 'host' (embedded 0) and the vm
  # json on 'vm' (embedded 7). This is the host-trace-plus-per-VM-capture flow.
  def test_machines_two_siblings_merge_distinct(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [
                        {
                            'path': 'host.json',
                            'machine': {
                                'name': 'host'
                            }
                        },
                        {
                            'path': 'vm.json',
                            'machine': {
                                'name': 'vm'
                            }
                        },
                        {
                            'path':
                                'spine.pb',
                            'machines': [{
                                'id': 0,
                                'name': 'host'
                            }, {
                                'id': 7,
                                'name': 'vm'
                            }]
                        },
                    ],
                }),
            'host.json':
                _json_trace('host_json', pid=100),
            'vm.json':
                _json_trace('vm_json', pid=30),
            'spine.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE + _M7_PROCESS),
        }),
        query='''
          SELECT s.name, m.name AS machine
          FROM slice s
          JOIN thread_track tt ON s.track_id = tt.id
          JOIN thread t USING(utid)
          JOIN machine m ON t.machine_id = m.id
          WHERE s.name IN ('host_json', 'vm_json')
          ORDER BY s.name;
        ''',
        out=Csv('''
        "name","machine"
        "host_json","host"
        "vm_json","vm"
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
                            'name': 'vm'
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
            (SELECT m.name
             FROM slice s
             JOIN track t ON s.track_id = t.id
             JOIN machine m ON t.machine_id = m.id
             WHERE s.name = 'm7_slice') AS m7_machine;
        ''',
        out=Csv('''
        "slices","m7_machine"
        2,"vm"
        '''))

  # A name-keyed override attributes the file to a machine labelled with that
  # name; trace_processor allocates the raw id.
  def test_machine_name_assignment(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
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

  # --- Machine override errors ---

  def test_error_machines_id_out_of_range(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'app.json',
                        'machines': [{
                            'id': 4294967296,
                            'name': 'vm'
                        }]
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_manifest: machines: id must be in [0, 4294967295]'))

  # A single-machine `machine` override on a proto trace with packets from a
  # remote machine fails: it claimed one machine but is actually multi-machine
  # (use `machines` to remap them instead).
  def test_error_proto_multi_machine_machine_override(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [{
                        'path': 'spine.pb',
                        'machine': {
                            'name': 'vm'
                        }
                    }],
                }),
            'spine.pb':
                TextProto(_PROTO_SLICE + _M7_PROCESS),
        }),
        query='SELECT 1;',
        out=ExpectedError('Replace `machine` with a `machines` block'))

  def test_error_machine_empty(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'files': [{
                        'path': 'app.json',
                        'machine': {
                            'name': ''
                        }
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError('perfetto_manifest: machine: name must be non-empty'))

  def test_error_machine_and_machines_exclusive(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'app.json',
                        'machine': {
                            'name': 'a'
                        },
                        'machines': [{
                            'id': 0,
                            'name': 'b'
                        }]
                    }],
                }),
            'app.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_manifest: machine and machines are mutually exclusive'))

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
                    'version':
                        1,
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.pb', 'server'),
                    ],
                }),
            'a.pb':
                _proto_rt('a_slice', 1, 111, 1000000000, 1700000001000000000,
                          1100000000),
            'b.pb':
                _proto_rt('b_slice', 2, 222, 1000000000, 1700000002000000000,
                          1100000000),
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
                    'version':
                        1,
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.pb', 'server'),
                        _machine_file('c.pb', 'watch'),
                    ],
                }),
            'a.pb':
                _proto_rt('a_slice', 1, 111, 1000000000, 1700000001000000000,
                          1100000000),
            'b.pb':
                _proto_rt('b_slice', 2, 222, 1000000000, 1700000002000000000,
                          1100000000),
            'c.pb':
                _proto_rt('c_slice', 3, 333, 1000000000, 1700000003000000000,
                          1100000000),
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
                    'version':
                        1,
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.pb', 'server'),
                    ],
                }),
            'a.pb':
                _proto_rt('a_slice', 1, 111, 1000000000, 1700000001000000000,
                          1100000000),
            'b.pb':
                _proto_rt('b_slice', 2, 222, 5000000000, 1700000001000000000,
                          5100000000),
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
                    'version':
                        1,
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.pb', 'server'),
                    ],
                }),
            'a.pb':
                _proto_rt('a_slice', 1, 111, 1000000000, 1700000001000000000,
                          1100000000),
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
                    'version':
                        1,
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.pb', 'server'),
                    ],
                }),
            'a.pb':
                _proto_boot_snap('a_slice', 1, 111, 1000000000, 1100000000),
            'b.pb':
                _proto_rt('b_slice', 2, 222, 1000000000, 1700000002000000000,
                          1100000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "a_slice",1100000000,"phone"
        "b_slice",1100000000,"server"
        '''))

  # Different real clock domains are not assumed aligned. trace_time_clock is
  # REALTIME; a machine whose events are on a real BOOTTIME with no path to
  # REALTIME cannot be related to trace time, so its events are dropped rather
  # than misplaced. The REALTIME peer (a proto with a REALTIME snapshot)
  # resolves normally.
  def test_cross_domain_clocks_not_assumed(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'trace_time': {
                        'clock': 'REALTIME'
                    },
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.pb', 'server'),
                    ],
                }),
            'a.pb':
                _proto_rt('phone_slice', 1, 111, 1000000000,
                          1700000001500000000, 1100000000),
            'b.pb':
                _proto_boot_snap('boot_slice', 2, 222, 1000000000, 1100000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "phone_slice",1700000001600000000,"phone"
        '''))

  # Dropping events because of unrelatable clock domains is surfaced by a
  # dedicated import log / stat (not just the generic no-path error), so the
  # cause is discoverable.
  def test_cross_domain_drop_is_logged(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'trace_time': {
                        'clock': 'REALTIME'
                    },
                    'files': [
                        _machine_file('a.pb', 'phone'),
                        _machine_file('b.pb', 'server'),
                    ],
                }),
            'a.pb':
                _proto_rt('phone_slice', 1, 111, 1000000000,
                          1700000001500000000, 1100000000),
            'b.pb':
                _proto_boot_snap('boot_slice', 2, 222, 1000000000, 1100000000),
        }),
        query='''
          SELECT name, sum(value) AS value FROM stats
          WHERE name = 'clock_sync_unrelatable_clock_domains';
        ''',
        out=Csv('''
        "name","value"
        "clock_sync_unrelatable_clock_domains",1
        '''))

  # --- File-to-file clock sync (clocks.sync_to.file) ---

  # An explicit cross-file offset: server.pb's BOOTTIME = phone.pb's BOOTTIME +
  # 500ns, stated on server's own clocks block via sync_to.file. phone owns
  # trace time, so server's slice lands 500ns after phone's.
  def test_sync_to_file_offset(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [
                        {
                            'path': 'phone.pb',
                            'machine': {
                                'name': 'phone'
                            }
                        },
                        {
                            'path': 'server.pb',
                            'machine': {
                                'name': 'server'
                            },
                            'clocks': {
                                'clock': 'BOOTTIME',
                                'offset_ns': 500,
                                'sync_to': {
                                    'file': 'phone.pb',
                                    'clock': 'BOOTTIME'
                                }
                            }
                        },
                    ],
                }),
            'phone.pb':
                _proto_boot_snap('phone_slice', 1, 111, 1000000000, 1100000000),
            'server.pb':
                _proto_boot_snap('server_slice', 2, 222, 1000000000,
                                 1100000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "phone_slice",1100000000,"phone"
        "server_slice",1100000500,"server"
        '''))

  # sync_to.machine names the reference machine within sync_to.file. Here
  # phone.pb is single-machine, so its only machine is 'phone'.
  def test_sync_to_machine_offset(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [
                        {
                            'path': 'phone.pb',
                            'machine': {
                                'name': 'phone'
                            }
                        },
                        {
                            'path': 'server.pb',
                            'machine': {
                                'name': 'server'
                            },
                            'clocks': {
                                'clock': 'BOOTTIME',
                                'offset_ns': 500,
                                'sync_to': {
                                    'file': 'phone.pb',
                                    'machine': 'phone',
                                    'clock': 'BOOTTIME'
                                }
                            }
                        },
                    ],
                }),
            'phone.pb':
                _proto_boot_snap('phone_slice', 1, 111, 1000000000, 1100000000),
            'server.pb':
                _proto_boot_snap('server_slice', 2, 222, 1000000000,
                                 1100000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "phone_slice",1100000000,"phone"
        "server_slice",1100000500,"server"
        '''))

  # sync_to.file naming an undeclared file is rejected.
  def test_error_sync_to_file_unknown(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'a.pb',
                        'machine': {
                            'name': 'phone'
                        },
                        'clocks': {
                            'clock': 'BOOTTIME',
                            'offset_ns': 1,
                            'sync_to': {
                                'file': 'ghost.pb',
                                'clock': 'BOOTTIME'
                            }
                        }
                    },],
                }),
            'a.pb':
                _proto_boot_snap('a_slice', 1, 111, 1000000000, 1100000000),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            "perfetto_manifest: clocks: sync_to.file names unknown file "
            "'ghost.pb'"))

  # trace_time.file names which file's clock is the global trace time. This is
  # the same setup as test_sync_to_file_offset (server = phone + 500), but
  # trace time
  # is now server's BOOTTIME instead of the first file's, so it is server's
  # slice that keeps its raw ts and phone's that the edge shifts back by 500.
  def test_trace_time_file(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'trace_time': {
                        'file': 'server.pb',
                        'clock': 'BOOTTIME'
                    },
                    'files': [
                        {
                            'path': 'phone.pb',
                            'machine': {
                                'name': 'phone'
                            }
                        },
                        {
                            'path': 'server.pb',
                            'machine': {
                                'name': 'server'
                            },
                            'clocks': {
                                'clock': 'BOOTTIME',
                                'offset_ns': 500,
                                'sync_to': {
                                    'file': 'phone.pb',
                                    'clock': 'BOOTTIME'
                                }
                            }
                        },
                    ],
                }),
            'phone.pb':
                _proto_boot_snap('phone_slice', 1, 111, 1000000000, 1100000000),
            'server.pb':
                _proto_boot_snap('server_slice', 2, 222, 1000000000,
                                 1100000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "phone_slice",1099999500,"phone"
        "server_slice",1100000000,"server"
        '''))

  # trace_time.file naming an undeclared file is rejected.
  def test_error_trace_time_file_unknown(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version': 1,
                    'trace_time': {
                        'file': 'ghost.pb',
                        'clock': 'BOOTTIME'
                    },
                    'files': [{
                        'path': 'a.json'
                    }],
                }),
            'a.json':
                _json_trace('json_slice'),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            "perfetto_manifest: trace_time: file names unknown file 'ghost.pb'")
    )

  # The same physical clock on different machines IS assumed aligned: two
  # machines with only BOOTTIME (no REALTIME) are taken to share a boot instant
  # and overlay on the BOOTTIME trace time.
  def test_same_domain_boottime_overlay(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
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

  # --- Multi-machine remap (machines) ---

  # A `machines` block remaps a single multi-machine proto's embedded ids to
  # named machines: embedded 0 (host) -> phone, embedded 7 -> vm. Each slice is
  # attributed to its declared machine.
  def test_machines_remap(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path':
                            'multi.pb',
                        'machines': [{
                            'id': 0,
                            'name': 'phone'
                        }, {
                            'id': 7,
                            'name': 'vm'
                        }]
                    }],
                }),
            'multi.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE + _M7_SLICE),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "proto_slice",1100000000,"phone"
        "vm_slice",50100000000,"vm"
        '''))

  # The realistic multi-machine shape: the remote machine's clock is related to
  # the host's via a `remote_clock_sync` packet (not a standalone per-machine
  # snapshot). The `machines` remap must survive its own remote_clock_sync (it
  # passes CheckManifestSingleMachine only because the remap clears
  # has_machine_override) and fork machine 7 onto 'vm' before the sync is
  # parsed, so vm_slice is shifted onto the host timeline by the synced offset
  # (client BOOTTIME 50s == host BOOTTIME 2s, so guest 50.1s -> host 2.1s).
  def test_machines_remote_clock_sync(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path':
                            'multi.pb',
                        'machines': [{
                            'id': 0,
                            'name': 'host'
                        }, {
                            'id': 7,
                            'name': 'vm'
                        }]
                    }],
                }),
            'multi.pb':
                TextProto('''
                  packet {
                    clock_snapshot {
                      clocks { clock_id: 6 timestamp: 1000000000 }
                    }
                  }
                  packet {
                    trusted_packet_sequence_id: 1
                    track_descriptor { uuid: 1 }
                  }
                  packet {
                    trusted_packet_sequence_id: 1 timestamp: 1100000000
                    timestamp_clock_id: 6
                    track_event {
                      type: TYPE_SLICE_BEGIN track_uuid: 1 name: "host_slice"
                    }
                  }
                  packet {
                    trusted_packet_sequence_id: 1 timestamp: 1200000000
                    timestamp_clock_id: 6
                    track_event { type: TYPE_SLICE_END track_uuid: 1 }
                  }
                  packet {
                    machine_id: 7
                    remote_clock_sync {
                      synced_clocks {
                        client_clocks {
                          clocks { clock_id: 6 timestamp: 50000000000 }
                        }
                        host_clocks {
                          clocks { clock_id: 6 timestamp: 2000000000 }
                        }
                      }
                      synced_clocks {
                        client_clocks {
                          clocks { clock_id: 6 timestamp: 50000000000 }
                        }
                        host_clocks {
                          clocks { clock_id: 6 timestamp: 2000000000 }
                        }
                      }
                    }
                  }
                  packet {
                    machine_id: 7 trusted_packet_sequence_id: 2
                    track_descriptor { uuid: 7 }
                  }
                  packet {
                    machine_id: 7 trusted_packet_sequence_id: 2
                    timestamp: 50100000000 timestamp_clock_id: 6
                    track_event {
                      type: TYPE_SLICE_BEGIN track_uuid: 7 name: "vm_slice"
                    }
                  }
                  packet {
                    machine_id: 7 trusted_packet_sequence_id: 2
                    timestamp: 50200000000 timestamp_clock_id: 6
                    track_event { type: TYPE_SLICE_END track_uuid: 7 }
                  }
                '''),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "host_slice",1100000000,"host"
        "vm_slice",2100000000,"vm"
        '''))

  # A packet from an embedded machine id the `machines` block does not declare
  # is an error.
  def test_error_machines_undeclared_id(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'multi.pb',
                        'machines': [{
                            'id': 0,
                            'name': 'phone'
                        }]
                    }],
                }),
            'multi.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE + _M7_SLICE),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            'perfetto_manifest: machines: trace has a packet from undeclared '
            'machine id 7'))

  # Embedded machine ids are scoped to their trace: the same embedded id 7 in
  # two different proto files maps to two distinct machines (pb and qb). Only an
  # explicitly shared name would merge them.
  def test_machines_embedded_ids_scoped_per_file(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [
                        {
                            'path':
                                'p.pb',
                            'machines': [{
                                'id': 0,
                                'name': 'pa'
                            }, {
                                'id': 7,
                                'name': 'pb'
                            }]
                        },
                        {
                            'path':
                                'q.pb',
                            'machines': [{
                                'id': 0,
                                'name': 'qa'
                            }, {
                                'id': 7,
                                'name': 'qb'
                            }]
                        },
                    ],
                }),
            'p.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE + _M7_SLICE),
            'q.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT +
                          _PROTO_SLICE.replace('proto_slice', 'q_slice') +
                          _M7_SLICE.replace('vm_slice', 'qvm_slice')),
        }),
        query='''
          SELECT s.name, m.name AS machine
          FROM slice s
          JOIN track t ON s.track_id = t.id
          JOIN machine m ON t.machine_id = m.id
          WHERE s.name GLOB '*_slice'
          ORDER BY s.name;
        ''',
        out=Csv('''
        "name","machine"
        "proto_slice","pa"
        "q_slice","qa"
        "qvm_slice","qb"
        "vm_slice","pb"
        '''))

  # Naming the same embedded id the same in two files merges them onto one
  # machine: the two embedded-7 machines resolve to the single machine 'shared'.
  def test_machines_shared_name_merges(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [
                        {
                            'path':
                                'p.pb',
                            'machines': [{
                                'id': 0,
                                'name': 'host_p'
                            }, {
                                'id': 7,
                                'name': 'shared'
                            }]
                        },
                        {
                            'path':
                                'q.pb',
                            'machines': [{
                                'id': 0,
                                'name': 'host_q'
                            }, {
                                'id': 7,
                                'name': 'shared'
                            }]
                        },
                    ],
                }),
            'p.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE + _M7_SLICE),
            'q.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT +
                          _PROTO_SLICE.replace('proto_slice', 'q_slice') +
                          _M7_SLICE.replace('vm_slice', 'qvm_slice')),
        }),
        query='''
          SELECT
            (SELECT count(*) FROM machine WHERE name = 'shared') AS shared,
            (SELECT count(DISTINCT t.machine_id)
             FROM slice s
             JOIN track t ON s.track_id = t.id
             JOIN machine m ON t.machine_id = m.id
             WHERE s.name IN ('vm_slice', 'qvm_slice')) AS distinct_machines;
        ''',
        out=Csv('''
        "shared","distinct_machines"
        1,1
        '''))

  # sync_to.file referencing a multi-machine file must also name the machine:
  # the file is several machines, so the file alone is ambiguous.
  def test_error_sync_to_file_multi_machine_needs_machine(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [
                        {
                            'path':
                                'multi.pb',
                            'machines': [{
                                'id': 0,
                                'name': 'phone'
                            }, {
                                'id': 7,
                                'name': 'vm'
                            }]
                        },
                        {
                            'path': 'server.pb',
                            'machine': {
                                'name': 'server'
                            },
                            'clocks': {
                                'clock': 'BOOTTIME',
                                'offset_ns': 500,
                                'sync_to': {
                                    'file': 'multi.pb',
                                    'clock': 'BOOTTIME'
                                }
                            }
                        },
                    ],
                }),
            'multi.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE + _M7_SLICE),
            'server.pb':
                _proto_boot_snap('server_slice', 3, 333, 1000000000,
                                 1100000000),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            "perfetto_manifest: clocks: sync_to.file 'multi.pb' is a "
            "multi-machine trace; also name the machine with clocks: "
            "sync_to.machine"))

  # A sync_to block with no file is rejected: a reference is always a file.
  def test_error_sync_to_without_file(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [{
                        'path': 'a.pb',
                        'machine': {
                            'name': 'phone'
                        },
                        'clocks': {
                            'clock': 'BOOTTIME',
                            'offset_ns': 1,
                            'sync_to': {
                                'machine': 'phone',
                                'clock': 'BOOTTIME'
                            }
                        }
                    },],
                }),
            'a.pb':
                _proto_boot_snap('a_slice', 1, 111, 1000000000, 1100000000),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            "perfetto_manifest: clocks: sync_to.file is required"))

  # sync_to.machine must name a machine the referenced file itself declares.
  def test_error_sync_to_machine_not_in_file(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [
                        {
                            'path':
                                'multi.pb',
                            'machines': [{
                                'id': 0,
                                'name': 'phone'
                            }, {
                                'id': 7,
                                'name': 'vm'
                            }]
                        },
                        {
                            'path': 'server.pb',
                            'machine': {
                                'name': 'server'
                            },
                            'clocks': {
                                'clock': 'BOOTTIME',
                                'offset_ns': 500,
                                'sync_to': {
                                    'file': 'multi.pb',
                                    'machine': 'ghost',
                                    'clock': 'BOOTTIME'
                                }
                            }
                        },
                    ],
                }),
            'multi.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE + _M7_SLICE),
            'server.pb':
                _proto_boot_snap('server_slice', 3, 333, 1000000000,
                                 1100000000),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            "perfetto_manifest: clocks: sync_to.machine 'ghost' is not a "
            "machine declared by file 'multi.pb'"))

  # Cross-file clock sync onto a specific machine inside a multi-machine file:
  # server's BOOTTIME = vm's BOOTTIME + 500, where vm is embedded id 7 of
  # multi.pb. server's slice aligns onto vm's timeline (~50s), not phone's (~1s).
  def test_sync_to_file_machine_offset(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'trace_time': {
                        'file': 'multi.pb',
                        'machine': 'vm',
                        'clock': 'BOOTTIME'
                    },
                    'files': [
                        {
                            'path':
                                'multi.pb',
                            'machines': [{
                                'id': 0,
                                'name': 'phone'
                            }, {
                                'id': 7,
                                'name': 'vm'
                            }]
                        },
                        {
                            'path': 'server.pb',
                            'machine': {
                                'name': 'server'
                            },
                            'clocks': {
                                'clock': 'BOOTTIME',
                                'offset_ns': 500,
                                'sync_to': {
                                    'file': 'multi.pb',
                                    'machine': 'vm',
                                    'clock': 'BOOTTIME'
                                }
                            }
                        },
                    ],
                }),
            'multi.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE + _M7_SLICE),
            'server.pb':
                _proto_boot_snap('server_slice', 3, 333, 50000000000,
                                 50100000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "proto_slice",1100000000,"phone"
        "vm_slice",50100000000,"vm"
        "server_slice",50100000500,"server"
        '''))

  # --- Manual: clockless source, cross-machine, optional clock ---

  # A clockless JSON on one machine relates to a proto's clock on another machine
  # at an offset: the JSON event (file ts 2_000_000) maps to phone's BOOTTIME
  # 2_000_000 + 500 = 2_000_500 in the (phone-owned) trace time.
  def test_clockless_manual_cross_machine(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [
                        {
                            'path': 'phone.pb',
                            'machine': {
                                'name': 'phone'
                            }
                        },
                        {
                            'path': 'app.json',
                            'machine': {
                                'name': 'server'
                            },
                            'clocks': {
                                'offset_ns': 500,
                                'sync_to': {
                                    'file': 'phone.pb',
                                    'clock': 'BOOTTIME'
                                }
                            }
                        },
                    ],
                }),
            'phone.pb':
                _proto_boot_snap('phone_slice', 1, 111, 1000000000, 1100000000),
            'app.json':
                _json_trace('json_slice'),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "json_slice",2000500,"server"
        "phone_slice",1100000000,"phone"
        '''))

  # sync_to.clock may be omitted to relate to the reference's own private
  # (clockless) timeline: b.json's events sit on a.json's timeline + 500. a.json,
  # the first file, owns trace time, so its slice keeps its identity ts.
  def test_sync_to_clock_omitted_clockless_ref(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [
                        {
                            'path': 'a.json'
                        },
                        {
                            'path': 'b.json',
                            'clocks': {
                                'offset_ns': 500,
                                'sync_to': {
                                    'file': 'a.json'
                                }
                            }
                        },
                    ],
                }),
            'a.json':
                _json_trace('a_slice', pid=10),
            'b.json':
                _json_trace('b_slice', pid=11),
        }),
        query='''
          SELECT name, ts FROM slice
          WHERE name IN ('a_slice', 'b_slice')
          ORDER BY name;
        ''',
        out=Csv('''
        "name","ts"
        "a_slice",2000000
        "b_slice",2000500
        '''))

  # A multi-machine source names which of its declared machines owns the related
  # clock: multi.pb's vm BOOTTIME = server.pb's BOOTTIME + 500. server owns trace
  # time, so vm's slice lands 500ns after server's.
  def test_multi_machine_source_clock(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'trace_time': {
                        'file': 'server.pb',
                        'clock': 'BOOTTIME'
                    },
                    'files': [
                        {
                            'path': 'multi.pb',
                            'machines': [{
                                'id': 0,
                                'name': 'phone'
                            }, {
                                'id': 7,
                                'name': 'vm'
                            }],
                            'clocks': {
                                'machine': 'vm',
                                'clock': 'BOOTTIME',
                                'offset_ns': 500,
                                'sync_to': {
                                    'file': 'server.pb',
                                    'clock': 'BOOTTIME'
                                }
                            }
                        },
                        {
                            'path': 'server.pb',
                            'machine': {
                                'name': 'server'
                            }
                        },
                    ],
                }),
            'multi.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE + _M7_SLICE),
            'server.pb':
                _proto_boot_snap('server_slice', 3, 333, 50000000000,
                                 50100000000),
        }),
        query=_ALIGN_QUERY,
        out=Csv('''
        "name","ts","machine"
        "proto_slice",1100000000,"phone"
        "server_slice",50100000000,"server"
        "vm_slice",50100000500,"vm"
        '''))

  # A multi-machine source must name which machine the related clock is on.
  def test_error_multi_machine_source_needs_machine(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [
                        {
                            'path': 'multi.pb',
                            'machines': [{
                                'id': 0,
                                'name': 'phone'
                            }, {
                                'id': 7,
                                'name': 'vm'
                            }],
                            'clocks': {
                                'clock': 'BOOTTIME',
                                'sync_to': {
                                    'file': 'server.pb',
                                    'clock': 'BOOTTIME'
                                }
                            }
                        },
                        {
                            'path': 'server.pb',
                            'machine': {
                                'name': 'server'
                            }
                        },
                    ],
                }),
            'multi.pb':
                TextProto(_SPINE_CLOCK_SNAPSHOT + _PROTO_SLICE + _M7_SLICE),
            'server.pb':
                _proto_boot_snap('server_slice', 3, 333, 50000000000,
                                 50100000000),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            "perfetto_manifest: clocks: file 'multi.pb' is a multi-machine "
            "trace; name which machine the clock is on with clocks: machine."))

  # The source `machine` must name a machine the file itself declares.
  def test_error_source_machine_unknown(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                _meta({
                    'version':
                        1,
                    'files': [
                        {
                            'path': 'a.pb',
                            'machine': {
                                'name': 'phone'
                            },
                            'clocks': {
                                'machine': 'ghost',
                                'clock': 'BOOTTIME',
                                'sync_to': {
                                    'file': 'b.pb',
                                    'clock': 'BOOTTIME'
                                }
                            }
                        },
                        {
                            'path': 'b.pb',
                            'machine': {
                                'name': 'srv'
                            }
                        },
                    ],
                }),
            'a.pb':
                _proto_boot_snap('a_slice', 1, 111, 1000000000, 1100000000),
            'b.pb':
                _proto_boot_snap('b_slice', 2, 222, 1000000000, 1100000000),
        }),
        query='SELECT 1;',
        out=ExpectedError(
            "perfetto_manifest: clocks: machine 'ghost' is not a machine "
            "declared by file 'a.pb'."))
