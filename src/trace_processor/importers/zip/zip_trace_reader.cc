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
#include <cinttypes>
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
#include "src/trace_processor/importers/android_bugreport/android_bugreport_parser.h"
#include "src/trace_processor/importers/proto/proto_trace_tokenizer.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"
#include "src/trace_processor/util/trace_type.h"

namespace perfetto {
namespace trace_processor {
namespace {

// Proto traces should always parsed first as they might contains clock sync
// data needed to correctly parse other traces.
// The rest of the types are sorted by position in the enum but this is not
// something users should rely on.
// TODO(carlscab): Proto traces with just ModuleSymbols packets should be an
// exception. We actually need those are the very end (once whe have all the
// Frames). Alternatively we could build a map address -> symbol during
// tokenization and use this during parsing to resolve symbols.
bool CompareTraceType(TraceType lhs, TraceType rhs) {
  if (rhs == TraceType::kProtoTraceType) {
    return false;
  }
  if (lhs == TraceType::kProtoTraceType) {
    return true;
  }
  return lhs < rhs;
}

bool HasSymbols(const TraceBlobView& blob) {
  bool has_symbols = false;
  ProtoTraceTokenizer().Tokenize(blob.copy(), [&](TraceBlobView raw) {
    protos::pbzero::TracePacket::Decoder packet(raw.data(), raw.size());
    has_symbols = packet.has_module_symbols();
    return base::ErrStatus("break");
  });
  return has_symbols;
}

}  // namespace

ZipTraceReader::ZipTraceReader(TraceProcessorContext* context)
    : context_(context) {}
ZipTraceReader::~ZipTraceReader() = default;

bool ZipTraceReader::Entry::operator<(const Entry& rhs) const {
  // Traces with symbols should be the last ones to be read.
  if (has_symbols) {
    return false;
  }
  if (rhs.has_symbols) {
    return true;
  }
  if (CompareTraceType(trace_type, rhs.trace_type)) {
    return true;
  }
  if (CompareTraceType(rhs.trace_type, trace_type)) {
    return false;
  }
  return std::tie(name, index) < std::tie(rhs.name, rhs.index);
}

util::Status ZipTraceReader::Parse(TraceBlobView blob) {
  zip_reader_.Parse(blob.data(), blob.size());
  return base::OkStatus();
}

void ZipTraceReader::NotifyEndOfFile() {
  base::Status status = NotifyEndOfFileImpl();
  if (!status.ok()) {
    PERFETTO_ELOG("ZipTraceReader failed: %s", status.c_message());
  }
}

base::Status ZipTraceReader::NotifyEndOfFileImpl() {
  std::vector<util::ZipFile> files = zip_reader_.TakeFiles();

  // Android bug reports are ZIP files and its files do not get handled
  // separately.
  if (AndroidBugreportParser::IsAndroidBugReport(files)) {
    return AndroidBugreportParser::Parse(context_, std::move(files));
  }

  base::StatusOr<std::vector<Entry>> entries = ExtractEntries(std::move(files));
  if (!entries.ok()) {
    return entries.status();
  }
  std::sort(entries->begin(), entries->end());

  for (Entry& e : *entries) {
    parsers_.push_back(std::make_unique<ForwardingTraceParser>(context_));
    auto& parser = *parsers_.back();
    RETURN_IF_ERROR(parser.Parse(std::move(e.uncompressed_data)));
    parser.NotifyEndOfFile();
  }
  return base::OkStatus();
}

base::StatusOr<std::vector<ZipTraceReader::Entry>>
ZipTraceReader::ExtractEntries(std::vector<util::ZipFile> files) const {
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
    entry.has_symbols = entry.trace_type == TraceType::kProtoTraceType &&
                        HasSymbols(entry.uncompressed_data);
    entries.push_back(std::move(entry));
  }
  return std::move(entries);
}

}  // namespace trace_processor
}  // namespace perfetto
