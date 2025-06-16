/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/trace_summary/summary.h"

#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/util/descriptors.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::summary {
namespace {

using ::testing::HasSubstr;

TEST(TraceSummaryTest, DuplicateDimensionsErrorIfUnique) {
  auto tp = TraceProcessor::CreateInstance(Config{});
  tp->NotifyEndOfFile();
  DescriptorPool pool;

  std::string spec_str = R"(
    metric_spec {
      id: "my_metric"
      value: "value"
      dimensions: "dim"
      query {
        sql {
          sql: "SELECT 'a' as dim, 1.0 as value UNION ALL SELECT 'a' as dim, 2.0 as value"
          column_names: "dim"
          column_names: "value"
        }
      }
      dimension_uniqueness: UNIQUE
    }
  )";
  TraceSummarySpecBytes spec;
  spec.ptr = reinterpret_cast<const uint8_t*>(spec_str.data());
  spec.size = spec_str.size();
  spec.format = TraceSummarySpecBytes::Format::kTextProto;

  std::vector<uint8_t> output;
  TraceSummaryOutputSpec output_spec;
  output_spec.format = TraceSummaryOutputSpec::Format::kBinaryProto;

  base::Status status =
      Summarize(tp.get(), pool, {}, {spec}, &output, output_spec);

  ASSERT_FALSE(status.ok());
  EXPECT_THAT(status.message(),
              HasSubstr("Duplicate dimensions found for metric 'my_metric'"));
}

TEST(TraceSummaryTest, DuplicateDimensionsNoErrorIfNotUnique) {
  auto tp = TraceProcessor::CreateInstance(Config{});
  tp->NotifyEndOfFile();
  DescriptorPool pool;

  std::string spec_str = R"(
    metric_spec {
      id: "my_metric"
      value: "value"
      dimensions: "dim"
      query {
        sql {
          sql: "SELECT 'a' as dim, 1.0 as value UNION ALL SELECT 'a' as dim, 2.0 as value"
          column_names: "dim"
          column_names: "value"
        }
      }
    }
  )";
  TraceSummarySpecBytes spec;
  spec.ptr = reinterpret_cast<const uint8_t*>(spec_str.data());
  spec.size = spec_str.size();
  spec.format = TraceSummarySpecBytes::Format::kTextProto;

  std::vector<uint8_t> output;
  TraceSummaryOutputSpec output_spec;
  output_spec.format = TraceSummaryOutputSpec::Format::kBinaryProto;

  base::Status status =
      Summarize(tp.get(), pool, {}, {spec}, &output, output_spec);

  ASSERT_TRUE(status.ok());
}

}  // namespace
}  // namespace perfetto::trace_processor::summary
