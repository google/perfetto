/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/perf/perf_session.h"

#include <cstdint>
#include <cstring>

#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/perf/perf_event.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::perf_importer {
namespace {

using ::testing::Eq;
using ::testing::NotNull;

MATCHER(IsOk, "is ok") {
  return arg.ok();
}

MATCHER_P(IsOkAndHolds, matcher, "") {
  return ExplainMatchResult(IsOk(), arg, result_listener) &&
         ExplainMatchResult(matcher, *arg, result_listener);
}

TEST(PerfSessionTest, NoAttrBuildFails) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  PerfSession::Builder builder(&context);
  EXPECT_FALSE(builder.Build().ok());
}

TEST(PerfSessionTest, OneAttrAndNoIdBuildSucceeds) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  PerfSession::Builder builder(&context);
  perf_event_attr attr;
  attr.sample_id_all = false;
  attr.sample_type = PERF_SAMPLE_CALLCHAIN | PERF_SAMPLE_CPU | PERF_SAMPLE_TIME;
  builder.AddAttrAndIds(attr, {1});

  auto session = builder.Build();
  ASSERT_TRUE(session.ok());

  EXPECT_THAT(
      (*session)->FindAttrForRecord(perf_event_header{}, TraceBlobView()),
      IsOkAndHolds(NotNull()));
}

TEST(PerfSessionTest, MultipleAttrsAndNoIdBuildFails) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  PerfSession::Builder builder(&context);
  perf_event_attr attr;
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_CALLCHAIN | PERF_SAMPLE_CPU | PERF_SAMPLE_TIME;
  builder.AddAttrAndIds(attr, {1});
  builder.AddAttrAndIds(attr, {2});
  EXPECT_FALSE(builder.Build().ok());
}

TEST(PerfSessionTest, MultipleIdsSameAttrAndNoIdCanExtractAttrFromRecord) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  PerfSession::Builder builder(&context);
  perf_event_attr attr;
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_CPU | PERF_SAMPLE_TIME;
  builder.AddAttrAndIds(attr, {1, 2, 3});

  auto session = builder.Build();
  ASSERT_TRUE(session.ok());

  perf_event_header header;
  header.type = PERF_RECORD_SAMPLE;
  auto attr_ptr = (*session)->FindAttrForRecord(header, TraceBlobView());

  ASSERT_THAT(attr_ptr, IsOkAndHolds(NotNull()));
  EXPECT_THAT((*attr_ptr)->sample_type(), Eq(attr.sample_type));

  header.type = PERF_RECORD_MMAP2;
  attr_ptr = (*session)->FindAttrForRecord(header, TraceBlobView());

  ASSERT_THAT(attr_ptr, IsOkAndHolds(NotNull()));
  EXPECT_THAT((*attr_ptr)->sample_type(), Eq(attr.sample_type));
}

TEST(PerfSessionTest, NoCommonSampleIdAllBuildFails) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  PerfSession::Builder builder(&context);
  perf_event_attr attr;
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_IDENTIFIER;
  builder.AddAttrAndIds(attr, {1});
  builder.AddAttrAndIds(attr, {2});
  // Make sure sample_type is correct (i.e. the test is really testing the
  // sample_id_all).
  ASSERT_TRUE(builder.Build().ok());

  attr.sample_id_all = false;
  builder.AddAttrAndIds(attr, {3});
  EXPECT_FALSE(builder.Build().ok());
}

TEST(PerfSessionTest, NoCommonOffsetForSampleBuildFails) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  PerfSession::Builder builder(&context);
  perf_event_attr attr;
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ID;
  builder.AddAttrAndIds(attr, {1});
  attr.sample_type |= PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {2});
  EXPECT_FALSE(builder.Build().ok());
}

TEST(PerfSessionTest, NoCommonOffsetForNonSampleBuildFails) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  PerfSession::Builder builder(&context);
  perf_event_attr attr;
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_ID | PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {1});
  builder.AddAttrAndIds(attr, {2});
  // Make sure sample_type is correct (i.e. the test is really testing the
  // non common sample_type).
  ASSERT_TRUE(builder.Build().ok());

  attr.sample_type |= PERF_SAMPLE_IDENTIFIER;
  builder.AddAttrAndIds(attr, {3});
  EXPECT_FALSE(builder.Build().ok());
}

TEST(PerfSessionTest, NoCommonOffsetForNonSampleAndNoSampleIdAllBuildSucceeds) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  PerfSession::Builder builder(&context);
  perf_event_attr attr;
  attr.sample_id_all = false;
  attr.sample_type = PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {1});
  attr.sample_type |= PERF_SAMPLE_ID;
  builder.AddAttrAndIds(attr, {2});
  EXPECT_TRUE(builder.Build().ok());
}

TEST(PerfSessionTest, MultiplesessionBuildSucceeds) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  PerfSession::Builder builder(&context);
  perf_event_attr attr;
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ID;
  builder.AddAttrAndIds(attr, {1});
  builder.AddAttrAndIds(attr, {2});
  EXPECT_TRUE(builder.Build().ok());
}

TEST(PerfSessionTest, FindAttrInRecordWithId) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  PerfSession::Builder builder(&context);
  perf_event_attr attr;
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ID;
  attr.read_format = 1;
  builder.AddAttrAndIds(attr, {1});
  attr.read_format = 2;
  builder.AddAttrAndIds(attr, {2});

  auto session = builder.Build();
  ASSERT_TRUE(session.ok());

  struct {
    uint64_t ip = 1234;
    uint64_t id = 2;
  } data;

  perf_event_header header;
  header.type = PERF_RECORD_SAMPLE;
  auto attr_ptr = (*session)->FindAttrForRecord(
      header, TraceBlobView(TraceBlob ::CopyFrom(&data, sizeof(data))));

  ASSERT_THAT(attr_ptr, IsOkAndHolds(NotNull()));
  EXPECT_THAT((*attr_ptr)->read_format(), Eq(2u));

  header.type = PERF_RECORD_MMAP2;
  data.id = 1;
  attr_ptr = (*session)->FindAttrForRecord(
      header, TraceBlobView(TraceBlob::CopyFrom(&data, sizeof(data))));

  ASSERT_THAT(attr_ptr, IsOkAndHolds(NotNull()));
  EXPECT_THAT((*attr_ptr)->read_format(), Eq(1u));
}

TEST(PerfSessionTest, FindAttrInRecordWithIdentifier) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  PerfSession::Builder builder(&context);
  perf_event_attr attr;
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_IP;
  attr.read_format = 1;
  builder.AddAttrAndIds(attr, {1});
  attr.read_format = 2;
  builder.AddAttrAndIds(attr, {2});

  auto session = builder.Build();
  ASSERT_TRUE(session.ok());

  struct {
    uint64_t identifier = 2;
    uint64_t ip = 1234;
  } sample;

  struct {
    uint64_t ip = 1234;
    uint64_t identifier = 1;
  } mmap;

  perf_event_header header;
  header.type = PERF_RECORD_SAMPLE;
  auto attr_ptr = (*session)->FindAttrForRecord(
      header, TraceBlobView(TraceBlob ::CopyFrom(&sample, sizeof(sample))));

  ASSERT_THAT(attr_ptr, IsOkAndHolds(NotNull()));
  EXPECT_THAT((*attr_ptr)->read_format(), Eq(2u));

  header.type = PERF_RECORD_MMAP2;
  attr_ptr = (*session)->FindAttrForRecord(
      header, TraceBlobView(TraceBlob::CopyFrom(&mmap, sizeof(mmap))));

  ASSERT_THAT(attr_ptr, IsOkAndHolds(NotNull()));
  EXPECT_THAT((*attr_ptr)->read_format(), Eq(1u));
}

}  // namespace
}  // namespace perfetto::trace_processor::perf_importer
