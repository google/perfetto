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

#include "src/perfetto_cmd/rate_limiter.h"

#include "test/gtest_and_gmock.h"

using testing::_;
using testing::Contains;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::StrictMock;

namespace perfetto {
namespace {

TEST(RateLimiterTest, CantTraceOnUser) {
  RateLimiter limiter;
  RateLimiter::Args args;

  args.is_user_build = true;
  args.allow_user_build_tracing = false;
  args.is_uploading = true;

  ASSERT_EQ(limiter.ShouldTrace(args), RateLimiter::kNotAllowedOnUserBuild);
}

TEST(RateLimiterTest, CanTraceOnUser) {
  RateLimiter limiter;
  RateLimiter::Args args;

  args.is_user_build = false;
  args.allow_user_build_tracing = false;
  args.is_uploading = true;

  ASSERT_EQ(limiter.ShouldTrace(args), RateLimiter::kOkToTrace);
}

}  // namespace

}  // namespace perfetto
