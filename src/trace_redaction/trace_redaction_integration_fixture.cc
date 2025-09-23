/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_redaction/trace_redaction_integration_fixture.h"

#include "perfetto/ext/base/file_utils.h"
#include "src/base/test/utils.h"

namespace perfetto::trace_redaction {

TraceRedactionIntegrationFixure::TraceRedactionIntegrationFixure() {
  dest_trace_ = tmp_dir_.AbsolutePath("dst.pftrace");

  // TODO: Most of the tests were written usng this trace. Those tests make a
  // lot of assumption around using this trace. Those tests should be
  // transitioned to the other constructor to remove this assumption.
  SetSourceTrace("test/data/trace-redaction-general.pftrace");
}

void TraceRedactionIntegrationFixure::SetSourceTrace(
    std::string_view source_file) {
  src_trace_ = base::GetTestDataPath(std::string(source_file));
}

base::Status TraceRedactionIntegrationFixure::Redact(
    const TraceRedactor& redactor,
    Context* context) {
  auto status = redactor.Redact(src_trace_, dest_trace_, context);

  if (status.ok()) {
    tmp_dir_.TrackFile("dst.pftrace");
  }

  return status;
}

base::StatusOr<std::string> TraceRedactionIntegrationFixure::LoadOriginal()
    const {
  return ReadRawTrace(src_trace_);
}

base::StatusOr<std::string> TraceRedactionIntegrationFixure::LoadRedacted()
    const {
  return ReadRawTrace(dest_trace_);
}

base::StatusOr<std::string> TraceRedactionIntegrationFixure::ReadRawTrace(
    const std::string& path) const {
  std::string redacted_buffer;

  if (base::ReadFile(path, &redacted_buffer)) {
    return redacted_buffer;
  }

  return base::ErrStatus("Failed to read %s", path.c_str());
}

base::Status TraceRedactionIntegrationFixure::LoadTrace(
    std::string trace_path,
    trace_processor::TraceProcessor* trace_processor) const {
  auto raw_trace = ReadRawTrace(trace_path);
  if (!raw_trace.ok()) {
    return raw_trace.status();
  }

  auto read_buffer = std::make_unique<uint8_t[]>(raw_trace->size());
  memcpy(read_buffer.get(), raw_trace->data(), raw_trace->size());

  RETURN_IF_ERROR(
      trace_processor->Parse(std::move(read_buffer), raw_trace->size()));
  RETURN_IF_ERROR(trace_processor->NotifyEndOfFile());

  return base::OkStatus();
}

std::string TraceRedactionIntegrationFixure::GetSourceTrace() {
  return src_trace_;
}

}  // namespace perfetto::trace_redaction
