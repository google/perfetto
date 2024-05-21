#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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
          previous_packet_dropped: true
        }
        packet {
          trusted_packet_sequence_id: 2
        }
        packet {
          trusted_packet_sequence_id: 2
        }
        packet {
          trusted_packet_sequence_id: 3
          previous_packet_dropped: true
        }
        packet {
          trusted_packet_sequence_id: 3
          previous_packet_dropped: true
        }
        packet {
          trusted_packet_sequence_id: 3
        }
        packet {
          trusted_packet_sequence_id: 4
          previous_packet_dropped: true
        }
        packet {
          trusted_packet_sequence_id: 4
          previous_packet_dropped: true
        }
        packet {
          trusted_packet_sequence_id: 4
        }
        packet {
          trusted_packet_sequence_id: 5
          previous_packet_dropped: true
        }
        packet {
          trusted_packet_sequence_id: 5
          previous_packet_dropped: true
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
