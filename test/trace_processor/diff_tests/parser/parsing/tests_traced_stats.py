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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ParsingTracedStats(TestSuite):
  # Check that `previous_packed_dropped: true` maps to
  # `traced_buf_sequence_packet_loss`.
  def test_sequence_packet_loss(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 2
          previous_packet_dropped: 1
        }
        packet {
          trusted_packet_sequence_id: 2
        }
        packet {
          trusted_packet_sequence_id: 2
        }
        packet {
          trusted_packet_sequence_id: 3
          previous_packet_dropped: 1
        }
        packet {
          trusted_packet_sequence_id: 3
          previous_packet_dropped: 1
        }
        packet {
          trusted_packet_sequence_id: 3
        }
        packet {
          trusted_packet_sequence_id: 4
          previous_packet_dropped: 1
        }
        packet {
          trusted_packet_sequence_id: 4
          previous_packet_dropped: 1
        }
        packet {
          trusted_packet_sequence_id: 4
        }
        packet {
          trusted_packet_sequence_id: 5
          previous_packet_dropped: 1
        }
        packet {
          trusted_packet_sequence_id: 5
          previous_packet_dropped: 1
        }
        packet {
          trusted_packet_sequence_id: 5
        }
        packet {
          trusted_uid: 9999
          trusted_packet_sequence_id: 1
          trace_stats {
            writer_stats {
              sequence_id: 2
              buffer: 0
            }
            writer_stats {
              sequence_id: 3
              buffer: 1
            }
            writer_stats {
              sequence_id: 4
              buffer: 2
            }
            writer_stats {
              sequence_id: 5
              buffer: 2
            }
          }
        }
        """),
        query="""
          SELECT idx, value
          FROM stats
          WHERE name = 'traced_buf_sequence_packet_loss'
          ORDER BY idx;
        """,
        out=Csv("""
        "idx","value"
        0,0
        1,1
        2,2
        """))

  # Check that dropping all packets leads to
  # `traced_buf_incremental_sequences_dropped` being set.
  def test_sequence_all_incremental_dropped(self):
    return DiffTestBlueprint(
        trace=TextProto('''
        packet {
          trusted_packet_sequence_id: 2
          previous_packet_dropped: 1
          first_packet_on_sequence: true
          sequence_flags: 1  # SEQ_INCREMENTAL_STATE_CLEARED
        }
        packet {
          trusted_packet_sequence_id: 2
          sequence_flags: 2  # SEQ_NEEDS_INCREMENTAL_STATE
        }
        packet {
          trusted_packet_sequence_id: 2
          sequence_flags: 2  # SEQ_NEEDS_INCREMENTAL_STATE
        }
        packet {
          trusted_packet_sequence_id: 3
          sequence_flags: 2  # SEQ_NEEDS_INCREMENTAL_STATE
        }
        packet {
          trusted_packet_sequence_id: 3
          sequence_flags: 2  # SEQ_NEEDS_INCREMENTAL_STATE
        }
        packet {
          trusted_packet_sequence_id: 4
          sequence_flags: 2  # SEQ_NEEDS_INCREMENTAL_STATE
        }
        packet {
          trusted_uid: 9999
          trusted_packet_sequence_id: 1
          trace_stats {
            writer_stats {
              sequence_id: 2
              buffer: 0
            }
            writer_stats {
              sequence_id: 3
              buffer: 1
            }
            writer_stats {
              sequence_id: 4
              buffer: 1
            }
          }
        }
        '''),
        query='''
          SELECT idx, value
          FROM stats
          WHERE name = 'traced_buf_incremental_sequences_dropped'
          ORDER BY idx;
        ''',
        out=Csv('''
        "idx","value"
        0,0
        1,2
        '''))

  # previous_packet_dropped is a uint32 bitmask: any nonzero value counts as
  # one packet loss (backward-compatible with the old bool). The first packet
  # of each sequence is skipped (bootstrap sentinel).
  # Sequence 2 (buffer 0): two nonzero drops (values 3 and 65) → count = 2.
  # Sequence 3 (buffer 1): one nonzero drop (value 1, bare DATA_LOSS_PRESENT) → count = 1.
  def test_sequence_packet_loss_bitmask(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 2
        }
        packet {
          trusted_packet_sequence_id: 2
          previous_packet_dropped: 3
        }
        packet {
          trusted_packet_sequence_id: 2
          previous_packet_dropped: 65
        }
        packet {
          trusted_packet_sequence_id: 3
        }
        packet {
          trusted_packet_sequence_id: 3
          previous_packet_dropped: 1
        }
        packet {
          trusted_uid: 9999
          trusted_packet_sequence_id: 1
          trace_stats {
            writer_stats {
              sequence_id: 2
              buffer: 0
            }
            writer_stats {
              sequence_id: 3
              buffer: 1
            }
          }
        }
        """),
        query="""
          SELECT idx, value
          FROM stats
          WHERE name = 'traced_buf_sequence_packet_loss'
          ORDER BY idx;
        """,
        out=Csv("""
        "idx","value"
        0,2
        1,1
        """))

  # Each TraceBufferV2 cause bit in previous_packet_dropped is broken down into
  # its own traced_buf_data_loss_* stat per buffer. DATA_LOSS_PRESENT (bit 0) on
  # its own is not attributed (it's already covered by
  # traced_buf_sequence_packet_loss).
  # Values used (PRESENT is bit 0, always set on a loss):
  #   3   = PRESENT|READ_GAP        27 = PRESENT|READ_GAP|ORPHAN|REASSEMBLY_GAP
  #   5   = PRESENT|CHUNK_CORRUPTED 33 = PRESENT|REASSEMBLY_BROKEN_CHAIN
  #   65  = PRESENT|OVERWRITE       129 = PRESENT|WRITER_ABORT
  #   1   = PRESENT only (not attributed)
  def test_data_loss_cause_breakdown(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet { trusted_packet_sequence_id: 2 }
        packet { trusted_packet_sequence_id: 2 previous_packet_dropped: 3 }
        packet { trusted_packet_sequence_id: 2 previous_packet_dropped: 65 }
        packet { trusted_packet_sequence_id: 2 previous_packet_dropped: 27 }
        packet { trusted_packet_sequence_id: 2 previous_packet_dropped: 5 }
        packet { trusted_packet_sequence_id: 2 previous_packet_dropped: 33 }
        packet { trusted_packet_sequence_id: 2 previous_packet_dropped: 129 }
        packet { trusted_packet_sequence_id: 2 previous_packet_dropped: 1 }
        packet {
          trusted_uid: 9999
          trusted_packet_sequence_id: 1
          trace_stats {
            writer_stats { sequence_id: 2 buffer: 0 }
          }
        }
        """),
        query="""
          SELECT name, idx, value
          FROM stats
          WHERE name GLOB 'traced_buf_data_loss_*' AND value > 0
          ORDER BY name;
        """,
        out=Csv("""
        "name","idx","value"
        "traced_buf_data_loss_chunk_corrupted",0,1
        "traced_buf_data_loss_orphan_continuation",0,1
        "traced_buf_data_loss_overwrite",0,1
        "traced_buf_data_loss_read_gap",0,2
        "traced_buf_data_loss_reassembly_broken_chain",0,1
        "traced_buf_data_loss_reassembly_gap",0,1
        "traced_buf_data_loss_writer_abort",0,1
        """))
