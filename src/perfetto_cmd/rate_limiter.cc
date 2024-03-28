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

#include "perfetto/base/logging.h"
#include "src/perfetto_cmd/perfetto_cmd.h"

namespace perfetto {

RateLimiter::RateLimiter() = default;
RateLimiter::~RateLimiter() = default;

RateLimiter::ShouldTraceResponse RateLimiter::ShouldTrace(const Args& args) {
  // Not uploading?
  // -> We can just trace.
  if (!args.is_uploading)
    return ShouldTraceResponse::kOkToTrace;

  // If we're tracing a user build we should only trace if the override in
  // the config is set:
  if (args.is_user_build && !args.allow_user_build_tracing) {
    PERFETTO_ELOG(
        "Guardrail: allow_user_build_tracing must be set to trace on user "
        "builds");
    return ShouldTraceResponse::kNotAllowedOnUserBuild;
  }
  return ShouldTraceResponse::kOkToTrace;
}

}  // namespace perfetto
