/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "perfetto/trace_processor/trace_processor.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/trace_redaction_integration_fixture.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_redaction {
namespace {
constexpr auto kTrace = "test/data/trace-redaction-perf-sample.pftrace";

constexpr auto kPackageName = "com.example.sampleapp";
constexpr auto kPid = 25131;
}  // namespace

class PrunePerfEventsIntegrationTest
    : public testing::Test,
      protected TraceRedactionIntegrationFixure {
 protected:
  void SetUp() override {
    SetSourceTrace(kTrace);

    trace_processor::Config tp_config_unredacted;
    trace_processor_unredacted_ =
        trace_processor::TraceProcessor::CreateInstance(tp_config_unredacted);
    trace_processor::Config tp_config_redacted;
    trace_processor_redacted_ =
        trace_processor::TraceProcessor::CreateInstance(tp_config_redacted);

    TraceRedactor::Config tr_config;
    auto trace_redactor = TraceRedactor::CreateInstance(tr_config);

    Context context;
    context.package_name = kPackageName;
    base::Status status = Redact(*trace_redactor, &context);
    if (!status.ok()) {
      PERFETTO_ELOG("Redaction error: %s", status.c_message());
    }
    ASSERT_OK(status);

    auto raw = LoadRedacted();
    ASSERT_OK(raw);

    auto read_buffer = std::make_unique<uint8_t[]>(raw->size());
    memcpy(read_buffer.get(), raw->data(), raw->size());

    ASSERT_OK(
        trace_processor_redacted_->Parse(std::move(read_buffer), raw->size()));
    ASSERT_OK(trace_processor_redacted_->NotifyEndOfFile());

    status = LoadTrace(GetSourceTrace(), trace_processor_unredacted_.get());
    ASSERT_OK(status);
  }

  std::unique_ptr<trace_processor::TraceProcessor> trace_processor_unredacted_;
  std::unique_ptr<trace_processor::TraceProcessor> trace_processor_redacted_;
};

TEST_F(PrunePerfEventsIntegrationTest, OnlyKeepsTargetProcessPerfSamples) {
  // This query retrieves the total number of perf samples for target process
  // in redacted trace
  auto query =
      " SELECT COUNT(*) FROM perf_sample "
      "JOIN thread ON thread.utid = perf_sample.utid "
      "JOIN process ON process.upid = thread.upid "
      "GROUP BY pid "
      "HAVING pid = " +
      std::to_string(kPid);

  auto rows = trace_processor_redacted_->ExecuteQuery(query);
  ASSERT_TRUE(rows.Next());
  int64_t perf_samples_for_target_pid = rows.Get(0).AsLong();
  ASSERT_TRUE(perf_samples_for_target_pid > 0);

  // This query retrieves the total number of perf samples for all processes
  // in redacted trace
  query =
      " SELECT COUNT(*) FROM perf_sample "
      "JOIN thread ON thread.utid = perf_sample.utid "
      "JOIN process ON process.upid = thread.upid";
  rows = trace_processor_redacted_->ExecuteQuery(query);
  ASSERT_TRUE(rows.Next());
  int64_t trace_perf_samples = rows.Get(0).AsLong();

  // Check that all the trace samples in the trace correspond to target process.
  ASSERT_TRUE(perf_samples_for_target_pid == trace_perf_samples);

  ASSERT_OK(rows.Status());
}

TEST_F(PrunePerfEventsIntegrationTest,
       TargetProcessPerfSamplesMatchesUnredacted) {
  auto query =
      " SELECT COUNT(*) FROM perf_sample "
      "JOIN thread ON thread.utid = perf_sample.utid "
      "JOIN process ON process.upid = thread.upid "
      "GROUP BY pid "
      "HAVING pid = " +
      std::to_string(kPid);

  auto rows = trace_processor_unredacted_->ExecuteQuery(query);
  ASSERT_TRUE(rows.Next());
  int64_t unredacted_target_process_samples = rows.Get(0).AsLong();

  rows = trace_processor_redacted_->ExecuteQuery(query);
  ASSERT_TRUE(rows.Next());
  int64_t redacted_target_process_samples = rows.Get(0).AsLong();

  // Check that all the trace samples in the trace correspond to target process.
  ASSERT_TRUE(unredacted_target_process_samples ==
              redacted_target_process_samples);

  ASSERT_OK(rows.Status());
}

}  // namespace perfetto::trace_redaction
