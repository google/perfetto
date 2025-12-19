#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ArgsFunctions(TestSuite):

  def test_extract_arg(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          timestamp: 10000000
          track_event {
            track_uuid: 1
            categories: "cat"
            name: "Event"
            type: TYPE_INSTANT
            debug_annotations {
              name: "key1"
              string_value: "value1"
            }
            debug_annotations {
              name: "key2"
              int_value: 42
            }
            debug_annotations {
              name: "key3"
              double_value: 3.25
            }
          }
        }
        """),
        query="""
        SELECT
          name,
          extract_arg(arg_set_id, 'debug.key1') AS string_val,
          extract_arg(arg_set_id, 'debug.key2') AS int_val,
          extract_arg(arg_set_id, 'debug.key3') AS double_val,
          extract_arg(arg_set_id, 'debug.missing_key') AS missing_val
        FROM slice;
        """,
        out=Csv("""
        "name","string_val","int_val","double_val","missing_val"
        "Event","value1",42,3.250000,"[NULL]"
        """))

  def test_display_value(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          timestamp: 10000000
          track_event {
            track_uuid: 1
            categories: "cat"
            name: "Event"
            type: TYPE_INSTANT
            debug_annotations {
              name: "int_val"
              int_value: -123
            }
            debug_annotations {
              name: "uint_val"
              uint_value: 18446744073709551615
            }
            debug_annotations {
              name: "string_val"
              string_value: "hello"
            }
            debug_annotations {
              name: "real_val"
              double_value: 3.5
            }
            debug_annotations {
              name: "pointer_val"
              pointer_value: 48879
            }
            debug_annotations {
              name: "bool_true"
              bool_value: true
            }
            debug_annotations {
              name: "bool_false"
              bool_value: false
            }
          }
        }
        """),
        query="""
        SELECT
          key,
          value_type,
          display_value
        FROM slice
        JOIN args USING (arg_set_id);
        """,
        out=Csv("""
        "key","value_type","display_value"
        "debug.int_val","int","-123"
        "debug.uint_val","uint","18446744073709551615"
        "debug.string_val","string","hello"
        "debug.real_val","real","3.5"
        "debug.pointer_val","pointer","0xbeef"
        "debug.bool_true","bool","true"
        "debug.bool_false","bool","false"
        """))

  def test__intrinsic_arg_set_to_json_simple(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          timestamp: 10000000
          track_event {
            track_uuid: 1
            categories: "cat"
            name: "Event"
            type: TYPE_INSTANT
            debug_annotations {
              name: "key1"
              string_value: "value1"
            }
            debug_annotations {
              name: "key2"
              int_value: 42
            }
          }
        }
        """),
        query="""
        SELECT
          name,
          __intrinsic_arg_set_to_json(arg_set_id) AS args_json
        FROM slice;
        """,
        out=Csv("""
        "name","args_json"
        "Event","{\"debug\":{\"key1\":\"value1\",\"key2\":42}}"
        """))

  def test_arg_set_to_json_empty(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          timestamp: 10000000
          track_event {
            track_uuid: 1
            categories: "cat"
            name: "Event"
            type: TYPE_INSTANT
          }
        }
        """),
        query="""
        SELECT
          name,
          COALESCE(__intrinsic_arg_set_to_json(arg_set_id), '{}') AS args_json
        FROM slice;
        """,
        out=Csv("""
        "name","args_json"
        "Event","{}"
        """))

  def test_arg_set_to_json_complex_types(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          timestamp: 10000000
          track_event {
            track_uuid: 1
            categories: "cat"
            name: "Event"
            type: TYPE_INSTANT
            debug_annotations {
              name: "string_val"
              string_value: "test"
            }
            debug_annotations {
              name: "int_val"
              int_value: -123
            }
            debug_annotations {
              name: "uint_val"
              uint_value: 456
            }
            debug_annotations {
              name: "bool_val"
              bool_value: true
            }
            debug_annotations {
              name: "ptr_val"
              pointer_value: 100
            }
          }
        }
        """),
        query="""
        SELECT
          name,
          __intrinsic_arg_set_to_json(arg_set_id) AS args_json
        FROM slice;
        """,
        out=Csv("""
        "name","args_json"
        "Event","{\"debug\":{\"string_val\":\"test\",\"int_val\":-123,\"uint_val\":456,\"bool_val\":true,\"ptr_val\":\"0x64\"}}"
        """))

  def test_arg_set_to_json_nested(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          timestamp: 10000000
          track_event {
            track_uuid: 1
            categories: "cat"
            name: "Event"
            type: TYPE_INSTANT
            debug_annotations {
              name: "simple"
              string_value: "value"
            }
            debug_annotations {
              name: "nested"
              dict_entries {
                name: "inner1"
                string_value: "inner_value1"
              }
              dict_entries {
                name: "inner2"
                int_value: 99
              }
            }
          }
        }
        """),
        query="""
        SELECT
          name,
          __intrinsic_arg_set_to_json(arg_set_id) AS args_json
        FROM slice;
        """,
        out=Csv("""
        "name","args_json"
        "Event","{\"debug\":{\"simple\":\"value\",\"nested\":{\"inner1\":\"inner_value1\",\"inner2\":99}}}"
        """))

  def test_arg_set_to_json_with_arrays(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          timestamp: 10000000
          track_event {
            track_uuid: 1
            categories: "cat"
            name: "Event"
            type: TYPE_INSTANT
            debug_annotations {
              name: "array_field"
              array_values {
                int_value: 1
              }
              array_values {
                int_value: 2
              }
              array_values {
                int_value: 3
              }
            }
            debug_annotations {
              name: "string_array"
              array_values {
                string_value: "a"
              }
              array_values {
                string_value: "b"
              }
              array_values {
                string_value: "c"
              }
            }
          }
        }
        """),
        query="""
        SELECT
          name,
          __intrinsic_arg_set_to_json(arg_set_id) AS args_json
        FROM slice;
        """,
        out=Csv("""
        "name","args_json"
        "Event","{\"debug\":{\"array_field\":[1,2,3],\"string_array\":[\"a\",\"b\",\"c\"]}}"
        """))

  def test_arg_set_to_json_preserves_order(self):
    """Test that __intrinsic_arg_set_to_json preserves the insertion order of arguments
    and does not accidentally sort or reorder them."""
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          timestamp: 10000000
          track_event {
            track_uuid: 1
            categories: "cat"
            name: "Event"
            type: TYPE_INSTANT
            debug_annotations {
              name: "zebra"
              string_value: "last_alphabetically"
            }
            debug_annotations {
              name: "apple"
              string_value: "first_alphabetically"
            }
            debug_annotations {
              name: "middle"
              string_value: "middle_alphabetically"
            }
            debug_annotations {
              name: "nested"
              dict_entries {
                name: "z_field"
                int_value: 3
              }
              dict_entries {
                name: "a_field"
                int_value: 1
              }
              dict_entries {
                name: "m_field"
                int_value: 2
              }
            }
            debug_annotations {
              name: "array"
              array_values {
                string_value: "z"
              }
              array_values {
                string_value: "a"
              }
              array_values {
                string_value: "m"
              }
            }
          }
        }
        """),
        query="""
        SELECT
          name,
          __intrinsic_arg_set_to_json(arg_set_id) AS args_json
        FROM slice;
        """,
        out=Csv("""
        "name","args_json"
        "Event","{\"debug\":{\"zebra\":\"last_alphabetically\",\"apple\":\"first_alphabetically\",\"middle\":\"middle_alphabetically\",\"nested\":{\"z_field\":3,\"a_field\":1,\"m_field\":2},\"array\":[\"z\",\"a\",\"m\"]}}"
        """))
