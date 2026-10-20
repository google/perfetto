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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ARCHIVE_ARCHIVE_ENTRY_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ARCHIVE_ARCHIVE_ENTRY_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "src/trace_processor/util/trace_type.h"

namespace perfetto::trace_processor {

struct TraceManifestState;

// Returns true if `path` should be ignored when reading an archive (tar/zip),
// i.e. if any of its path components starts with a '.' (other than the "." and
// ".." path segments). Such entries are hidden files and directories which are
// never traces: the most common cases are the AppleDouble resource-fork files
// ("._foo") and ".DS_Store" entries that macOS/BSD tar and Finder-created zips
// sprinkle alongside the real files. Ignoring them lets archives created on
// macOS load without spurious "unknown trace type" failures.
bool IsHiddenArchivePath(const std::string& path);

// The sort key of one archive member. Archive readers key every member, sort
// and then read them one at a time in that order.
struct ArchiveEntry {
  // Returns the archive ordering priority for `type` (lower is read first):
  // manifest < proto < containers < others < symbols.
  static int ComputePriority(TraceImporterId type,
                             const TraceImporterRegistry& registry);

  // Returns the `priority` of the perfetto_manifest entry for the member
  // `name`, or 0 if the manifest has no entry for it.
  static int64_t ComputeManifestPriority(const TraceManifestState& manifest,
                                         const std::string& name);

  // File name. Used to break ties.
  std::string name;
  // Position. Used to break ties.
  size_t index;
  // Trace type. Kept for the reader to cross-check the detected type.
  TraceImporterId trace_type;
  // Archive ordering priority derived from trace_type (see ComputePriority).
  // This is the main attribute traces are ordered by; proto is read first as it
  // may contain clock sync data needed to correctly parse other traces.
  int priority;
  // Rank among the members of the same `priority` (lower is read first), set
  // by the member's perfetto_manifest entry (see ComputeManifestPriority). It
  // lets e.g. a system trace be read before an in-process SDK trace which has
  // no ClockSnapshot of its own. It cannot move a member ahead of one with a
  // lower `priority`, as that order also decides which trace is read first:
  // the primary trace of its machine, whose clocks the traces without a
  // ClockSnapshot are converted through (see ClockTracker), and the one that
  // claims the trace time clock.
  int64_t manifest_priority = 0;
  // Comparator used to determine the order in which archive members are read.
  bool operator<(const ArchiveEntry& rhs) const;
};
}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ARCHIVE_ARCHIVE_ENTRY_H_
