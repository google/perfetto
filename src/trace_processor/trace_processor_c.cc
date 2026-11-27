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

#include "perfetto/trace_processor/trace_processor_c.h"

#include <memory>
#include <string>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "perfetto/trace_processor/trace_processor.h"

namespace ptp = perfetto::trace_processor;

struct PerfettoTp {
  std::unique_ptr<ptp::TraceProcessor> impl;
};

struct PerfettoTpIterator {
  explicit PerfettoTpIterator(ptp::Iterator it) : impl(std::move(it)) {}

  ptp::Iterator impl;
  std::string column_name;
};

struct PerfettoTpError {
  std::string message;
};

namespace {

PerfettoTpError* ToError(const perfetto::base::Status& status) {
  if (status.ok())
    return nullptr;
  return new PerfettoTpError{status.message()};
}

}  // namespace

const char* PerfettoTpErrorMessage(const PerfettoTpError* err) {
  return err->message.c_str();
}

void PerfettoTpErrorDestroy(PerfettoTpError* err) {
  delete err;
}

PerfettoTp* PerfettoTpCreate(void) {
  return new PerfettoTp{ptp::TraceProcessor::CreateInstance(ptp::Config())};
}

void PerfettoTpDestroy(PerfettoTp* tp) {
  delete tp;
}

PerfettoTpError* PerfettoTpParse(PerfettoTp* tp,
                                 const uint8_t* data,
                                 size_t size,
                                 void* ctx,
                                 PerfettoTpDeleter deleter) {
  if (!deleter)
    deleter = [](void*) {};
  return ToError(tp->impl->Parse(
      ptp::TraceBlobView(ptp::TraceBlob::Adopt(data, size, ctx, deleter))));
}

PerfettoTpError* PerfettoTpNotifyEndOfFile(PerfettoTp* tp) {
  return ToError(tp->impl->NotifyEndOfFile());
}

PerfettoTpIterator* PerfettoTpExecuteQuery(PerfettoTp* tp, const char* sql) {
  return new PerfettoTpIterator(tp->impl->ExecuteQuery(sql));
}

bool PerfettoTpIteratorNext(PerfettoTpIterator* it) {
  return it->impl.Next();
}

PerfettoTpError* PerfettoTpIteratorStatus(PerfettoTpIterator* it) {
  return ToError(it->impl.Status());
}

uint32_t PerfettoTpIteratorColumnCount(PerfettoTpIterator* it) {
  return it->impl.ColumnCount();
}

const char* PerfettoTpIteratorColumnName(PerfettoTpIterator* it, uint32_t col) {
  it->column_name = it->impl.GetColumnName(col);
  return it->column_name.c_str();
}

PerfettoTpValueType PerfettoTpIteratorGetType(PerfettoTpIterator* it,
                                              uint32_t col) {
  switch (it->impl.Get(col).type) {
    case ptp::SqlValue::kNull:
      return PERFETTO_TP_VALUE_TYPE_NULL;
    case ptp::SqlValue::kLong:
      return PERFETTO_TP_VALUE_TYPE_INT64;
    case ptp::SqlValue::kDouble:
      return PERFETTO_TP_VALUE_TYPE_DOUBLE;
    case ptp::SqlValue::kString:
      return PERFETTO_TP_VALUE_TYPE_STRING;
    case ptp::SqlValue::kBytes:
      return PERFETTO_TP_VALUE_TYPE_BYTES;
  }
  PERFETTO_FATAL("Unknown SqlValue type");
}

int64_t PerfettoTpIteratorGetInt64(PerfettoTpIterator* it, uint32_t col) {
  return it->impl.Get(col).AsLong();
}

double PerfettoTpIteratorGetDouble(PerfettoTpIterator* it, uint32_t col) {
  return it->impl.Get(col).AsDouble();
}

const char* PerfettoTpIteratorGetString(PerfettoTpIterator* it, uint32_t col) {
  return it->impl.Get(col).AsString();
}

const uint8_t* PerfettoTpIteratorGetBytes(PerfettoTpIterator* it,
                                          uint32_t col,
                                          size_t* size) {
  ptp::SqlValue v = it->impl.Get(col);
  const uint8_t* data = static_cast<const uint8_t*>(v.AsBytes());
  *size = v.bytes_count;
  return data;
}

void PerfettoTpIteratorDestroy(PerfettoTpIterator* it) {
  delete it;
}
