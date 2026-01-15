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
"""R8 retrace compatibility tests.

These tests verify Perfetto's deobfuscation is compatible with R8's retrace.
See README.md for links to the upstream R8 tests.

Most tests pass with simple class+method name resolution. The key failing test
is inline_positions which requires line-number-based method resolution to
reconstruct inline chains in stack_profile_symbol.
"""

from python.generators.diff_tests.testing import Csv, Path, DiffTestBlueprint, TestSuite


class R8RetraceCompat(TestSuite):

  # MethodWithInlinePositionsStackSampleRetraceTest
  #
  # R8 mapping format:
  #   com.example.Main -> a:
  #       1:1:void foo():54:54 -> a
  #       1:1:void test():50 -> a
  #       2:2:void bar():59:59 -> a
  #       2:2:void foo():55 -> a
  #       2:2:void test():50 -> a
  #       3:3:void baz():64:64 -> a
  #       3:3:void bar():60 -> a
  #       3:3:void foo():55 -> a
  #       3:3:void test():50 -> a
  #
  # At obfuscated line 1: foo() inlined into test()
  # At obfuscated line 2: bar() inlined into foo() inlined into test()
  # At obfuscated line 3: baz() inlined into bar() inlined into foo() inlined into test()
  #
  # Expected: stack_profile_symbol should contain the inline chain for each frame
  def test_inline_positions(self):
    return DiffTestBlueprint(
        trace=Path('inline_positions.textproto'),
        query="""
        SELECT
          spf.name AS obfuscated_name,
          sps.name AS deobfuscated_name,
          sps.line_number,
          sps.inlined
        FROM stack_profile_frame spf
        LEFT JOIN stack_profile_symbol sps USING (symbol_set_id)
        WHERE spf.name = 'a.a'
        ORDER BY spf.id, sps.id
        """,
        out=Csv("""
        "obfuscated_name","deobfuscated_name","line_number","inlined"
        "a.a","com.example.Main.foo",54,1
        "a.a","com.example.Main.test",50,0
        "a.a","com.example.Main.bar",59,1
        "a.a","com.example.Main.foo",55,1
        "a.a","com.example.Main.test",50,0
        "a.a","com.example.Main.baz",64,1
        "a.a","com.example.Main.bar",60,1
        "a.a","com.example.Main.foo",55,1
        "a.a","com.example.Main.test",50,0
        """))

  # HorizontalClassMergingStackSampleRetraceTest
  #
  # R8 mapping: Class B merged into A. Methods from B now on class A but should
  # retrace to original class B.
  #   a.c -> void A.foo()
  #   a.b -> void B.bar() (method originally from class B)
  #   a.a -> {void A.baz(), void B.baz()} (ambiguous)
  #
  # Expected: Methods should resolve to their ORIGINAL class, not the merged class.
  # For a.a, without line number context, both A.baz() and B.baz() are valid,
  # so it's marked as ambiguous with "Name1 | Name2" format.
  def test_horizontal_class_merging(self):
    return DiffTestBlueprint(
        trace=Path('horizontal_class_merging.textproto'),
        query="""
        SELECT name, deobfuscated_name
        FROM stack_profile_frame
        ORDER BY name
        """,
        out=Csv("""
        "name","deobfuscated_name"
        "a.a","com.example.A.baz | com.example.B.baz"
        "a.b","com.example.B.bar"
        "a.c","com.example.A.foo"
        """))

  # MethodWithOverloadStackSampleRetraceTest
  #
  # R8 mapping: Overloaded methods with same name but different return types
  # get different obfuscated names.
  #   b.a -> java.lang.Object StringSupplier.get()
  #   b.b -> java.lang.String StringSupplier.get()
  #
  # Expected: Both resolve to same method name (return type not preserved)
  def test_method_overload(self):
    return DiffTestBlueprint(
        trace=Path('method_overload.textproto'),
        query="""
        SELECT name, deobfuscated_name
        FROM stack_profile_frame
        ORDER BY name
        """,
        out=Csv("""
        "name","deobfuscated_name"
        "b.a","com.example.StringSupplier.get"
        "b.b","com.example.StringSupplier.get"
        """))

  # MethodWithRemovedArgumentStackSampleRetraceTest
  #
  # R8 mapping: Method test(Object) had unused argument removed.
  # The residual signature metadata indicates the compiled method has no args.
  #   a.a -> void Main.test(java.lang.Object)
  #       # {"id":"com.android.tools.r8.residualsignature","signature":"()V"}
  #
  # Expected: Simple method name resolution
  def test_removed_argument(self):
    return DiffTestBlueprint(
        trace=Path('removed_argument.textproto'),
        query="""
        SELECT name, deobfuscated_name
        FROM stack_profile_frame
        WHERE name = 'a.a'
        """,
        out=Csv("""
        "name","deobfuscated_name"
        "a.a","com.example.Main.test"
        """))

  # StaticizedMethodStackSampleRetraceTest
  #
  # R8 mapping: Instance method test() was made static.
  #   a.a -> void Main.test()
  #
  # Expected: Simple method name resolution (this case should work)
  def test_staticized_method(self):
    return DiffTestBlueprint(
        trace=Path('staticized_method.textproto'),
        query="""
        SELECT name, deobfuscated_name
        FROM stack_profile_frame
        WHERE name = 'a.a'
        """,
        out=Csv("""
        "name","deobfuscated_name"
        "a.a","com.example.Main.test"
        """))

  # VerticalClassMergingStackSampleRetraceTest
  #
  # R8 mapping: Class A merged into subclass B. A is marked as removed.
  #   A -> R8$$REMOVED$$CLASS$$0
  #   B -> a
  #   a.d -> void A.foo()
  #   a.b -> void A.bar()
  #   a.c -> void A.baz()
  #   a.a -> void B.bar()
  #
  # Expected: Methods should resolve to their original class
  def test_vertical_class_merging(self):
    return DiffTestBlueprint(
        trace=Path('vertical_class_merging.textproto'),
        query="""
        SELECT name, deobfuscated_name
        FROM stack_profile_frame
        ORDER BY name
        """,
        out=Csv("""
        "name","deobfuscated_name"
        "a.a","com.example.B.bar"
        "a.b","com.example.A.bar"
        "a.c","com.example.A.baz"
        "a.d","com.example.A.foo"
        """))
