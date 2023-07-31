/*
 * Copyright (C) 2020 The Android Open Source Project
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

// This example demonstrates system-wide tracing with Perfetto.
//
// 1). To use it, first build the `tracebox` and this file. The tracebox will
// internally build tracing service (traced, which is long running
// process / daemon ) and perfetto consumer client, and many other perfetto
// tracing related tools.
// `ninja -C out/default/ tracebox example_system_wide`
//
// 2). Run traced (long running process), and open another terminal tab.
// `./out/default/tracebox traced`
//
// 3). Run this file. This is main application to trace.
// `./out/default/example_system_wide`
//
// 4). Use perfetto client to start a session and record trace in a file.
// `./out/default/tracebox perfetto -c /tmp/trace_config.txt --txt
//      -o /tmp/trace_output`
//
// but before running that command, put following trace config (protobuf config)
// in a file named `/tmp/trace_config.txt`
// This can also be copied from: https://pastebin.com/embed_iframe/ufmtBBuq
// ---------------------
// buffers: {
//     size_kb: 63488
// }
// data_sources: {
//     config {
//         name: "track_event"
//     }
// }
// duration_ms: 10000
// ---------------------
// After running the command above, trace will be saved in `/tmp/trace_output`
// file. It is a binary content. We can read it by running:
// `./tools/traceconv text /tmp/trace_output`
// Or we can use "Open Trace File" option in the perfetto UI
// (https://ui.perfetto.dev)
//
// Learn More:
// https://perfetto.dev/docs/quickstart/linux-tracing#capturing-a-trace

#include "trace_categories.h"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <thread>

namespace {

class Observer : public perfetto::TrackEventSessionObserver {
 public:
  Observer() { perfetto::TrackEvent::AddSessionObserver(this); }
  ~Observer() override { perfetto::TrackEvent::RemoveSessionObserver(this); }

  void OnStart(const perfetto::DataSourceBase::StartArgs&) override {
    std::unique_lock<std::mutex> lock(mutex);
    cv.notify_one();
  }

  void WaitForTracingStart() {
    PERFETTO_LOG("Waiting for tracing to start...");
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [] { return perfetto::TrackEvent::IsEnabled(); });
    PERFETTO_LOG("Tracing started");
  }

  std::mutex mutex;
  std::condition_variable cv;
};

void InitializePerfetto() {
  perfetto::TracingInitArgs args;
  // The backends determine where trace events are recorded. For this example we
  // are going to use the system-wide tracing service, so that we can see our
  // app's events in context with system profiling information.
  args.backends = perfetto::kSystemBackend;
  args.enable_system_consumer = false;

  perfetto::Tracing::Initialize(args);
  perfetto::TrackEvent::Register();
}

void DrawPlayer(int player_number) {
  TRACE_EVENT("rendering", "DrawPlayer", "player_number", player_number);
  // Sleep to simulate a long computation.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

void DrawGame() {
  TRACE_EVENT("rendering", "DrawGame");
  DrawPlayer(1);
  DrawPlayer(2);
}

}  // namespace

int main(int, const char**) {
  InitializePerfetto();

  Observer observer;
  observer.WaitForTracingStart();

  // Simulate some work that emits trace events.
  // Note that we don't start and stop tracing here; for system-wide tracing
  // this needs to be done through the "perfetto" command line tool or the
  // Perfetto UI (https://ui.perfetto.dev).
  DrawGame();

  // Make sure the last event is closed for this example.
  perfetto::TrackEvent::Flush();

  return 0;
}
