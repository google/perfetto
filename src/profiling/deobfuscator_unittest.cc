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

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {

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
  EXPECT_THAT(
      p.ConsumeMapping(),
      ElementsAre(std::pair<std::string, ObfuscatedClass>(
          "android.arch.a.a.a", {"android.arch.core.executor.ArchTaskExecutor",
                                 {{"b", "mDelegate"}},
                                 {}})));
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
  EXPECT_THAT(
      mapping.find("android.arch.a.a.a")->second.deobfuscated_methods(),
      ElementsAre(Pair(
          "b", "android.arch.core.executor.ArchTaskExecutor.[ambiguous]")));
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
  EXPECT_THAT(mapping.find("android.arch.a.a.a")->second.deobfuscated_methods(),
              ElementsAre(Pair(
                  "b",
                  "Foo.somethingDifferent | "
                  "android.arch.core.executor.ArchTaskExecutor.[ambiguous]")));
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
  EXPECT_THAT(mapping.find("android.arch.a.a.a")->second.deobfuscated_methods(),
              ElementsAre(Pair(
                  "b",
                  "Foo.[ambiguous] | "
                  "android.arch.core.executor.ArchTaskExecutor.[ambiguous]")));
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
  EXPECT_THAT(
      p.ConsumeMapping(),
      ElementsAre(std::pair<std::string, ObfuscatedClass>(
          "C", {"Example$$Class", {{"q", "first"}, {"o", "second"}}, {}})));
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
