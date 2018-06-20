/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_TRACING_CORE_CONSUMER_H_
#define INCLUDE_PERFETTO_TRACING_CORE_CONSUMER_H_

#include "perfetto/tracing/core/basic_types.h"

#include <vector>

#include "perfetto/base/export.h"

namespace perfetto {

class TracePacket;

class PERFETTO_EXPORT Consumer {
 public:
  virtual ~Consumer();

  // Called by Service (or more typically by the transport layer, on behalf of
  // the remote Service), once the Consumer <> Service connection has been
  // established.
  virtual void OnConnect() = 0;

  // Called by the Service or by the transport layer if the connection with the
  // service drops, either voluntarily (e.g., by destroying the ConsumerEndpoint
  // obtained through Service::ConnectConsumer()) or involuntarily (e.g., if the
  // Service process crashes).
  virtual void OnDisconnect() = 0;

  // Called by the Service after the tracing session has ended. This can happen
  // for a variety of reasons:
  // - The consumer explicitly called DisableTracing()
  // - The TraceConfig's |duration_ms| has been reached.
  // - The TraceConfig's |max_file_size_bytes| has been reached.
  // - An error occurred while trying to enable tracing.
  virtual void OnTracingDisabled() = 0;

  // Called back by the Service (or transport layer) after invoking
  // TracingService::ConsumerEndpoint::ReadBuffers(). This function can be
  // called more than once. Each invocation can carry one or more
  // TracePacket(s). Upon the last call, |has_more| is set to true (i.e.
  // |has_more| is a !EOF).
  virtual void OnTraceData(std::vector<TracePacket>, bool has_more) = 0;
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_CORE_CONSUMER_H_
