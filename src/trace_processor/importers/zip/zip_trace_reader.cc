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

#include "src/trace_processor/importers/zip/zip_trace_reader.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/forwarding_trace_parser.h"
#include "src/trace_processor/importers/android_bugreport/android_bugreport_reader.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"
#include "src/trace_processor/util/trace_type.h"
#include "src/trace_processor/util/zip_reader.h"

namespace perfetto::trace_processor {

ZipTraceReader::ZipTraceReader(TraceProcessorContext* context)
    : context_(context) {}
ZipTraceReader::~ZipTraceReader() = default;

bool ZipTraceReader::Entry::operator<(const Entry& rhs) const {
  // Traces with symbols should be the last ones to be read.
  // TODO(carlscab): Proto traces with just ModuleSymbols packets should be an
  // exception. We actually need those are the very end (once whe have all the
  // Frames). Alternatively we could build a map address -> symbol during
  // tokenization and use this during parsing to resolve symbols.
  if (trace_type == kSymbolsTraceType) {
    return false;
  }
  if (rhs.trace_type == kSymbolsTraceType) {
    return true;
  }

  // Proto traces should always parsed first as they might contains clock sync
  // data needed to correctly parse other traces.
  if (rhs.trace_type == TraceType::kProtoTraceType) {
    return false;
  }
  if (trace_type == TraceType::kProtoTraceType) {
    return true;
  }

  if (rhs.trace_type == TraceType::kGzipTraceType) {
    return false;
  }
  if (trace_type == TraceType::kGzipTraceType) {
    return true;
  }

  return std::tie(name, index) < std::tie(rhs.name, rhs.index);
}

base::Status ZipTraceReader::Parse(TraceBlobView blob) {
  return zip_reader_.Parse(std::move(blob));
}

base::Status ZipTraceReader::NotifyEndOfFile() {
  std::vector<util::ZipFile> files = zip_reader_.TakeFiles();

  // Android bug reports are ZIP files and its files do not get handled
  // separately.
  if (AndroidBugreportReader::IsAndroidBugReport(files)) {
    return AndroidBugreportReader::Parse(context_, std::move(files));
  }

  ASSIGN_OR_RETURN(std::vector<Entry> entries,
                   ExtractEntries(std::move(files)));
  std::sort(entries.begin(), entries.end());

  for (Entry& e : entries) {
    ScopedActiveTraceFile trace_file =
        context_->trace_file_tracker->StartNewFile(e.name, e.trace_type,
                                                   e.uncompressed_data.size());

    auto chunk_reader = std::make_unique<ForwardingTraceParser>(context_);
    auto& parser = *chunk_reader;
    context_->chunk_readers.push_back(std::move(chunk_reader));

    RETURN_IF_ERROR(parser.Parse(std::move(e.uncompressed_data)));
    RETURN_IF_ERROR(parser.NotifyEndOfFile());

    // Make sure the ForwardingTraceParser determined the same trace type as we
    // did.
    PERFETTO_CHECK(parser.trace_type() == e.trace_type);
  }
  return base::OkStatus();
}

base::StatusOr<std::vector<ZipTraceReader::Entry>>
ZipTraceReader::ExtractEntries(std::vector<util::ZipFile> files) {
  // TODO(carlsacab): There is a lot of unnecessary copying going on here.
  // ZipTraceReader can directly parse the ZIP file and given that we know the
  // decompressed size we could directly decompress into TraceBlob chunks and
  // send them to the tokenizer.
  std::vector<Entry> entries;
  std::vector<uint8_t> buffer;
  for (size_t i = 0; i < files.size(); ++i) {
    const util::ZipFile& zip_file = files[i];
    Entry entry;
    entry.name = zip_file.name();
    entry.index = i;
    RETURN_IF_ERROR(files[i].Decompress(&buffer));
    entry.uncompressed_data =
        TraceBlobView(TraceBlob::CopyFrom(buffer.data(), buffer.size()));
    entry.trace_type = GuessTraceType(entry.uncompressed_data.data(),
                                      entry.uncompressed_data.size());
    entries.push_back(std::move(entry));
  }
  return std::move(entries);
}

}  // namespace perfetto::trace_processor
