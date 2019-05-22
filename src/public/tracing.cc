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
#include "src/public/internal/tracing_muxer_impl.h"

namespace perfetto {

// static
void Tracing::Initialize(const TracingInitArgs& args) {
  internal::TracingMuxerImpl::InitializeInstance(args);
}

//  static
std::unique_ptr<TracingSession> Tracing::NewTrace(BackendType backend) {
  return static_cast<internal::TracingMuxerImpl*>(internal::TracingMuxer::Get())
      ->CreateTracingSession(backend);
}

}  // namespace perfetto
