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

#include "src/trace_processor/importers/archive/archive_entry.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <tuple>

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/proto/proto_trace_tokenizer.h"
#include "src/trace_processor/util/trace_type.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor {

bool IsHiddenArchivePath(const std::string& path) {
  size_t start = 0;
  while (start <= path.size()) {
    size_t slash = path.find('/', start);
    size_t end = slash == std::string::npos ? path.size() : slash;
    // Inspect the component [start, end).
    if (end > start && path[start] == '.') {
      // "." and ".." are current/parent directory segments, not hidden files.
      bool is_dot = (end - start) == 1;
      bool is_dot_dot = (end - start) == 2 && path[start + 1] == '.';
      if (!is_dot && !is_dot_dot) {
        return true;
      }
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
  return false;
}

bool ProtoTraceMayHaveClockSnapshot(const TraceBlobView* begin,
                                    const TraceBlobView* end) {
  using protos::pbzero::TracePacket;
  bool found = false;
  auto check_packet = [&found](TraceBlobView packet) {
    // Only the field numbers of the packet are read, not its payloads.
    protozero::ProtoDecoder decoder(packet.data(), packet.size());
    for (protozero::Field f = decoder.ReadField(); !found && f.valid();
         f = decoder.ReadField()) {
      found = f.id() == TracePacket::kClockSnapshotFieldNumber ||
              f.id() == TracePacket::kCompressedPacketsFieldNumber ||
              f.id() == TracePacket::kZstdCompressedPacketsFieldNumber;
    }
    return base::OkStatus();
  };
  // The tokenizer is fed slices rather than whole chunks, so that the walk
  // stops soon after the first match even in a large chunk. A packet split
  // across slices or chunks is stitched together by the tokenizer.
  constexpr size_t kSliceSize = 1024 * 1024;
  ProtoTraceTokenizer tokenizer;
  for (const TraceBlobView* chunk = begin; chunk != end && !found; ++chunk) {
    for (size_t off = 0; off < chunk->size() && !found; off += kSliceSize) {
      size_t size = std::min(kSliceSize, chunk->size() - off);
      if (!tokenizer.Tokenize(chunk->slice_off(off, size), check_packet).ok()) {
        return false;
      }
    }
  }
  return found;
}

int ArchiveEntry::ComputePriority(TraceImporterId type,
                                  const TraceImporterRegistry& registry) {
  return registry.Find(type)->archive_priority;
}

bool ArchiveEntry::ComputeLacksClockSnapshot(
    TraceImporterId type,
    const TraceImporterRegistry& registry,
    const TraceBlobView* begin,
    const TraceBlobView* end) {
  return registry.Find(type)->archive_clock_snapshot_first &&
         !ProtoTraceMayHaveClockSnapshot(begin, end);
}

bool ArchiveEntry::operator<(const ArchiveEntry& rhs) const {
  // Compare first by archive priority, then members with a ClockSnapshot
  // before those without, then by name, and finally by index to ensure strict
  // ordering.
  return std::tie(priority, lacks_clock_snapshot, name, index) <
         std::tie(rhs.priority, rhs.lacks_clock_snapshot, rhs.name, rhs.index);
}

}  // namespace perfetto::trace_processor
