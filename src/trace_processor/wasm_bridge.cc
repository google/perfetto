/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <emscripten/emscripten.h>
#include <map>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/trace_processor.h"

#include "perfetto/trace_processor/raw_query.pb.h"
#include "perfetto/trace_processor/sched.pb.h"

namespace perfetto {
namespace trace_processor {

using RequestID = uint32_t;

// Reply(): replies to a RPC method invocation.
// Called asynchronously (i.e. in a separate task) by the C++ code inside the
// trace processor to return data for a RPC method call.
// The function is generic and thankfully we need just one for all methods
// because the output is always a protobuf buffer.
// Args:
//  RequestID: the ID passed by the embedder when invoking the RPC method (e.g.,
//             the first argument passed to sched_getSchedEvents()).
using ReplyFunction = void (*)(RequestID,
                               bool success,
                               const char* /*proto_reply_data*/,
                               uint32_t /*len*/);

namespace {
TraceProcessor* g_trace_processor;
ReplyFunction g_reply;
}  // namespace
// +---------------------------------------------------------------------------+
// | Exported functions called by the JS/TS running in the worker.             |
// +---------------------------------------------------------------------------+
extern "C" {

void EMSCRIPTEN_KEEPALIVE Initialize(ReplyFunction);
void Initialize(ReplyFunction reply_function) {
  PERFETTO_ILOG("Initializing WASM bridge");
  Config config;
  g_trace_processor = TraceProcessor::CreateInstance(config).release();
  g_reply = reply_function;
}

void EMSCRIPTEN_KEEPALIVE trace_processor_parse(RequestID,
                                                const uint8_t*,
                                                uint32_t);
void trace_processor_parse(RequestID id, const uint8_t* data, size_t size) {
  // TODO(primiano): This copy is extremely unfortunate. Ideally there should be
  // a way to take the Blob coming from JS (either from FileReader or from th
  // fetch() stream) and move into WASM.
  // See https://github.com/WebAssembly/design/issues/1162.
  std::unique_ptr<uint8_t[]> buf(new uint8_t[size]);
  memcpy(buf.get(), data, size);
  g_trace_processor->Parse(std::move(buf), size);
  g_reply(id, true, "", 0);
}

// We keep the same signature as other methods even though we don't take input
// arguments for simplicity.
void EMSCRIPTEN_KEEPALIVE trace_processor_notifyEof(RequestID,
                                                    const uint8_t*,
                                                    uint32_t);
void trace_processor_notifyEof(RequestID id, const uint8_t*, uint32_t size) {
  PERFETTO_DCHECK(!size);
  g_trace_processor->NotifyEndOfFile();
  g_reply(id, true, "", 0);
}

void EMSCRIPTEN_KEEPALIVE trace_processor_rawQuery(RequestID,
                                                   const uint8_t*,
                                                   int);
void trace_processor_rawQuery(RequestID id,
                              const uint8_t* query_data,
                              int len) {
  protos::RawQueryArgs query;
  bool parsed = query.ParseFromArray(query_data, len);
  if (!parsed) {
    std::string err = "Failed to parse input request";
    g_reply(id, false, err.data(), err.size());
    return;
  }

  using ColumnDesc = protos::RawQueryResult::ColumnDesc;
  protos::RawQueryResult result;
  auto it = g_trace_processor->ExecuteQuery(query.sql_query().c_str());
  for (uint32_t col = 0; col < it.ColumnCount(); ++col) {
    // Setup the descriptors.
    auto* descriptor = result.add_column_descriptors();
    descriptor->set_name(it.GetColumName(col));
    descriptor->set_type(ColumnDesc::UNKNOWN);

    // Add an empty column.
    result.add_columns();
  }

  for (uint32_t rows = 0; it.Next(); ++rows) {
    for (uint32_t col = 0; col < it.ColumnCount(); ++col) {
      auto* column = result.mutable_columns(static_cast<int>(col));
      auto* desc = result.mutable_column_descriptors(static_cast<int>(col));

      using SqlValue = trace_processor::SqlValue;
      auto cell = it.Get(col);
      if (desc->type() == ColumnDesc::UNKNOWN) {
        switch (cell.type) {
          case SqlValue::Type::kLong:
            desc->set_type(ColumnDesc::LONG);
            break;
          case SqlValue::Type::kString:
            desc->set_type(ColumnDesc::STRING);
            break;
          case SqlValue::Type::kDouble:
            desc->set_type(ColumnDesc::DOUBLE);
            break;
          case SqlValue::Type::kNull:
            break;
        }
      }

      // If either the column type is null or we still don't know the type,
      // just add null values to all the columns.
      if (cell.type == SqlValue::Type::kNull ||
          desc->type() == ColumnDesc::UNKNOWN) {
        column->add_long_values(0);
        column->add_string_values("[NULL]");
        column->add_double_values(0);
        column->add_is_nulls(true);
        continue;
      }

      // Cast the sqlite value to the type of the column.
      switch (desc->type()) {
        case ColumnDesc::LONG:
          PERFETTO_CHECK(cell.type == SqlValue::Type::kLong ||
                         cell.type == SqlValue::Type::kDouble);
          if (cell.type == SqlValue::Type::kLong) {
            column->add_long_values(cell.long_value);
          } else /* if (cell.type == SqlValue::Type::kDouble) */ {
            column->add_long_values(static_cast<int64_t>(cell.double_value));
          }
          column->add_is_nulls(false);
          break;
        case ColumnDesc::STRING: {
          PERFETTO_CHECK(cell.type == SqlValue::Type::kString);
          column->add_string_values(cell.string_value);
          column->add_is_nulls(false);
          break;
        }
        case ColumnDesc::DOUBLE:
          PERFETTO_CHECK(cell.type == SqlValue::Type::kLong ||
                         cell.type == SqlValue::Type::kDouble);
          if (cell.type == SqlValue::Type::kLong) {
            column->add_double_values(static_cast<double>(cell.long_value));
          } else /* if (cell.type == SqlValue::Type::kDouble) */ {
            column->add_double_values(cell.double_value);
          }
          column->add_is_nulls(false);
          break;
        case ColumnDesc::UNKNOWN:
          PERFETTO_FATAL("Handled in if statement above.");
      }
    }
    result.set_num_records(rows + 1);
  }
  if (auto opt_error = it.GetLastError()) {
    result.set_error(*opt_error);
  }

  std::string encoded;
  result.SerializeToString(&encoded);
  g_reply(id, true, encoded.data(), static_cast<uint32_t>(encoded.size()));
}

}  // extern "C"

}  // namespace trace_processor
}  // namespace perfetto
