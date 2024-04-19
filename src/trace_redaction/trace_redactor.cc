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

#include "src/trace_redaction/trace_redactor.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/scoped_mmap.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/util/status_macros.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/trace.pbzero.h"

namespace perfetto::trace_redaction {

using Trace = protos::pbzero::Trace;
using TracePacket = protos::pbzero::TracePacket;

TraceRedactor::TraceRedactor() = default;

TraceRedactor::~TraceRedactor() = default;

base::Status TraceRedactor::Redact(std::string_view source_filename,
                                   std::string_view dest_filename,
                                   Context* context) const {
  const std::string source_filename_str(source_filename);
  base::ScopedMmap mapped =
      base::ReadMmapWholeFile(source_filename_str.c_str());
  if (!mapped.IsValid()) {
    return base::ErrStatus("TraceRedactor: failed to map pages for trace (%s)",
                           source_filename_str.c_str());
  }

  trace_processor::TraceBlobView whole_view(
      trace_processor::TraceBlob::FromMmap(std::move(mapped)));

  RETURN_IF_ERROR(Collect(context, whole_view));

  for (const auto& builder : builders_) {
    RETURN_IF_ERROR(builder->Build(context));
  }

  return Transform(*context, whole_view, std::string(dest_filename));
}

base::Status TraceRedactor::Collect(
    Context* context,
    const trace_processor::TraceBlobView& view) const {
  for (const auto& collector : collectors_) {
    RETURN_IF_ERROR(collector->Begin(context));
  }

  const Trace::Decoder trace_decoder(view.data(), view.length());

  for (auto packet_it = trace_decoder.packet(); packet_it; ++packet_it) {
    const TracePacket::Decoder packet(packet_it->as_bytes());

    for (auto& collector : collectors_) {
      RETURN_IF_ERROR(collector->Collect(packet, context));
    }
  }

  for (const auto& collector : collectors_) {
    RETURN_IF_ERROR(collector->End(context));
  }

  return base::OkStatus();
}

base::Status TraceRedactor::Transform(
    const Context& context,
    const trace_processor::TraceBlobView& view,
    const std::string& dest_file) const {
  std::ignore = context;
  const auto dest_fd = base::OpenFile(dest_file, O_RDWR | O_CREAT, 0666);

  if (dest_fd.get() == -1) {
    return base::ErrStatus(
        "Failed to open destination file; can't write redacted trace.");
  }

  const Trace::Decoder trace_decoder(view.data(), view.length());
  for (auto packet_it = trace_decoder.packet(); packet_it; ++packet_it) {
    auto packet = packet_it->as_std_string();

    for (const auto& transformer : transformers_) {
      // If the packet has been cleared, it means a tranformation has removed it
      // from the trace. Stop processing it. This saves transforms from having
      // to check and handle empty packets.
      if (packet.empty()) {
        break;
      }

      RETURN_IF_ERROR(transformer->Transform(context, &packet));
    }

    // The packet has been removed from the trace. Don't write an empty packet
    // to disk.
    if (packet.empty()) {
      continue;
    }

    protozero::HeapBuffered<protos::pbzero::Trace> serializer;
    serializer->add_packet()->AppendRawProtoBytes(packet.data(), packet.size());
    packet.assign(serializer.SerializeAsString());

    if (const auto exported_data =
            base::WriteAll(dest_fd.get(), packet.data(), packet.size());
        exported_data <= 0) {
      return base::ErrStatus(
          "TraceRedactor: failed to write redacted trace to disk");
    }
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
