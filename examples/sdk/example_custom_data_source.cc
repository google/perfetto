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

// This example demonstrates a custom tracing data source.

// This source file can be built in two ways:
// 1. As part of the regular GN build, against standard includes.
// 2. To test that the amalgmated SDK works, against the perfetto.h source.
#if defined(PERFETTO_SDK_EXAMPLE_USE_INTERNAL_HEADERS)
#include "perfetto/tracing.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/data_source.h"
#include "perfetto/tracing/tracing.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#else
#include <perfetto.h>
#endif

#include <fstream>
#include <thread>

namespace {

// The definition of our custom data source. Instances of this class will be
// automatically created and destroyed by Perfetto.
class CustomDataSource : public perfetto::DataSource<CustomDataSource> {
 public:
  void OnSetup(const SetupArgs&) override {
    // Use this callback to apply any custom configuration to your data source
    // based on the TraceConfig in SetupArgs.
  }

  // Optional callbacks for tracking the lifecycle of the data source.
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs&) override {}
};

void InitializePerfetto() {
  perfetto::TracingInitArgs args;
  // The backends determine where trace events are recorded. For this example we
  // are going to use the in-process tracing service, which only includes in-app
  // events.
  args.backends = perfetto::kInProcessBackend;
  perfetto::Tracing::Initialize(args);

  // Register our custom data source. Only the name is required, but other
  // properties can be advertised too.
  perfetto::DataSourceDescriptor dsd;
  dsd.set_name("com.example.custom_data_source");
  CustomDataSource::Register(dsd);
}

std::unique_ptr<perfetto::TracingSession> StartTracing() {
  // The trace config defines which types of data sources are enabled for
  // recording. In this example we enable the custom data source we registered
  // above.
  perfetto::TraceConfig cfg;
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("com.example.custom_data_source");

  auto tracing_session = perfetto::Tracing::NewTrace();
  tracing_session->Setup(cfg);
  tracing_session->StartBlocking();
  return tracing_session;
}

void StopTracing(std::unique_ptr<perfetto::TracingSession> tracing_session) {
  // Flush to make sure the last written event ends up in the trace.
  CustomDataSource::Trace(
      [](CustomDataSource::TraceContext ctx) { ctx.Flush(); });

  // Stop tracing and read the trace data.
  tracing_session->StopBlocking();
  std::vector<char> trace_data(tracing_session->ReadTraceBlocking());

  // Write the result into a file.
  // Note: To save memory with longer traces, you can tell Perfetto to write
  // directly into a file by passing a file descriptor into Setup() above.
  std::ofstream output;
  const char* filename = "example_custom_data_source.pftrace";
  output.open(filename, std::ios::out | std::ios::binary);
  output.write(&trace_data[0], static_cast<std::streamsize>(trace_data.size()));
  output.close();
  PERFETTO_LOG(
      "Trace written in %s file. To read this trace in "
      "text form, run `./tools/traceconv text %s`",
      filename, filename);
}

}  // namespace

PERFETTO_DECLARE_DATA_SOURCE_STATIC_MEMBERS(CustomDataSource);
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(CustomDataSource);

int main(int, const char**) {
  InitializePerfetto();
  auto tracing_session = StartTracing();

  // Write an event using our custom data source.
  CustomDataSource::Trace([](CustomDataSource::TraceContext ctx) {
    auto packet = ctx.NewTracePacket();
    packet->set_timestamp(42);
    packet->set_for_testing()->set_str("Hello world!");
  });

  StopTracing(std::move(tracing_session));
  return 0;
}
