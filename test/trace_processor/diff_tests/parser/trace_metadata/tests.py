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


def _meta(payload):
  return json.dumps({'perfetto_manifest': payload})


class TraceMetadata(TestSuite):
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
