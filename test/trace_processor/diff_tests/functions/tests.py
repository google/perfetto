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
from google.protobuf import text_format


def PrintProfileProto(profile):
  locations = {l.id: l for l in profile.location}
  functions = {f.id: f for f in profile.function}
  samples = []
  for s in profile.sample:
    stack = []
    for location in [locations[id] for id in s.location_id]:
      for function in [functions[l.function_id] for l in location.line]:
        stack.append("{name} ({address})".format(
            name=profile.string_table[function.name],
            address=hex(location.address)))
      if len(location.line) == 0:
        stack.append("({address})".format(address=hex(location.address)))
    samples.append('Sample:\nValues: {values}\nStack:\n{stack}'.format(
        values=', '.join(map(str, s.value)), stack='\n'.join(stack)))
  return '\n\n'.join(sorted(samples)) + '\n'


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

  def test_math_functions(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT
          CAST(EXP(1) * 1000 AS INTEGER) AS a,
          CAST(LN(1) * 1000 AS INTEGER) AS b,
          CAST(LN(EXP(1)) * 1000 AS INTEGER) AS c,
          EXP("asd") AS d,
          EXP(NULL) AS e,
          LN("as") AS f,
          LN(NULL) AS g
        """,
        out=Csv("""
        "a","b","c","d","e","f","g"
        2718,0,1000,"[NULL]","[NULL]","[NULL]","[NULL]"
        """))
