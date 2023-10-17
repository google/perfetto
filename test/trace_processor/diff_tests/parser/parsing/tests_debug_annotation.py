#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ParsingDebugAnnotation(TestSuite):
  # Verify parsing of interned_string_value in DebugAnnotation proto.
  def test_interned_string_value(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          track_descriptor {
            uuid: 1
            thread {
              pid: 5
              tid: 1
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          interned_data {
            debug_annotation_names {
                iid: 1
                name: "key"
            }
            debug_annotation_string_values {
                iid: 1
                str: "value"
            }
          }
          track_event {
            track_uuid: 1
            type: TYPE_INSTANT
            name: "slice1"
            debug_annotations {
              name_iid: 1
              string_value_iid: 1
            }
          }
        }
        """),
        query="""
          SELECT EXTRACT_ARG(s.arg_set_id, 'debug.key') AS value
          FROM slice s;
        """,
        out=Csv("""
        "value"
        "value"
        """))
