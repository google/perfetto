#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

import zlib

from python.generators.diff_tests.testing import Csv, Path, DataPath
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import TextProto
from python.generators.diff_tests.testing import Tar
from python.generators.diff_tests.testing import Zip as ZipTrace

# An in-process SDK trace: MONOTONIC (clock id 3) packets and no ClockSnapshot,
# so its timestamps can only be converted through a sibling trace's snapshots.
_SDK_NO_SNAPSHOT = TextProto('''
  packet {
    trusted_packet_sequence_id: 1
    timestamp: 900
    timestamp_clock_id: 3
    track_descriptor { uuid: 1 name: "sdk_track" }
  }
  packet {
    trusted_packet_sequence_id: 1
    timestamp: 1000
    timestamp_clock_id: 3
    track_event { track_uuid: 1 type: TYPE_INSTANT name: "sdk" }
  }
''')

# A system trace's ClockSnapshot: MONOTONIC 500 is BOOTTIME 5000.
_SYSTEM_SNAPSHOT = '''
  packet {
    timestamp: 5000
    clock_snapshot {
      clocks { clock_id: 6 timestamp: 5000 }
      clocks { clock_id: 3 timestamp: 500 }
    }
  }
'''

# A system trace's own events (BOOTTIME).
_SYSTEM_EVENTS = '''
  packet {
    trusted_packet_sequence_id: 2
    timestamp: 5900
    track_descriptor { uuid: 2 name: "sys_track" }
  }
  packet {
    trusted_packet_sequence_id: 2
    timestamp: 6000
    track_event { track_uuid: 2 type: TYPE_INSTANT name: "sys" }
  }
'''

_SYSTEM_WITH_SNAPSHOT = TextProto(_SYSTEM_SNAPSHOT + _SYSTEM_EVENTS)

# Packets which carry nothing, to move what follows a few hundred bytes in.
_PADDING = 'packet { trusted_packet_sequence_id: 3 }\n' * 200


# A packet holding `trace` (serialized packets) as deflated compressed_packets.
# Level 0 stores `trace` as is, which keeps the packet as big as `trace`.
def _compressed_packets(trace, level=-1):
  escaped = ''.join('\\%03o' % b for b in zlib.compress(trace, level))
  return 'packet { compressed_packets: "%s" }' % escaped


class Zip(TestSuite):

  def test_perf_proto_sym(self):
    return DiffTestBlueprint(
        trace=DataPath('zip/perf_track_sym.zip'),
        query=Path('../simpleperf/stacks_test.sql'),
        out=Csv('''
        "name"
        "main,A"
        "main,A,B"
        "main,A,B,C"
        "main,A,B,C,D"
        "main,A,B,C,D,E"
        "main,A,B,C,E"
        "main,A,B,D"
        "main,A,B,D,E"
        "main,A,B,E"
        "main,A,C"
        "main,A,C,D"
        "main,A,C,D,E"
        "main,A,C,E"
        "main,A,D"
        "main,A,D,E"
        "main,A,E"
        "main,B"
        "main,B,C"
        "main,B,C,D"
        "main,B,C,D,E"
        "main,B,C,E"
        "main,B,D"
        "main,B,D,E"
        "main,B,E"
        "main,C"
        "main,C,D"
        "main,C,D,E"
        "main,C,E"
        "main,D"
        "main,D,E"
        "main,E"
        '''))

  def test_zip_tokenization_order(self):
    return DiffTestBlueprint(
        trace=DataPath('zip/perf_track_sym.zip'),
        query='''
          SELECT id, parent_id, name, size, trace_type, processing_order
          FROM __intrinsic_trace_file
          ORDER BY processing_order
        ''',
        out=Csv('''
        "id","parent_id","name","size","trace_type","processing_order"
        0,"[NULL]","[NULL]",94651,"zip",0
        3,0,"c.trace.pb",379760,"proto",1
        1,0,"b.simpleperf.data",554911,"perf",2
        2,0,"a.symbols.pb",186149,"symbols",3
        '''))

  def test_tar_gz_tokenization_order(self):
    return DiffTestBlueprint(
        trace=DataPath('perf_track_sym.tar.gz'),
        query='''
          SELECT id, parent_id, name, size, trace_type, processing_order
          FROM __intrinsic_trace_file
          ORDER BY processing_order
        ''',
        out=Csv('''
        "id","parent_id","name","size","trace_type","processing_order"
        0,"[NULL]","[NULL]",94091,"gzip",0
        1,0,"",1126400,"tar",1
        4,1,"c.trace.pb",379760,"proto",2
        3,1,"b.simpleperf.data",554911,"perf",3
        2,1,"a.symbols.pb",186149,"symbols",4
        '''))

  # Make sure the logcat timestamps are correctly converted to trace ts. All
  # logcat events in the trace were emitted while a perfetto trace collection
  # was active. Thus their timestamps should be between the min and max ts of
  # all track events.
  # The device where the trace was collected had a timezone setting of UTC+1
  def test_logcat_and_proto(self):
    return DiffTestBlueprint(
        trace=DataPath('zip/logcat_and_proto.zip'),
        query='''
        WITH
          INTERVAL AS (
            SELECT
              (SELECT MIN(ts) FROM slice) AS min_ts,
              (SELECT MAX(ts) FROM slice) AS max_ts
          )
        SELECT COUNT(*) AS count
        FROM android_logs, INTERVAL
        WHERE ts BETWEEN min_ts AND max_ts;
        ''',
        out=Csv('''
        "count"
        58
        '''))

  # A zip assembled inline from blueprint members: a textproto-defined proto
  # trace and a raw-text systrace. Proto members are processed first.
  def test_zip_blueprint_inline_members(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'a.systrace':
                '''# tracer: nop
#
  app-100 (  100) [001] ...1  1.000000: tracing_mark_write: B|100|zip_slice
  app-100 (  100) [001] ...1  1.500000: tracing_mark_write: E|100
''',
            'b.pb':
                TextProto('''
              packet {
                timestamp: 1
                process_tree {
                  processes { pid: 5 ppid: 0 cmdline: "proc_in_zip" }
                }
              }
            '''),
        }),
        query='''
          SELECT name, trace_type, processing_order
          FROM __intrinsic_trace_file
          ORDER BY processing_order;
        ''',
        out=Csv('''
        "name","trace_type","processing_order"
        "[NULL]","zip",0
        "b.pb","proto",1
        "a.systrace","systrace",2
        '''))

  # Systrace timestamps must be written back after clock conversion: inside a
  # zip with a proto trace providing a MONOTONIC<->BOOTTIME snapshot the
  # conversion is not the identity, and the value written to tables must be
  # the converted one (which is also the sorting key). The 1.0s MONOTONIC
  # slice lands at BOOTTIME 1_000_000_000 + 500_000_000.
  def test_zip_systrace_converted_timestamps(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'sys.systrace':
                '''# tracer: nop
#
  app-100 (  100) [001] ...1  1.000000: tracing_mark_write: B|100|sys_slice
  app-100 (  100) [001] ...1  1.500000: tracing_mark_write: E|100
''',
            'spine.pb':
                TextProto('''
              packet {
                clock_snapshot {
                  clocks { clock_id: 6 timestamp: 1000000000 }
                  clocks { clock_id: 3 timestamp: 500000000 }
                }
              }
            '''),
        }),
        query='''
          SELECT name, ts, dur FROM slice WHERE name = 'sys_slice';
        ''',
        out=Csv('''
        "name","ts","dur"
        "sys_slice",1500000000,500000000
        '''))

  # A tar archive with an external file (DataPath) as a member: the raw bytes
  # of the checked-in trace are included verbatim.
  def test_tar_blueprint_external_member(self):
    return DiffTestBlueprint(
        trace=Tar({
            'sched.pb':
                DataPath('synth_1.pb'),
            'log.systrace':
                '''# tracer: nop
#
  app-100 (  100) [001] ...1  1.000000: tracing_mark_write: B|100|tar_slice
  app-100 (  100) [001] ...1  1.500000: tracing_mark_write: E|100
''',
        }),
        query='''
          SELECT
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT count(*) FROM __intrinsic_trace_file
             WHERE name = 'sched.pb') AS named_member;
        ''',
        out=Csv('''
        "sched_count","named_member"
        8,1
        '''))

  # A tar archive as produced by macOS/BSD `tar`: PAX extended headers,
  # space-terminated numeric fields, and AppleDouble ("._foo") / ".DS_Store"
  # metadata files sprinkled next to the real trace. The real trace must load
  # and every hidden (dot-prefixed) entry must be ignored, including one nested
  # under a subdirectory.
  def test_macos_tar(self):
    return DiffTestBlueprint(
        trace=Tar(
            {
                'sched.pb': DataPath('synth_1.pb'),
                '._sched.pb': b'Mac OS X AppleDouble junk\x00\x01\x02',
                '.DS_Store': b'Bud1\x00\x00\x00\x00junk',
                'sub/._nested.pb': b'more junk',
            },
            macos_style=True,
        ),
        query='''
          SELECT
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT count(*) FROM __intrinsic_trace_file) AS file_count,
            (SELECT count(*) FROM __intrinsic_trace_file
             WHERE name GLOB '*._*' OR name GLOB '*.DS_Store') AS hidden_count;
        ''',
        out=Csv('''
        "sched_count","file_count","hidden_count"
        8,2,0
        '''))

  def test_multi_trace_single_machine_clock(self):
    return DiffTestBlueprint(
        trace=DataPath('multi_trace_single_machine_clock.zip'),
        query='''
        SELECT ts
        FROM slice
        WHERE name = 'InterruptibleSleep::run_with_interval';
        ''',
        out=Csv('''
        "ts"
        1276407306585477
        1276408471040116
        '''))

  # The second trace's primary_trace_clock (MONOTONIC) is related only to its
  # own BOOTTIME, and the first trace never mentions MONOTONIC. The second
  # trace must reach trace time through its BOOTTIME.
  def test_zip_primary_clock_related_to_boottime_by_own_snapshot(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'a.pb':
                TextProto('''
              packet {
                clock_snapshot {
                  clocks { clock_id: 6 timestamp: 10000 }  # BOOTTIME
                }
              }
            '''),
            'b.pb':
                TextProto('''
              packet {
                clock_snapshot {
                  primary_trace_clock: 3  # BUILTIN_CLOCK_MONOTONIC
                  clocks { clock_id: 3 timestamp: 1000 }  # MONOTONIC
                  clocks { clock_id: 6 timestamp: 5000 }  # BOOTTIME
                }
              }
              packet {
                trusted_packet_sequence_id: 1
                timestamp: 2000
                incremental_state_cleared: true
                track_descriptor { uuid: 1 name: "t" }
              }
              packet {
                trusted_packet_sequence_id: 1
                timestamp: 2000
                track_event { type: TYPE_INSTANT track_uuid: 1 name: "ev" }
              }
            '''),
        }),
        query='''
          SELECT ts, name FROM slice;
        ''',
        out=Csv('''
        "ts","name"
        6000,"ev"
        '''))

  # As above, but the first trace makes MONOTONIC the trace time clock. The
  # second trace's MONOTONIC reaches it directly, so it is bridged rather
  # than BOOTTIME.
  def test_zip_primary_clock_related_to_boottime_trace_time_monotonic(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'a.pb':
                TextProto('''
              packet {
                clock_snapshot {
                  primary_trace_clock: 3  # BUILTIN_CLOCK_MONOTONIC
                  clocks { clock_id: 3 timestamp: 10000 }  # MONOTONIC
                }
              }
            '''),
            'b.pb':
                TextProto('''
              packet {
                clock_snapshot {
                  primary_trace_clock: 3  # BUILTIN_CLOCK_MONOTONIC
                  clocks { clock_id: 3 timestamp: 1000 }  # MONOTONIC
                  clocks { clock_id: 6 timestamp: 5000 }  # BOOTTIME
                }
              }
              packet {
                trusted_packet_sequence_id: 1
                timestamp: 2000
                incremental_state_cleared: true
                track_descriptor { uuid: 1 name: "t" }
              }
              packet {
                trusted_packet_sequence_id: 1
                timestamp: 2000
                track_event { type: TYPE_INSTANT track_uuid: 1 name: "ev" }
              }
            '''),
        }),
        query='''
          SELECT ts, name FROM slice;
        ''',
        out=Csv('''
        "ts","name"
        2000,"ev"
        '''))

  # A system trace relates MONOTONIC to BOOTTIME; the second trace's
  # snapshot only has its primary_trace_clock (MONOTONIC). The second trace
  # must reach trace time through the system trace's MONOTONIC.
  def test_zip_primary_clock_without_boottime_uses_shared_clock(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'a.pb':
                TextProto('''
              packet {
                clock_snapshot {
                  clocks { clock_id: 6 timestamp: 10000 }  # BOOTTIME
                  clocks { clock_id: 3 timestamp: 1000 }  # MONOTONIC
                }
              }
            '''),
            'b.pb':
                TextProto('''
              packet {
                clock_snapshot {
                  primary_trace_clock: 3  # BUILTIN_CLOCK_MONOTONIC
                  clocks { clock_id: 3 timestamp: 1000 }  # MONOTONIC
                }
              }
              packet {
                trusted_packet_sequence_id: 1
                timestamp: 2000
                incremental_state_cleared: true
                track_descriptor { uuid: 1 name: "t" }
              }
              packet {
                trusted_packet_sequence_id: 1
                timestamp: 2000
                track_event { type: TYPE_INSTANT track_uuid: 1 name: "ev" }
              }
            '''),
        }),
        query='''
          SELECT ts, name FROM slice;
        ''',
        out=Csv('''
        "ts","name"
        11000,"ev"
        '''))

  # a_sdk.pb sorts first by name, but b_system.pb carries the ClockSnapshot, so
  # it is read first and the sdk slice is converted through it: it lands at
  # BOOTTIME 5000 + (1000 - 500). Read in name order, it would be dropped.
  def test_zip_proto_with_clock_snapshot_read_first(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'a_sdk.pb': _SDK_NO_SNAPSHOT,
            'b_system.pb': _SYSTEM_WITH_SNAPSHOT,
        }),
        query='''
          SELECT name, ts FROM slice ORDER BY ts;
        ''',
        out=Csv('''
        "name","ts"
        "sdk",5500
        "sys",6000
        '''))

  # Same in a tar archive. The ClockSnapshot comes a few hundred bytes in, so
  # tar must hand over more than the first bytes of each member to be sniffed.
  def test_tar_proto_with_clock_snapshot_read_first(self):
    return DiffTestBlueprint(
        trace=Tar({
            'a_sdk.pb':
                _SDK_NO_SNAPSHOT,
            'b_system.pb':
                TextProto(_PADDING + _SYSTEM_SNAPSHOT + _SYSTEM_EVENTS),
        }),
        query='''
          SELECT name, ts FROM slice ORDER BY ts;
        ''',
        out=Csv('''
        "name","ts"
        "sdk",5500
        "sys",6000
        '''))

  # The ClockSnapshot need not be in the first packet.
  def test_zip_proto_with_late_clock_snapshot_read_first(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'a_sdk.pb': _SDK_NO_SNAPSHOT,
            'b_system.pb': TextProto(_SYSTEM_EVENTS + _SYSTEM_SNAPSHOT),
        }),
        query='''
          SELECT name, processing_order
          FROM __intrinsic_trace_file
          WHERE name IS NOT NULL
          ORDER BY processing_order;
        ''',
        out=Csv('''
        "name","processing_order"
        "b_system.pb",1
        "a_sdk.pb",2
        '''))

  # Compressed packets are not inflated to look for a ClockSnapshot: a trace
  # holding some counts as carrying one (traced versions from before September
  # 2026 compressed their snapshots too), so it is read before a trace without
  # any. This holds even when the compressed packet runs past the part of the
  # file looked at, as traced compresses whole batches of packets into one.
  def test_zip_proto_with_compressed_packets_read_first(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'a_plain.pb':
                TextProto('packet { trusted_packet_sequence_id: 1 }'),
            # Holds 20000 x packet { trusted_packet_sequence_id: 2 }, stored
            # as is: a single packet of about 80 KB.
            'b_compressed.pb':
                TextProto(
                    _compressed_packets(b'\x0a\x02\x50\x02' * 20000, level=0)),
        }),
        query='''
          SELECT name, processing_order
          FROM __intrinsic_trace_file
          WHERE name IS NOT NULL
          ORDER BY processing_order;
        ''',
        out=Csv('''
        "name","processing_order"
        "b_compressed.pb",1
        "a_plain.pb",2
        '''))
