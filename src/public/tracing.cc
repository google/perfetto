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

#include "perfetto/public/tracing.h"

namespace perfetto {

// static
void Tracing::Initialize(const TracingInitArgs& args) {
  base::ignore_result(args);
  // TOOD(primiano): Fill in next CL, forward call to
  // internal::TracingMuxerImpl::InitializeInstance(args);
}

//  static
std::unique_ptr<TracingSession> Tracing::NewTrace(BackendType backend) {
  base::ignore_result(backend);
  return nullptr;
  // TOOD(primiano): Fill in next CL, forward call to
  // internal::TracingMuxerImpl::CreateTracingSession().
}

}  // namespace perfetto
