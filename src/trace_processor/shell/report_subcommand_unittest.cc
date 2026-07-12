/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/shell/report_subcommand.h"

#include <string>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::shell {
namespace {

using ::testing::HasSubstr;

TEST(ReportArgsTest, DefaultsToOverview) {
  auto r = ParseReportArgs({"trace.pb"}, /*remote=*/false);
  ASSERT_TRUE(r.ok()) << r.status().message();
  EXPECT_EQ(r->noun, "overview");
  EXPECT_EQ(r->view, "");
  EXPECT_EQ(r->trace_file, "trace.pb");
}

TEST(ReportArgsTest, NounResolvesDefaultView) {
  auto r = ParseReportArgs({"slices", "trace.pb"}, /*remote=*/false);
  ASSERT_TRUE(r.ok()) << r.status().message();
  EXPECT_EQ(r->noun, "slices");
  EXPECT_EQ(r->view, "flat");
  EXPECT_EQ(r->trace_file, "trace.pb");
}

TEST(ReportArgsTest, ExplicitView) {
  auto r = ParseReportArgs({"slices", "flat", "trace.pb"}, /*remote=*/false);
  ASSERT_TRUE(r.ok()) << r.status().message();
  EXPECT_EQ(r->noun, "slices");
  EXPECT_EQ(r->view, "flat");
  EXPECT_EQ(r->trace_file, "trace.pb");
}

TEST(ReportArgsTest, UnknownNoun) {
  auto r = ParseReportArgs({"bogus", "trace.pb"}, /*remote=*/false);
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(r.status().message(), HasSubstr("unknown noun 'bogus'"));
  EXPECT_THAT(r.status().message(), HasSubstr("slices"));
}

TEST(ReportArgsTest, UnknownView) {
  auto r = ParseReportArgs({"slices", "bogus", "trace.pb"}, /*remote=*/false);
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(r.status().message(), HasSubstr("unknown view 'bogus'"));
}

TEST(ReportArgsTest, TooManyArgs) {
  auto r =
      ParseReportArgs({"slices", "flat", "extra", "trace.pb"}, /*remote=*/false);
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(r.status().message(), HasSubstr("unexpected argument"));
}

TEST(ReportArgsTest, MissingTraceFile) {
  auto r = ParseReportArgs({}, /*remote=*/false);
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(r.status().message(), HasSubstr("trace file is required"));
}

TEST(ReportArgsTest, NounWithoutTraceFileIsNotTreatedAsTrace) {
  // `report slices` (no trace) must treat "slices" as the noun and report a
  // missing trace file, not try to load a trace named "slices".
  auto r = ParseReportArgs({"slices"}, /*remote=*/false);
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(r.status().message(), HasSubstr("trace file is required"));
}

TEST(ReportArgsTest, TraceFileNamedLikeNounNeedsExplicitOverview) {
  // Escape hatch: a trace file literally named "slices" is reachable via the
  // explicit overview noun.
  auto r = ParseReportArgs({"overview", "slices"}, /*remote=*/false);
  ASSERT_TRUE(r.ok()) << r.status().message();
  EXPECT_EQ(r->noun, "overview");
  EXPECT_EQ(r->trace_file, "slices");
}

TEST(ReportArgsTest, RemoteConsumesNoTraceFile) {
  auto r = ParseReportArgs({"slices"}, /*remote=*/true);
  ASSERT_TRUE(r.ok()) << r.status().message();
  EXPECT_EQ(r->noun, "slices");
  EXPECT_EQ(r->view, "flat");
  EXPECT_EQ(r->trace_file, "");
}

TEST(ReportArgsTest, RemoteOverview) {
  auto r = ParseReportArgs({}, /*remote=*/true);
  ASSERT_TRUE(r.ok()) << r.status().message();
  EXPECT_EQ(r->noun, "overview");
  EXPECT_EQ(r->trace_file, "");
}

}  // namespace
}  // namespace perfetto::trace_processor::shell
