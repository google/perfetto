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
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


def SortProfileProto(profile):
  profile.location.sort(key=lambda l: l.id)
  profile.function.sort(key=lambda f: f.id)
  profile.mapping.sort(key=lambda m: m.id)


class Functions(TestSuite):

  def test_first_non_null_frame(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        CREATE TABLE TEST(id INTEGER, val INTEGER);

        INSERT INTO TEST
        VALUES (1, 1), (2, NULL), (3, 3), (4, 4), (5, NULL), (6, NULL), (7, NULL);

        SELECT
          id,
          LAST_NON_NULL(val)
          OVER (ORDER BY id ASC ROWS BETWEEN CURRENT ROW AND 2 FOLLOWING) AS val
        FROM TEST
        ORDER BY id ASC;
        """,
        out=Csv("""
        "id","val"
        1,3
        2,4
        3,4
        4,4
        5,"[NULL]"
        6,"[NULL]"
        7,"[NULL]"
        """))

  def test_first_non_null_partition(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        CREATE TABLE TEST(id INTEGER, part TEXT, val INTEGER);

        INSERT INTO TEST
        VALUES
        (1, 'A', 1),
        (2, 'A', NULL),
        (3, 'A', 3),
        (4, 'B', NULL),
        (5, 'B', 5),
        (6, 'B', NULL),
        (7, 'B', 7);

        SELECT id, LAST_NON_NULL(val) OVER (PARTITION BY part ORDER BY id ASC) AS val
        FROM TEST
        ORDER BY id ASC;
        """,
        out=Csv("""
        "id","val"
        1,1
        2,1
        3,3
        4,"[NULL]"
        5,5
        6,5
        7,7
        """))

  def test_first_non_null(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        CREATE TABLE TEST(id INTEGER, val INTEGER);

        INSERT INTO TEST
        VALUES (1, 1), (2, NULL), (3, 3), (4, 4), (5, NULL), (6, NULL), (7, NULL);

        SELECT id, LAST_NON_NULL(val) OVER (ORDER BY id ASC) AS val
        FROM TEST
        ORDER BY id ASC;
        """,
        out=Csv("""
        "id","val"
        1,1
        2,1
        3,3
        4,4
        5,4
        6,4
        7,4
        """))

  def test_spans_overlapping_dur_intersect_edge(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        SELECT IMPORT('common.timestamps');
        SELECT SPANS_OVERLAPPING_DUR(0, 2, 1, 2) AS dur
        """,
        out=Csv("""
        "dur"
        1
        """))

  def test_spans_overlapping_dur_intersect_edge_reversed(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        SELECT IMPORT('common.timestamps');
        SELECT SPANS_OVERLAPPING_DUR(1, 2, 0, 2) AS dur
        """,
        out=Csv("""
        "dur"
        1
        """))

  def test_spans_overlapping_dur_intersect_all(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        SELECT IMPORT('common.timestamps');
        SELECT SPANS_OVERLAPPING_DUR(0, 3, 1, 1) AS dur
        """,
        out=Csv("""
        "dur"
        1
        """))

  def test_spans_overlapping_dur_intersect_all_reversed(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        SELECT IMPORT('common.timestamps');
        SELECT SPANS_OVERLAPPING_DUR(1, 1, 0, 3) AS dur
        """,
        out=Csv("""
        "dur"
        1
        """))

  def test_spans_overlapping_dur_no_intersect(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        SELECT IMPORT('common.timestamps');
        SELECT SPANS_OVERLAPPING_DUR(0, 1, 2, 1) AS dur
        """,
        out=Csv("""
        "dur"
        0
        """))

  def test_spans_overlapping_dur_no_intersect_reversed(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        SELECT IMPORT('common.timestamps');
        SELECT SPANS_OVERLAPPING_DUR(2, 1, 0, 1) AS dur
        """,
        out=Csv("""
        "dur"
        0
        """))

  def test_spans_overlapping_dur_negative_dur(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        SELECT IMPORT('common.timestamps');
        SELECT SPANS_OVERLAPPING_DUR(0, -1, 0, 1) AS dur
        """,
        out=Csv("""
        "dur"
        0
        """))

  def test_spans_overlapping_dur_negative_dur_reversed(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        SELECT IMPORT('common.timestamps');
        SELECT SPANS_OVERLAPPING_DUR(0, 1, 0, -1) AS dur
        """,
        out=Csv("""
        "dur"
        0
        """))

  def test_stacks(self):
    return DiffTestBlueprint(
        trace=DataPath("perf_sample.pb"),
        query="""
        SELECT HEX(
          CAT_STACKS(
            "A",
            CAT_STACKS(
              "B",
              CAT_STACKS(
                "C",
                STACK_FROM_STACK_PROFILE_CALLSITE(5),
                "D")
              ),
            "E",
            NULL,
            STACK_FROM_STACK_PROFILE_CALLSITE(14),
            STACK_FROM_STACK_PROFILE_CALLSITE(NULL),
            STACK_FROM_STACK_PROFILE_FRAME(4),
            STACK_FROM_STACK_PROFILE_FRAME(NULL)))
        """,
        out=BinaryProto(
            message_type="perfetto.protos.Stack",
            contents="""
              entries {
                frame_id: 4
              }
              entries {
                callsite_id: 14
              }
              entries {
                name: "E"
              }
              entries {
                name: "D"
              }
              entries {
                callsite_id: 5
              }
              entries {
                name: "C"
              }
              entries {
                name: "B"
              }
              entries {
                name: "A"
              }
        """))

  def test_profile_default_sample_types(self):
    return DiffTestBlueprint(
        trace=DataPath("perf_sample.pb"),
        query="""
        SELECT HEX(
          EXPERIMENTAL_PROFILE(
            CAT_STACKS(
              "A",
              STACK_FROM_STACK_PROFILE_CALLSITE(2),
              "B"
        )))
        """,
        out=BinaryProto(
            message_type="perfetto.third_party.perftools.profiles.Profile",
            post_processing=SortProfileProto,
            contents="""
            sample_type {
              type: 1
              unit: 2
            }
            sample {
              location_id: 1
              location_id: 2
              location_id: 3
              location_id: 4
              location_id: 5
              value: 1
            }
            mapping {
              id: 1
              memory_start: 525083627520
              memory_limit: 525084442624
              file_offset: 155648
              filename: 5
              build_id: 4
              has_functions: true
            }
            mapping {
              id: 2
              memory_start: 525082697728
              memory_limit: 525083201536
              file_offset: 241664
              filename: 11
              build_id: 10
              has_functions: true
            }
            location {
              id: 1
              line {
                function_id: 1
              }
            }
            location {
              id: 2
              mapping_id: 1
              address: 525084062512
              line {
                function_id: 2
              }
            }
            location {
              id: 3
              mapping_id: 1
              address: 525084370520
              line {
                function_id: 3
              }
            }
            location {
              id: 4
              mapping_id: 2
              address: 525082997664
              line {
                function_id: 4
              }
            }
            location {
              id: 5
              line {
                function_id: 5
              }
            }
            function {
              id: 1
              name: 3
            }
            function {
              id: 2
              name: 7
              system_name: 6
            }
            function {
              id: 3
              name: 9
              system_name: 8
            }
            function {
              id: 4
              name: 12
              system_name: 12
            }
            function {
              id: 5
              name: 13
            }
            string_table: ""
            string_table: "samples"
            string_table: "count"
            string_table: "B"
            string_table: "ec2fd72b19ae22c597fdd10451c25026"
            string_table: "/system/lib64/libperfetto.so"
            string_table: "_ZN8perfetto4base14UnixTaskRunner3RunEv"
            string_table: "perfetto::base::UnixTaskRunner::Run()"
            string_table: "_ZN8perfetto11ServiceMainEiPPc"
            string_table: "perfetto::ServiceMain(int, char**)"
            string_table: "04f0867d28ed6d6d36d30798cfe738ac"
            string_table: "/apex/com.android.runtime/lib64/bionic/libc.so"
            string_table: "__libc_init"
            string_table: "A"
        """))

  def test_profile_with_sample_types(self):
    return DiffTestBlueprint(
        trace=DataPath("perf_sample.pb"),
        query="""
        SELECT HEX(
          EXPERIMENTAL_PROFILE(
            CAT_STACKS("A", "B"), "type", "units", 42))
        """,
        out=BinaryProto(
            message_type="perfetto.third_party.perftools.profiles.Profile",
            post_processing=SortProfileProto,
            contents="""
            sample_type {
              type: 1
              unit: 2
            }
            sample {
              location_id: 1
              location_id: 2
              value: 42
            }
            location {
              id: 1
              line {
                function_id: 1
              }
            }
            location {
              id: 2
              line {
                function_id: 2
              }
            }
            function {
              id: 1
              name: 3
            }
            function {
              id: 2
              name: 4
            }
            string_table: ""
            string_table: "type"
            string_table: "units"
            string_table: "B"
            string_table: "A"
        """))
