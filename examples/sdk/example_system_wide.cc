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

#include "trace_categories.h"

#include <chrono>
#include <fstream>
#include <thread>

void InitializePerfetto() {
  perfetto::TracingInitArgs args;
  // The backends determine where trace events are recorded. For this example we
  // are going to use the system-wide tracing service, so that we can see our
  // app's events in context with system profiling information.
  args.backends = perfetto::kSystemBackend;

  perfetto::Tracing::Initialize(args);
  perfetto::TrackEvent::Register();
}

void WaitForTracingStart() {
  PERFETTO_LOG("Waiting for tracing to start...");
  while (!TRACE_EVENT_CATEGORY_ENABLED("rendering")) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  PERFETTO_LOG("Tracing started");
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

int main(int, const char**) {
  InitializePerfetto();
  WaitForTracingStart();

  // Simulate some work that emits trace events.
  // Note that we don't start and stop tracing here; for system-wide tracing
  // this needs to be done through the "perfetto" command line tool or the
  // Perfetto UI (https://ui.perfetto.dev).
  DrawGame();

  // Make sure the last event is closed for this example.
  perfetto::TrackEvent::Flush();

  return 0;
}
