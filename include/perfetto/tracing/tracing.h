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

#ifndef INCLUDE_PERFETTO_TRACING_TRACING_H_
#define INCLUDE_PERFETTO_TRACING_TRACING_H_

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace perfetto {

class TracingBackend;
class Platform;
class TraceConfig;
class TracingSession;  // Declared below.

enum BackendType : uint32_t {
  kUnspecifiedBackend = 0,

  // Connects to a previously-initialized perfetto tracing backend for
  // in-process. If the in-process backend has not been previously initialized
  // it will do so and create the tracing service on a dedicated thread.
  kInProcessBackend = 1 << 0,

  // Connects to the system tracing service (e.g. on Linux/Android/Mac uses a
  // named UNIX socket).
  kSystemBackend = 1 << 1,

  // Used to provide a custom IPC transport to connect to the service.
  // TracingInitArgs::custom_backend must be non-null and point to an
  // indefinitely lived instance.
  kCustomBackend = 1 << 2,
};

struct TracingInitArgs {
  uint32_t backends = 0;                     // One or more BackendFlags.
  TracingBackend* custom_backend = nullptr;  // [Optional].

  // [Optional] Platform implementation. It allows the embedder to take control
  // of platform-specific bits like thread creation and TLS slot handling. If
  // not set it will use Platform::GetDefaultPlatform().
  Platform* platform = nullptr;
};

// The entry-point for using perfetto.
class Tracing {
 public:
  // Initializes Perfetto with the given backends in the calling process and/or
  // with a user-provided backend. Can only be called once.
  static void Initialize(const TracingInitArgs&);

  // For the moment this can be used only when initializing tracing in
  // kInProcess mode. For the system mode use the 'bin/perfetto' cmdline client.
  static std::unique_ptr<TracingSession> NewTrace(BackendType);

 private:
  Tracing() = delete;
};

class TracingSession {
 public:
  virtual ~TracingSession();

  // Configure the session passing the trace config.
  // TODO(primiano): add an error callback.
  virtual void Setup(const TraceConfig&) = 0;

  virtual void Start() = 0;

  // Use SetOnStopCallback() to get a notification when the tracing session is
  // fully stopped and all data sources have acked.
  virtual void Stop() = 0;

  // This callback will be invoked when tracing is disabled.
  // This can happen either when explicitly calling TracingSession.Stop() or
  // when the trace reaches its |duration_ms| time limit.
  // This callback will be invoked on an internal perfetto thread.
  virtual void SetOnStopCallback(std::function<void()>) = 0;

  // Struct passed as argument to the callback passed to ReadTrace().
  // [data, size] is guaranteed to contain 1 or more full trace packets, which
  // can be decoded using trace.proto. No partial or truncated packets are
  // exposed. If the trace is empty this returns a zero-sized nullptr with
  // |has_more| == true to signal EOF.
  // This callback will be invoked on an internal perfetto thread.
  struct ReadTraceCallbackArgs {
    const char* data = nullptr;
    size_t size = 0;

    // When false, this will be the last invocation of the callback for this
    // read cycle.
    bool has_more = false;
  };

  // Reads back the trace data (raw protobuf-encoded bytes) asynchronously.
  // Can be called at any point during the trace, typically but not necessarily,
  // after stopping. Reading the trace data is a destructive operation w.r.t.
  // contents of the trace buffer and is not idempotent.
  // A single ReadTrace() call can yield >1 callback invocations, until
  // |has_more| is true.
  using ReadTraceCallback = std::function<void(ReadTraceCallbackArgs)>;
  virtual void ReadTrace(ReadTraceCallback) = 0;

  // Synchronous version of ReadTrace(). It blocks the calling thread until all
  // the trace contents are read. This is slow and inefficient (involves more
  // copies) and is mainly intended for testing.
  std::vector<char> ReadTraceBlocking();
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_TRACING_H_
