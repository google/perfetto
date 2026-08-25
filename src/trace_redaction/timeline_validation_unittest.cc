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

#include "src/trace_redaction/timeline_validation.h"

#include <memory>

#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_redaction {

TEST(TimelineValidationTest, NullTimelineReturnsError) {
  TimelineValidation validator;
  Context context;
  context.timeline = nullptr;

  auto status = validator.Validate(context);
  ASSERT_FALSE(status.ok());
  ASSERT_EQ(status.message(),
            "TraceRedactor: No process timeline found. Are sched_free or "
            "process stats data sources missing");
}

TEST(TimelineValidationTest, EmptyTimelineReturnsError) {
  TimelineValidation validator;
  Context context;
  context.timeline = std::make_unique<ProcessThreadTimeline>();

  ASSERT_TRUE(context.timeline->empty());

  auto status = validator.Validate(context);
  ASSERT_FALSE(status.ok());
  ASSERT_EQ(status.message(),
            "TraceRedactor: No process timeline found. Are sched_free or "
            "process stats data sources missing");
}

TEST(TimelineValidationTest, NonEmptyTimelineReturnsOk) {
  TimelineValidation validator;
  Context context;
  context.timeline = std::make_unique<ProcessThreadTimeline>();
  context.timeline->Append(ProcessThreadTimeline::Event::Open(100, 1, 0, 1000));
  context.timeline->Sort();

  ASSERT_FALSE(context.timeline->empty());

  auto status = validator.Validate(context);
  ASSERT_OK(status);
}

}  // namespace perfetto::trace_redaction
