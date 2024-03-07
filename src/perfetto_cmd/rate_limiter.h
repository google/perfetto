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

#ifndef SRC_PERFETTO_CMD_RATE_LIMITER_H_
#define SRC_PERFETTO_CMD_RATE_LIMITER_H_

namespace perfetto {

class RateLimiter {
 public:
  struct Args {
    bool is_user_build = false;
    bool is_uploading = false;
    bool allow_user_build_tracing = false;
  };
  enum ShouldTraceResponse {
    kOkToTrace,
    kNotAllowedOnUserBuild,
  };

  RateLimiter();
  virtual ~RateLimiter();

  ShouldTraceResponse ShouldTrace(const Args& args);
};

}  // namespace perfetto

#endif  // SRC_PERFETTO_CMD_RATE_LIMITER_H_
