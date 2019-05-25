/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_METRICS_METRICS_H_
#define SRC_TRACE_PROCESSOR_METRICS_METRICS_H_

#include <sqlite3.h>
#include <unordered_map>
#include <vector>

#include "perfetto/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/message.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_processor.h"

#include "src/trace_processor/metrics/descriptors.h"

namespace perfetto {
namespace trace_processor {
namespace metrics {

// Helper class to build a nested (metric) proto checking the schema against
// a descriptor.
// Visible for testing.
class ProtoBuilder {
 public:
  ProtoBuilder(TraceProcessor* tp, const ProtoDescriptor*);

  util::Status AppendSqlValue(const std::string& field_name,
                              const SqlValue& value);

  util::Status AppendLong(const std::string& field_name, int64_t value);
  util::Status AppendDouble(const std::string& field_name, double value);
  util::Status AppendString(const std::string& field_name,
                            base::StringView value) {
    const auto* data = reinterpret_cast<const uint8_t*>(value.data());
    return AppendBytesInternal(field_name, data, value.size(), true);
  }

  // Appends the contents of the bytes to the proto being built.
  // Note: when the asssociated field is a message the bytes provided should be
  // a |protos::ProtoBuilderResult|. In this case, type-checking will be
  // performed to ensure the message type match the type of the field.
  // The |protos::ProtoBuilderResult| can be a repeated field; in this case each
  // value in the result will be appended to the message in the usual proto
  // fashion.
  util::Status AppendBytes(const std::string& field_name,
                           protozero::ConstBytes bytes) {
    return AppendBytes(field_name, bytes.data, bytes.size);
  }

  util::Status AppendBytes(const std::string& field_name,
                           const uint8_t* data,
                           size_t size) {
    return AppendBytesInternal(field_name, data, size, false);
  }

  // Returns the serialized |protos::ProtoBuilderResult| with the built proto
  // as the nested |protobuf| message.
  // Note: no other functions should be called on this class after this method
  // is called.
  std::vector<uint8_t> SerializeToProtoBuilderResult();

  // Returns the serialized version of the raw message being built.
  // This function should only be used at the top level where type checking is
  // no longer important because the proto will be returned as is. In all other
  // instances, prefer |SerializeToProtoBuilderResult()| instead.
  // Note: no other functions should be called on this class after this method
  // is called.
  std::vector<uint8_t> SerializeRaw();

 private:
  util::Status AppendBytesInternal(const std::string& field_name,
                                   const uint8_t* ptr,
                                   size_t size,
                                   bool is_string);

  util::Status AppendNestedMessage(const FieldDescriptor& field,
                                   const uint8_t* ptr,
                                   size_t size);

  util::Status AppendRepeated(const std::string& field_name,
                              base::StringView table_name);

  TraceProcessor* tp_ = nullptr;
  const ProtoDescriptor* descriptor_ = nullptr;
  protozero::HeapBuffered<protozero::Message> message_;

  // Used to prevent reentrancy of repeated fields.
  // TODO(lalitm): remove this once the proper repeated field support is ready.
  bool is_inside_repeated_query_ = false;
};

// Replaces templated variables inside |raw_text| using the substitution given
// by |substitutions| writing the result to |out|.
// The syntax followed is a cut-down variant of Jinja. This means variables that
// are to be replaced use {{variable-name}} in the raw text with subsitutions
// containing a mapping from (variable-name -> replacement).
int TemplateReplace(
    const std::string& raw_text,
    const std::unordered_map<std::string, std::string>& substitutions,
    std::string* out);

// Context struct for the below function.
struct BuildProtoContext {
  TraceProcessor* tp;
  const DescriptorPool* pool;
  const ProtoDescriptor* desc;
};

// This funciton implements all the proto creation functions.
void BuildProto(sqlite3_context* ctx, int argc, sqlite3_value** argv);

// Context struct for the below function.
struct RunMetricContext {
  TraceProcessor* tp;
  std::vector<SqlMetric> metrics;
};

// This function implements the RUN_METRIC SQL function.
void RunMetric(sqlite3_context* ctx, int argc, sqlite3_value** argv);

util::Status ComputeMetrics(TraceProcessor* impl,
                            const std::vector<SqlMetric>& metrics,
                            const ProtoDescriptor& root_descriptor,
                            std::vector<uint8_t>* metrics_proto);

}  // namespace metrics
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_METRICS_METRICS_H_
