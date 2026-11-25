/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>

#include "perfetto/base/compiler.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "src/trace_redaction/trace_redactor.h"

namespace perfetto::trace_redaction {
namespace {

// Traces come from devices, so the redactor must handle arbitrary bytes
// without crashing.
int FuzzTraceRedactor(const uint8_t* data, size_t size) {
  auto redactor = TraceRedactor::CreateInstance(TraceRedactor::Config());
  Context context;
  context.package_name = "com.example.target";
  std::string output;
  base::ignore_result(redactor->RedactBuffer(
      std::string_view(reinterpret_cast<const char*>(data), size), &output,
      &context));
  return 0;
}

}  // namespace
}  // namespace perfetto::trace_redaction

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  return perfetto::trace_redaction::FuzzTraceRedactor(data, size);
}
