/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/profiling/deobfuscator.h"

#include <string>
#include <utility>

#include "test/gtest_and_gmock.h"

namespace perfetto::profiling {

bool operator==(const ObfuscatedClass& a, const ObfuscatedClass& b);
bool operator==(const ObfuscatedClass& a, const ObfuscatedClass& b) {
  return a.deobfuscated_name() == b.deobfuscated_name() &&
         a.deobfuscated_fields() == b.deobfuscated_fields() &&
         a.deobfuscated_methods() == b.deobfuscated_methods();
}

namespace {

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Pair;

TEST(ProguardParserTest, ReadClass) {
  ProguardParser p;
  ASSERT_TRUE(
      p.AddLine(
           "android.arch.core.executor.ArchTaskExecutor -> android.arch.a.a.a:")
          .ok());
  ASSERT_THAT(p.ConsumeMapping(),
              ElementsAre(std::pair<std::string, ObfuscatedClass>(
                  "android.arch.a.a.a",
                  "android.arch.core.executor.ArchTaskExecutor")));
}

TEST(ProguardParserTest, MissingColon) {
  ProguardParser p;
  ASSERT_FALSE(
      p.AddLine(
           "android.arch.core.executor.ArchTaskExecutor -> android.arch.a.a.a")
          .ok());
}

TEST(ProguardParserTest, UnexpectedMember) {
  ProguardParser p;
  ASSERT_FALSE(
      p.AddLine("    android.arch.core.executor.TaskExecutor mDelegate -> b")
          .ok());
}

TEST(ProguardParserTest, Member) {
  ProguardParser p;
  ASSERT_TRUE(
      p.AddLine(
           "android.arch.core.executor.ArchTaskExecutor -> android.arch.a.a.a:")
          .ok());
  ASSERT_TRUE(
      p.AddLine("    android.arch.core.executor.TaskExecutor mDelegate -> b")
          .ok());
  auto mapping = p.ConsumeMapping();
  ASSERT_THAT(mapping, ElementsAre(Pair("android.arch.a.a.a", _)));
  const auto& cls = mapping.find("android.arch.a.a.a")->second;
  EXPECT_EQ(cls.deobfuscated_name(),
            "android.arch.core.executor.ArchTaskExecutor");
  EXPECT_THAT(cls.deobfuscated_fields(), ElementsAre(Pair("b", "mDelegate")));
  EXPECT_THAT(cls.deobfuscated_methods(), testing::IsEmpty());
}

TEST(ProguardParserTest, Method) {
  ProguardParser p;
  ASSERT_TRUE(
      p.AddLine(
           "android.arch.core.executor.ArchTaskExecutor -> android.arch.a.a.a:")
          .ok());
  ASSERT_TRUE(p.AddLine("    15:15:boolean isMainThread():116:116 -> b").ok());
  auto mapping = p.ConsumeMapping();
  ASSERT_THAT(mapping, ElementsAre(Pair("android.arch.a.a.a", _)));
  EXPECT_THAT(
      mapping.find("android.arch.a.a.a")->second.deobfuscated_methods(),
      ElementsAre(Pair(
          "b", "android.arch.core.executor.ArchTaskExecutor.isMainThread")));
}

TEST(ProguardParserTest, AmbiguousMethodSameCls) {
  ProguardParser p;
  ASSERT_TRUE(
      p.AddLine(
           "android.arch.core.executor.ArchTaskExecutor -> android.arch.a.a.a:")
          .ok());
  ASSERT_TRUE(p.AddLine("    15:15:boolean isMainThread():116:116 -> b").ok());
  ASSERT_TRUE(
      p.AddLine("    15:15:boolean somethingDifferent(int):116:116 -> b").ok());
  auto mapping = p.ConsumeMapping();
  ASSERT_THAT(mapping, ElementsAre(Pair("android.arch.a.a.a", _)));
  // Two different methods map to same obfuscated name - joined with " | "
  EXPECT_THAT(
      mapping.find("android.arch.a.a.a")->second.deobfuscated_methods(),
      ElementsAre(Pair(
          "b",
          "android.arch.core.executor.ArchTaskExecutor.isMainThread | "
          "android.arch.core.executor.ArchTaskExecutor.somethingDifferent")));
}

TEST(ProguardParserTest, AmbiguousMethodDifferentCls) {
  ProguardParser p;
  ASSERT_TRUE(
      p.AddLine(
           "android.arch.core.executor.ArchTaskExecutor -> android.arch.a.a.a:")
          .ok());
  ASSERT_TRUE(p.AddLine("    15:15:boolean isMainThread():116:116 -> b").ok());
  ASSERT_TRUE(
      p.AddLine("    15:15:boolean Foo.somethingDifferent(int):116:116 -> b")
          .ok());
  auto mapping = p.ConsumeMapping();
  ASSERT_THAT(mapping, ElementsAre(Pair("android.arch.a.a.a", _)));
  EXPECT_THAT(mapping.find("android.arch.a.a.a")->second.deobfuscated_methods(),
              ElementsAre(Pair(
                  "b",
                  "Foo.somethingDifferent | "
                  "android.arch.core.executor.ArchTaskExecutor.isMainThread")));
}

TEST(ProguardParserTest, AmbiguousMethodSameAndDifferentCls) {
  ProguardParser p;
  ASSERT_TRUE(
      p.AddLine(
           "android.arch.core.executor.ArchTaskExecutor -> android.arch.a.a.a:")
          .ok());
  ASSERT_TRUE(p.AddLine("    15:15:boolean isMainThread():116:116 -> b").ok());
  ASSERT_TRUE(p.AddLine("    15:15:boolean what(String):116:116 -> b").ok());
  ASSERT_TRUE(
      p.AddLine("    15:15:boolean Foo.somethingDifferent(int):116:116 -> b")
          .ok());
  auto mapping = p.ConsumeMapping();
  ASSERT_THAT(mapping, ElementsAre(Pair("android.arch.a.a.a", _)));
  // All unique fully-qualified names joined with " | "
  EXPECT_THAT(mapping.find("android.arch.a.a.a")->second.deobfuscated_methods(),
              ElementsAre(Pair(
                  "b",
                  "Foo.somethingDifferent | "
                  "android.arch.core.executor.ArchTaskExecutor.isMainThread | "
                  "android.arch.core.executor.ArchTaskExecutor.what")));
}

TEST(ProguardParserTest, AmbiguousMethodSameAndDifferentCls2) {
  ProguardParser p;
  ASSERT_TRUE(
      p.AddLine(
           "android.arch.core.executor.ArchTaskExecutor -> android.arch.a.a.a:")
          .ok());
  ASSERT_TRUE(p.AddLine("    15:15:boolean isMainThread():116:116 -> b").ok());
  ASSERT_TRUE(p.AddLine("    15:15:boolean what(String):116:116 -> b").ok());
  ASSERT_TRUE(
      p.AddLine("    15:15:boolean Foo.somethingDifferent(int):116:116 -> b")
          .ok());
  ASSERT_TRUE(
      p.AddLine("    15:15:boolean Foo.third(int,int):116:116 -> b").ok());
  auto mapping = p.ConsumeMapping();
  ASSERT_THAT(mapping, ElementsAre(Pair("android.arch.a.a.a", _)));
  // All unique fully-qualified names joined with " | "
  EXPECT_THAT(mapping.find("android.arch.a.a.a")->second.deobfuscated_methods(),
              ElementsAre(Pair(
                  "b",
                  "Foo.somethingDifferent | "
                  "Foo.third | "
                  "android.arch.core.executor.ArchTaskExecutor.isMainThread | "
                  "android.arch.core.executor.ArchTaskExecutor.what")));
}

TEST(ProguardParserTest, DuplicateClass) {
  ProguardParser p;
  ASSERT_TRUE(
      p.AddLine(
           "android.arch.core.executor.ArchTaskExecutor -> android.arch.a.a.a:")
          .ok());
  ASSERT_FALSE(p.AddLine("android.arch.core.executor.ArchTaskExecutor2 -> "
                         "android.arch.a.a.a:")
                   .ok());
}

TEST(ProguardParserTest, DuplicateField) {
  ProguardParser p;
  ASSERT_TRUE(
      p.AddLine(
           "android.arch.core.executor.ArchTaskExecutor -> android.arch.a.a.a:")
          .ok());
  ASSERT_TRUE(
      p.AddLine("    android.arch.core.executor.TaskExecutor mDelegate -> b")
          .ok());
  ASSERT_FALSE(
      p.AddLine("    android.arch.core.executor.TaskExecutor mDelegate2 -> b")
          .ok());
}

TEST(ProguardParserTest, DuplicateMethod) {
  ProguardParser p;
  ASSERT_TRUE(
      p.AddLine(
           "android.arch.core.executor.ArchTaskExecutor -> android.arch.a.a.a:")
          .ok());
  ASSERT_TRUE(p.AddLine("    15:15:boolean isMainThread():116:116 -> b").ok());
  ASSERT_TRUE(
      p.AddLine("    15:15:boolean doSomething(boolean):116:116 -> b").ok());
}

TEST(ProguardParserTest, DuplicateFieldSame) {
  ProguardParser p;
  ASSERT_TRUE(
      p.AddLine(
           "android.arch.core.executor.ArchTaskExecutor -> android.arch.a.a.a:")
          .ok());
  ASSERT_TRUE(
      p.AddLine("    android.arch.core.executor.TaskExecutor mDelegate -> b")
          .ok());
  ASSERT_TRUE(
      p.AddLine(
           "    1:1:android.arch.core.executor.TaskExecutor mDelegate -> b")
          .ok());
}

TEST(ProguardParserTest, EmptyLinesAndComments) {
  ProguardParser p;
  const char input[] = R"(
# comment

Example$$Class -> C:

    int first -> q
    # indented comment
    long second -> o
)";

  ASSERT_TRUE(p.AddLines(std::string(input)));
  auto mapping = p.ConsumeMapping();
  ASSERT_THAT(mapping, ElementsAre(Pair("C", _)));
  const auto& cls = mapping.find("C")->second;
  EXPECT_EQ(cls.deobfuscated_name(), "Example$$Class");
  EXPECT_THAT(cls.deobfuscated_fields(),
              ElementsAre(Pair("o", "second"), Pair("q", "first")));
  EXPECT_THAT(cls.deobfuscated_methods(), testing::IsEmpty());
}

// =============================================================================
// R8 Retrace Compatibility Tests
//
// These tests verify parsing of R8 mapping formats. They correspond to the
// diff tests in
// test/trace_processor/diff_tests/parser/profiling/r8_retrace_compat/
//
// Reference:
// https://r8.googlesource.com/r8/+/refs/heads/main/src/test/java/com/android/tools/r8/retrace/stacksamples/
// =============================================================================

// MethodWithInlinePositionsStackSampleRetraceTest
// https://r8.googlesource.com/r8/+/refs/heads/main/src/test/java/com/android/tools/r8/retrace/stacksamples/MethodWithInlinePositionsStackSampleRetraceTest.java
//
// R8 mapping format for inline positions:
//   com.example.Main -> a:
//       1:1:void foo():54:54 -> a
//       1:1:void test():50 -> a
//       2:2:void bar():59:59 -> a
//       2:2:void foo():55 -> a
//       2:2:void test():50 -> a
//
// At obfuscated line 1: foo() was inlined into test()
// At obfuscated line 2: bar() was inlined into foo() which was inlined into
// test()
//
// R8 expected behavior: Without line number context, method `a` should resolve
// to `test` (the outermost non-inlined method).
TEST(ProguardParserTest, R8InlinePositions) {
  ProguardParser p;
  const char input[] = R"(
com.example.Main -> a:
    1:1:void foo():54:54 -> a
    1:1:void test():50 -> a
    2:2:void bar():59:59 -> a
    2:2:void foo():55 -> a
    2:2:void test():50 -> a
    3:3:void baz():64:64 -> a
    3:3:void bar():60 -> a
    3:3:void foo():55 -> a
    3:3:void test():50 -> a
)";

  ASSERT_TRUE(p.AddLines(std::string(input)));
  auto mapping = p.ConsumeMapping();
  ASSERT_THAT(mapping, ElementsAre(Pair("a", _)));
  // R8 expected: Without line context, resolve to outermost method `test`.
  EXPECT_THAT(mapping.find("a")->second.deobfuscated_methods(),
              ElementsAre(Pair("a", "com.example.Main.test")));
}

// HorizontalClassMergingStackSampleRetraceTest
// https://r8.googlesource.com/r8/+/refs/heads/main/src/test/java/com/android/tools/r8/retrace/stacksamples/HorizontalClassMergingStackSampleRetraceTest.java
//
// R8 mapping: Class B merged into A. Methods from B now on class A but should
// retrace to original class B.
//   com.example.A -> a:
//       void foo() -> c
//   com.example.B -> a:
//       void bar() -> b
//       void baz() -> a  # ambiguous with A.baz if it existed
//
// Expected: Methods should resolve to their ORIGINAL class, not the merged
// class
TEST(ProguardParserTest, R8HorizontalClassMerging) {
  ProguardParser p;
  // When two classes map to the same obfuscated name, the parser should error
  // since it's a duplicate class mapping.
  ASSERT_TRUE(p.AddLine("com.example.A -> a:").ok());
  ASSERT_TRUE(p.AddLine("    void foo() -> c").ok());
  // This should fail - duplicate obfuscated class name
  ASSERT_FALSE(p.AddLine("com.example.B -> a:").ok());
}

// For horizontal class merging to work, R8 actually outputs methods with
// qualified names pointing to original class
TEST(ProguardParserTest, R8HorizontalClassMergingQualifiedMethods) {
  ProguardParser p;
  const char input[] = R"(
com.example.A -> a:
    void foo() -> c
    void com.example.B.bar() -> b
    void baz() -> a
)";

  ASSERT_TRUE(p.AddLines(std::string(input)));
  auto mapping = p.ConsumeMapping();
  ASSERT_THAT(mapping, ElementsAre(Pair("a", _)));
  EXPECT_THAT(mapping.find("a")->second.deobfuscated_methods(),
              ElementsAre(Pair("a", "com.example.A.baz"),
                          Pair("b", "com.example.B.bar"),
                          Pair("c", "com.example.A.foo")));
}

// MethodWithOverloadStackSampleRetraceTest
// https://r8.googlesource.com/r8/+/refs/heads/main/src/test/java/com/android/tools/r8/retrace/stacksamples/MethodWithOverloadStackSampleRetraceTest.java
//
// R8 mapping: Overloaded methods with same name but different return types
// get different obfuscated names.
TEST(ProguardParserTest, R8MethodOverload) {
  ProguardParser p;
  const char input[] = R"(
com.example.StringSupplier -> b:
    java.lang.Object get() -> a
    java.lang.String get() -> b
)";

  ASSERT_TRUE(p.AddLines(std::string(input)));
  auto mapping = p.ConsumeMapping();
  ASSERT_THAT(mapping, ElementsAre(Pair("b", _)));
  // Both map to "get" - different obfuscated names for different overloads
  EXPECT_THAT(mapping.find("b")->second.deobfuscated_methods(),
              ElementsAre(Pair("a", "com.example.StringSupplier.get"),
                          Pair("b", "com.example.StringSupplier.get")));
}

// StaticizedMethodStackSampleRetraceTest
// https://r8.googlesource.com/r8/+/refs/heads/main/src/test/java/com/android/tools/r8/retrace/stacksamples/StaticizedMethodStackSampleRetraceTest.java
//
// R8 mapping: Instance method test() was made static.
// Simple case that should work with basic name resolution.
TEST(ProguardParserTest, R8StaticizedMethod) {
  ProguardParser p;
  const char input[] = R"(
com.example.Main -> a:
    void test() -> a
)";

  ASSERT_TRUE(p.AddLines(std::string(input)));
  auto mapping = p.ConsumeMapping();
  ASSERT_THAT(mapping, ElementsAre(Pair("a", _)));
  EXPECT_THAT(mapping.find("a")->second.deobfuscated_methods(),
              ElementsAre(Pair("a", "com.example.Main.test")));
}

// VerticalClassMergingStackSampleRetraceTest
// https://r8.googlesource.com/r8/+/refs/heads/main/src/test/java/com/android/tools/r8/retrace/stacksamples/VerticalClassMergingStackSampleRetraceTest.java
//
// R8 mapping: Class A merged into subclass B. Methods from A appear with
// qualified names on obfuscated class.
TEST(ProguardParserTest, R8VerticalClassMerging) {
  ProguardParser p;
  const char input[] = R"(
com.example.B -> a:
    void com.example.A.foo() -> d
    void com.example.A.bar() -> b
    void com.example.A.baz() -> c
    void bar() -> a
)";

  ASSERT_TRUE(p.AddLines(std::string(input)));
  auto mapping = p.ConsumeMapping();
  ASSERT_THAT(mapping, ElementsAre(Pair("a", _)));
  EXPECT_THAT(mapping.find("a")->second.deobfuscated_methods(),
              ElementsAre(Pair("a", "com.example.B.bar"),
                          Pair("b", "com.example.A.bar"),
                          Pair("c", "com.example.A.baz"),
                          Pair("d", "com.example.A.foo")));
}

}  // namespace
}  // namespace perfetto::profiling
