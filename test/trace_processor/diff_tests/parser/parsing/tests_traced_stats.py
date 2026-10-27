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

  # Each cause bit in previous_packet_dropped is broken down into its own
  # traced_buf_data_loss_* stat per buffer. DATA_LOSS_PRESENT (bit 0) on its own
  # is not attributed (it's already covered by traced_buf_sequence_packet_loss).
  # Values used (PRESENT is bit 0, always set on a loss):
  #   3   = PRESENT|READ_GAP        27  = PRESENT|READ_GAP|ORPHAN|REASSEMBLY_GAP
  #   5   = PRESENT|CHUNK_CORRUPTED 33  = PRESENT|REASSEMBLY_BROKEN_CHAIN
  #   65  = PRESENT|OVERWRITE       129 = PRESENT|WRITER_ABORT
  #   257 = PRESENT|SMB_FULL        1   = PRESENT only (not attributed)
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
        packet { trusted_packet_sequence_id: 2 previous_packet_dropped: 257 }
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
        "traced_buf_data_loss_smb_full",0,1
        "traced_buf_data_loss_writer_abort",0,1
        """))

  # Check that overwrites in a `write_into_file` trace map to
  # `long_trace_mode_bytes_overwritten`, one row per affected buffer with the
  # bytes it overwrote. Buffer 1 overwrote nothing, so it gets no row.
  def test_long_trace_mode_bytes_overwritten(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            write_into_file: true
            file_write_period_ms: 5000
            buffers { size_kb: 1024 }
            buffers { size_kb: 1024 fill_policy: RING_BUFFER }
          }
        }
        packet {
          trusted_uid: 9999
          trusted_packet_sequence_id: 1
          trace_stats {
            buffer_stats { bytes_written: 4096 bytes_overwritten: 2048 }
            buffer_stats { bytes_written: 4096 bytes_overwritten: 0 }
          }
        }
        """),
        query="""
          SELECT idx, value
          FROM stats
          WHERE name = 'long_trace_mode_bytes_overwritten'
          ORDER BY idx;
        """,
        out=Csv("""
        "idx","value"
        0,2048
        """))

  # Check that a 7 day `file_write_period_ms` produces no stat: the client is
  # asking for ring-buffer behavior (see RecordLongTraceModeOverwrites).
  def test_long_trace_mode_bytes_overwritten_ring_buffer_intent(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            write_into_file: true
            file_write_period_ms: 604800000
            buffers { size_kb: 1024 }
          }
        }
        packet {
          trusted_uid: 9999
          trusted_packet_sequence_id: 1
          trace_stats {
            buffer_stats { bytes_written: 4096 bytes_overwritten: 2048 }
          }
        }
        """),
        query="""
          SELECT count(*) AS stat_rows
          FROM stats
          WHERE name = 'long_trace_mode_bytes_overwritten';
        """,
        out=Csv("""
        "stat_rows"
        0
        """))

  # Check that the TraceBufferV2 compaction counters of each buffer map to
  # `traced_buf_compactions` and `traced_buf_compactions_skipped`.
  def test_buffer_compaction_stats(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_uid: 9999
          trusted_packet_sequence_id: 1
          trace_stats {
            buffer_stats { compactions: 3 compactions_skipped: 1 }
            buffer_stats { compactions: 5 }
          }
        }
        """),
        query="""
          SELECT name, idx, value
          FROM stats
          WHERE name GLOB 'traced_buf_compactions*'
          ORDER BY name, idx;
        """,
        out=Csv("""
        "name","idx","value"
        "traced_buf_compactions",0,3
        "traced_buf_compactions",1,5
        "traced_buf_compactions_skipped",0,1
        "traced_buf_compactions_skipped",1,0
        """))

  # Check that each buffer counts its writers by the protocol that carried
  # their chunks.
  def test_buffer_protocol_writers(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_uid: 9999
          trusted_packet_sequence_id: 1
          trace_stats {
            writer_stats { sequence_id: 2 buffer: 0 protocol_abi_versions: 1 }
            writer_stats { sequence_id: 3 buffer: 0 protocol_abi_versions: 2 }
            writer_stats { sequence_id: 4 buffer: 0 protocol_abi_versions: 2 }
          }
        }
        """),
        query="""
          SELECT name, idx, severity, value
          FROM stats
          WHERE name GLOB 'traced_buf_protocol_*'
          ORDER BY name, idx;
        """,
        out=Csv("""
        "name","idx","severity","value"
        "traced_buf_protocol_v1_writers",0,"info",1
        "traced_buf_protocol_v2_writers",0,"info",2
        """))

  # Check that each ExperimentalTracingV2ProducerStats field maps to its
  # stat, indexed by producer ID.
  def test_tracing_v2_producer_stats(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_uid: 9999
          trusted_packet_sequence_id: 1
          experimental_tracing_v2_producer_stats {
            producer_id: 2
            pid: 200
            producer_name: "producer_b"
            chunk_size_bytes: 1024
            num_chunks: 16
            chunks_read: 7
          }
        }
        packet {
          trusted_uid: 9999
          trusted_packet_sequence_id: 1
          experimental_tracing_v2_producer_stats {
            producer_id: 1
            pid: 100
            producer_name: "producer_a"
            chunk_size_bytes: 4096
            num_chunks: 64
            chunks_read: 30
            bytes_read: 60000
            data_loss_chunks: 3
            malformed_chunks: 4
            unsupported_format_chunks: 5
            invalid_writer_chunks: 6
            invalid_destination_chunks: 7
            trace_buffer_rejected_chunks: 8
            rewrite_requests: 9
            positions_skipped: 10
            drain_passes: 11
            drain_time_ns: 12000
            drain_max_time_ns: 2000
            writer_dropped_packets: 13
            writer_stalls: 14
            writer_stall_time_ms: 1500
          }
        }
        """),
        query="""
          SELECT name, idx, value
          FROM stats
          WHERE name GLOB 'tracing_v2_producer_*'
          ORDER BY idx, name;
        """,
        out=Csv("""
        "name","idx","value"
        "tracing_v2_producer_bytes_read",1,60000
        "tracing_v2_producer_chunk_size_bytes",1,4096
        "tracing_v2_producer_chunks_read",1,30
        "tracing_v2_producer_data_loss_chunks",1,3
        "tracing_v2_producer_drain_max_time_ns",1,2000
        "tracing_v2_producer_drain_passes",1,11
        "tracing_v2_producer_drain_time_ns",1,12000
        "tracing_v2_producer_invalid_destination_chunks",1,7
        "tracing_v2_producer_invalid_writer_chunks",1,6
        "tracing_v2_producer_malformed_chunks",1,4
        "tracing_v2_producer_num_chunks",1,64
        "tracing_v2_producer_pid",1,100
        "tracing_v2_producer_positions_skipped",1,10
        "tracing_v2_producer_rewrite_requests",1,9
        "tracing_v2_producer_trace_buffer_rejected_chunks",1,8
        "tracing_v2_producer_unsupported_format_chunks",1,5
        "tracing_v2_producer_writer_dropped_packets",1,13
        "tracing_v2_producer_writer_stall_time_ms",1,1500
        "tracing_v2_producer_writer_stalls",1,14
        "tracing_v2_producer_bytes_read",2,0
        "tracing_v2_producer_chunk_size_bytes",2,1024
        "tracing_v2_producer_chunks_read",2,7
        "tracing_v2_producer_data_loss_chunks",2,0
        "tracing_v2_producer_drain_max_time_ns",2,0
        "tracing_v2_producer_drain_passes",2,0
        "tracing_v2_producer_drain_time_ns",2,0
        "tracing_v2_producer_invalid_destination_chunks",2,0
        "tracing_v2_producer_invalid_writer_chunks",2,0
        "tracing_v2_producer_malformed_chunks",2,0
        "tracing_v2_producer_num_chunks",2,16
        "tracing_v2_producer_pid",2,200
        "tracing_v2_producer_positions_skipped",2,0
        "tracing_v2_producer_rewrite_requests",2,0
        "tracing_v2_producer_trace_buffer_rejected_chunks",2,0
        "tracing_v2_producer_unsupported_format_chunks",2,0
        "tracing_v2_producer_writer_dropped_packets",2,0
        "tracing_v2_producer_writer_stall_time_ms",2,0
        "tracing_v2_producer_writer_stalls",2,0
        """))
