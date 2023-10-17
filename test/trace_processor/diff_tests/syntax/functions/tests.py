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
from python.generators.diff_tests.testing import PrintProfileProto
from google.protobuf import text_format


class Functions(TestSuite):

  def test_create_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT create_function('f(x INT)', 'INT', 'SELECT $x + 1');

        SELECT f(5) as result;
      """,
        out=Csv("""
        "result"
        6
      """))

  def test_create_function_returns_string(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT create_function('f(x INT)', 'STRING', 'SELECT "value_" || $x');

        SELECT f(5) as result;
      """,
        out=Csv("""
        "result"
        "value_5"
      """))

  def test_create_function_duplicated(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT create_function('f()', 'INT', 'SELECT 1');
        SELECT create_function('f()', 'INT', 'SELECT 1');

        SELECT f() as result;
      """,
        out=Csv("""
        "result"
        1
      """))

  def test_create_function_recursive(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        -- Compute factorial.
        SELECT create_function('f(x INT)', 'INT',
        '
          SELECT IIF($x = 0, 1, $x * f($x - 1))
        ');

        SELECT f(5) as result;
      """,
        out=Csv("""
        "result"
        120
      """))

  def test_create_function_recursive_string(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        -- Compute factorial.
        SELECT create_function('f(x INT)', 'STRING',
        '
          SELECT IIF(
            $x = 0,
            "",
            -- 97 is the ASCII code for "a".
            f($x - 1) || char(96 + $x) || f($x - 1))
        ');

        SELECT f(4) as result;
      """,
        out=Csv("""
          "result"
          "abacabadabacaba"
      """))

  def test_create_function_recursive_string_memoized(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        -- Compute factorial.
        SELECT create_function('f(x INT)', 'STRING',
        '
          SELECT IIF(
            $x = 0,
            "",
            -- 97 is the ASCII code for "a".
            f($x - 1) || char(96 + $x) || f($x - 1))
        ');

        SELECT experimental_memoize('f');

        SELECT f(4) as result;
      """,
        out=Csv("""
          "result"
          "abacabadabacaba"
      """))

  def test_create_function_memoize(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        -- Compute 2^n inefficiently to test memoization.
        -- If it times out, memoization is not working.
        SELECT create_function('f(x INT)', 'INT',
        '
          SELECT IIF($x = 0, 1, f($x - 1) + f($x - 1))
        ');

        SELECT EXPERIMENTAL_MEMOIZE('f');

        -- 2^50 is too expensive to compute, but memoization makes it fast.
        SELECT f(50) as result;
      """,
        out=Csv("""
        "result"
        1125899906842624
      """))

  def test_create_function_memoize_float(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        -- Compute 2^n inefficiently to test memoization.
        -- If it times out, memoization is not working.
        SELECT create_function('f(x INT)', 'FLOAT',
        '
          SELECT $x + 0.5
        ');

        SELECT EXPERIMENTAL_MEMOIZE('f');

        SELECT printf("%.1f", f(1)) as result
        UNION ALL
        SELECT printf("%.1f", f(1)) as result
        UNION ALL
        SELECT printf("%.1f", f(1)) as result
      """,
        out=Csv("""
        "result"
        "1.5"
        "1.5"
        "1.5"
      """))

  def test_create_function_memoize_intermittent_memoization(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        -- This function returns NULL for odd numbers and 1 for even numbers.
        -- As we do not memoize NULL results, we would only memoize the results
        -- for even numbers.
        SELECT create_function('f(x INT)', 'INT',
        '
          SELECT IIF($x = 0, 1,
            IIF(f($x - 1) IS NULL, 1, NULL)
          )
        ');

        SELECT EXPERIMENTAL_MEMOIZE('f');

        SELECT
          f(50) as f_50,
          f(51) as f_51;
      """,
        out=Csv("""
        "f_50","f_51"
        1,"[NULL]"
      """))

  def test_create_function_memoize_subtree_size(self):
    # Tree:
    #            1
    #           / \
    #          /   \
    #         /     \
    #        2       3
    #       / \     / \
    #      4   5   6   7
    #     / \  |   |  | \
    #    8   9 10 11 12 13
    #    |   |
    #   14   15
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE tree AS
        WITH data(id, parent_id) as (VALUES
          (1, NULL),
          (2, 1),
          (3, 1),
          (4, 2),
          (5, 2),
          (6, 3),
          (7, 3),
          (8, 4),
          (9, 4),
          (10, 5),
          (11, 6),
          (12, 7),
          (13, 7),
          (14, 8),
          (15, 9)
        )
        SELECT * FROM data;

        SELECT create_function('subtree_size(id INT)', 'INT',
        '
          SELECT 1 + IFNULL((
            SELECT
              SUM(subtree_size(child.id))
            FROM tree child
            WHERE child.parent_id = $id
          ), 0)
        ');

        SELECT EXPERIMENTAL_MEMOIZE('subtree_size');

        SELECT
          id, subtree_size(id) as size
        FROM tree
        ORDER BY id;
      """,
        out=Csv("""
        "id","size"
        1,15
        2,8
        3,6
        4,5
        5,2
        6,2
        7,3
        8,2
        9,2
        10,1
        11,1
        12,1
        13,1
        14,1
        15,1
      """))

  def test_create_view_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT create_view_function('f(x INT)', 'result INT', 'SELECT $x + 1 as result');

        SELECT * FROM f(5);
      """,
        out=Csv("""
        "result"
        6
      """))

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
        INCLUDE PERFETTO MODULE common.timestamps;
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
        INCLUDE PERFETTO MODULE common.timestamps;
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
        INCLUDE PERFETTO MODULE common.timestamps;
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
        INCLUDE PERFETTO MODULE common.timestamps;
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
        INCLUDE PERFETTO MODULE common.timestamps;
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
        INCLUDE PERFETTO MODULE common.timestamps;
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
        INCLUDE PERFETTO MODULE common.timestamps;
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
        INCLUDE PERFETTO MODULE common.timestamps;
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

  def test_profile_no_functions(self):
    return DiffTestBlueprint(
        trace=DataPath("perf_sample_no_functions.pb"),
        query="""
        SELECT HEX(
          EXPERIMENTAL_PROFILE(STACK_FROM_STACK_PROFILE_CALLSITE(callsite_id))
        )
        FROM PERF_SAMPLE
    """,
        out=BinaryProto(
            message_type="perfetto.third_party.perftools.profiles.Profile",
            post_processing=PrintProfileProto,
            contents="""
        Sample:
          Values: 1
          Stack:
            (0x7a4167d3f8)
            (0x783153c8e4)
            (0x7a4161ef8c)
            (0x7a42c3d8b0)
            (0x7a4167d9f4)
            (0x7a4163bc44)
            (0x7a4172f330)
            (0x7a4177a658)
            (0x7a4162b3a0)

        Sample:
          Values: 1
          Stack:
            (0x7a4167d9f8)
            (0x7a4163bc44)
            (0x7a4172f330)
            (0x7a4177a658)
            (0x7a4162b3a0)
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
            post_processing=PrintProfileProto,
            contents="""
            Sample:
              Values: 1
              Stack:
                B (0x0)
                perfetto::base::UnixTaskRunner::Run() (0x7a4172f330)
                perfetto::ServiceMain(int, char**) (0x7a4177a658)
                __libc_init (0x7a4162b3a0)
                A (0x0)
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
            post_processing=PrintProfileProto,
            contents="""
            Sample:
              Values: 42
                Stack:
                  B (0x0)
                  A (0x0)
            """))

  def test_profile_aggregates_samples(self):
    return DiffTestBlueprint(
        trace=DataPath("perf_sample.pb"),
        query="""
        WITH samples(stack, value) AS (
        VALUES
          (CAT_STACKS("A", "B"), 4),
          (CAT_STACKS("A", "B"), 8),
          (CAT_STACKS("A", "B"), 15),
          (CAT_STACKS("A", "C"), 16),
          (CAT_STACKS("C", "B"), 23),
          (CAT_STACKS("C", "B"), 42)
        )
        SELECT HEX(
          EXPERIMENTAL_PROFILE(
            stack, "type", "units", value))
        FROM samples
        """,
        out=BinaryProto(
            message_type="perfetto.third_party.perftools.profiles.Profile",
            post_processing=PrintProfileProto,
            contents="""
            Sample:
              Values: 16
              Stack:
                C (0x0)
                A (0x0)

            Sample:
              Values: 27
              Stack:
                B (0x0)
                A (0x0)

            Sample:
              Values: 65
              Stack:
                B (0x0)
                C (0x0)
            """))

  def test_annotated_callstack(self):
    return DiffTestBlueprint(
        trace=DataPath("perf_sample_annotations.pftrace"),
        query="""
        SELECT HEX(EXPERIMENTAL_PROFILE(STACK_FROM_STACK_PROFILE_CALLSITE(251, TRUE)))
        """,
        out=BinaryProto(
            message_type="perfetto.third_party.perftools.profiles.Profile",
            post_processing=PrintProfileProto,
            contents="""
            Sample:
              Values: 1
              Stack:
                art::ResolveFieldWithAccessChecks(art::Thread*, art::ClassLinker*, unsigned short, art::ArtMethod*, bool, bool, unsigned long) [common-frame] (0x724da79a74)
                NterpGetInstanceFieldOffset [common-frame-interp] (0x724da794b0)
                nterp_get_instance_field_offset [common-frame-interp] (0x724dcfc070)
                nterp_op_iget_object_slow_path [common-frame-interp] (0x724dcf5884)
                android.view.ViewRootImpl.notifyDrawStarted [interp] (0x7248f894d2)
                android.view.ViewRootImpl.performTraversals [aot] (0x71b8d378)
                android.view.ViewRootImpl.doTraversal [aot] (0x71b93220)
                android.view.ViewRootImpl$TraversalRunnable.run [aot] (0x71ab0384)
                android.view.Choreographer.doCallbacks [aot] (0x71a91b6c)
                android.view.Choreographer.doFrame [aot] (0x71a92550)
                android.view.Choreographer$FrameDisplayEventReceiver.run [aot] (0x71b26fb0)
                android.os.Handler.dispatchMessage [aot] (0x71975924)
                android.os.Looper.loopOnce [aot] (0x71978d6c)
                android.os.Looper.loop [aot] (0x719788a0)
                android.app.ActivityThread.main [aot] (0x717454cc)
                art_quick_invoke_static_stub [common-frame] (0x724db2de00)
                _jobject* art::InvokeMethod<(art::PointerSize)8>(art::ScopedObjectAccessAlreadyRunnable const&, _jobject*, _jobject*, _jobject*, unsigned long) [common-frame] (0x724db545ec)
                art::Method_invoke(_JNIEnv*, _jobject*, _jobject*, _jobjectArray*) (.__uniq.165753521025965369065708152063621506277) (0x724db53ad0)
                art_jni_trampoline [common-frame] (0x6ff5c578)
                com.android.internal.os.RuntimeInit$MethodAndArgsCaller.run [aot] (0x71c4ab6c)
                com.android.internal.os.ZygoteInit.main [aot] (0x71c54c7c)
                art_quick_invoke_static_stub (0x724db2de00)
                art::JValue art::InvokeWithVarArgs<_jmethodID*>(art::ScopedObjectAccessAlreadyRunnable const&, _jobject*, _jmethodID*, std::__va_list) (0x724dc422a8)
                art::JNI<true>::CallStaticVoidMethodV(_JNIEnv*, _jclass*, _jmethodID*, std::__va_list) (0x724dcc57c8)
                _JNIEnv::CallStaticVoidMethod(_jclass*, _jmethodID*, ...) (0x74e1b03ca8)
                android::AndroidRuntime::start(char const*, android::Vector<android::String8> const&, bool) (0x74e1b0feac)
                main (0x63da9c354c)
                __libc_init (0x74ff4a0728)
            """))

  def test_layout(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        """),
        query="""
        CREATE TABLE TEST(start INTEGER, end INTEGER);

        INSERT INTO TEST
        VALUES
        (1, 5),
        (2, 4),
        (3, 8),
        (6, 7),
        (6, 7),
        (6, 7);

        WITH custom_slices as (
          SELECT
            start as ts,
            end - start as dur
          FROM test
        )
        SELECT
          ts,
          INTERNAL_LAYOUT(ts, dur) over (
            order by ts
            rows between unbounded preceding and current row
          ) as depth
        FROM custom_slices
        """,
        out=Csv("""
        "ts","depth"
        1,0
        2,1
        3,2
        6,0
        6,1
        6,3
        """))

  def test_layout_with_instant_events(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        """),
        query="""
        CREATE TABLE TEST(start INTEGER, end INTEGER);

        INSERT INTO TEST
        VALUES
        (1, 5),
        (2, 2),
        (3, 3),
        (4, 4);

        WITH custom_slices as (
          SELECT
            start as ts,
            end - start as dur
          FROM test
        )
        SELECT
          ts,
          INTERNAL_LAYOUT(ts, dur) over (
            order by ts
            rows between unbounded preceding and current row
          ) as depth
        FROM custom_slices
        """,
        out=Csv("""
        "ts","depth"
        1,0
        2,1
        3,1
        4,1
        """))

  def test_layout_with_events_without_end(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        """),
        query="""
        CREATE TABLE TEST(ts INTEGER, dur INTEGER);

        INSERT INTO TEST
        VALUES
        (1, -1),
        (2, -1),
        (3, 5),
        (4, 1),
        (5, 1);

        SELECT
          ts,
          INTERNAL_LAYOUT(ts, dur) over (
            order by ts
            rows between unbounded preceding and current row
          ) as depth
        FROM test
        """,
        out=Csv("""
        "ts","depth"
        1,0
        2,1
        3,2
        4,3
        5,3
        """))

  def test_math_ln_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT
          CAST(LN(1) * 1000 AS INTEGER) AS valid,
          LN("as") AS invalid_str,
          LN(NULL) AS invalid_null
        """,
        out=Csv("""
        "valid","invalid_str","invalid_null"
        0,"[NULL]","[NULL]"
        """))

  def test_math_exp_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT
          CAST(EXP(1) * 1000 AS INTEGER) AS valid,
          EXP("asd") AS invalid_str,
          EXP(NULL) AS invalid_null
        """,
        out=Csv("""
        "valid","invalid_str","invalid_null"
        2718,"[NULL]","[NULL]"
        """))

  def test_math_sqrt_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT
          CAST(SQRT(4) AS INTEGER) AS valid,
          SQRT("asd") AS invalid_str,
          SQRT(NULL) AS invalid_null
        """,
        out=Csv("""
        "valid","invalid_str","invalid_null"
        2,"[NULL]","[NULL]"
        """))

  def test_math_functions(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT
          CAST(SQRT(EXP(LN(1))) AS INTEGER) AS valid,
          SQRT(EXP(LN("asd"))) AS invalid_str,
          SQRT(EXP(LN(NULL))) AS invalid_null
        """,
        out=Csv("""
        "valid","invalid_str","invalid_null"
        1,"[NULL]","[NULL]"
        """))

  def test_table_function_drop_partial(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
          CREATE TABLE bar AS SELECT 1;

          CREATE OR REPLACE PERFETTO FUNCTION foo()
          RETURNS TABLE(x INT) AS
          SELECT 1 AS x
          UNION
          SELECT * FROM bar;

          CREATE TABLE res AS SELECT * FROM foo() LIMIT 1;

          DROP TABLE bar;
        """,
        out=Csv(""))
