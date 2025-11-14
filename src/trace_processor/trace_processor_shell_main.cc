/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "perfetto/base/status.h"

#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "perfetto/ext/trace_processor/trace_processor_shell.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/read_trace_internal.h"

namespace perfetto::trace_processor {

class DefaultPlatformInterface : public TraceProcessorShell::PlatformInterface {
 public:
  ~DefaultPlatformInterface() override;

  Config DefaultConfig() const override { return {}; }

  base::Status OnTraceProcessorCreated(TraceProcessor*) override {
    return base::OkStatus();
  }

  base::Status LoadTrace(
      TraceProcessor* trace_processor,
      const std::string& path,
      std::function<void(size_t)> progress_callback) override {
    return ReadTraceUnfinalized(trace_processor, path.c_str(),
                                progress_callback);
  }
};

DefaultPlatformInterface::~DefaultPlatformInterface() = default;

}  // namespace perfetto::trace_processor

int main(int argc, char** argv) {
  auto shell = perfetto::trace_processor::TraceProcessorShell::Create(
      std::make_unique<perfetto::trace_processor::DefaultPlatformInterface>());
  auto status = shell->Run(argc, argv);
  if (!status.ok()) {
    fprintf(stderr, "%s\n", status.c_message());
    return 1;
  }
  return 0;
}
