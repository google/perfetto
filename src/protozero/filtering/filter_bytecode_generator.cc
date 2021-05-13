/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/protozero/filtering/filter_bytecode_generator.h"

#include "perfetto/base/logging.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"

#include "protos/perfetto/config/proto_filter.pbzero.h"

namespace protozero {

using ProtoFilter = perfetto::protos::pbzero::ProtoFilter;

FilterBytecodeGenerator::FilterBytecodeGenerator() = default;
FilterBytecodeGenerator::~FilterBytecodeGenerator() = default;

void FilterBytecodeGenerator::EndMessage() {
  endmessage_called_ = true;
  bytecode_.push_back(ProtoFilter::FILTER_OPCODE_END_OF_MESSAGE);
  last_field_id_ = 0;
  ++num_messages_;
}

// Allows a simple field (varint, fixed32/64, string or bytes).
void FilterBytecodeGenerator::AddSimpleField(uint32_t field_id) {
  PERFETTO_CHECK(field_id > last_field_id_);
  bytecode_.push_back(field_id << 3 | ProtoFilter::FILTER_OPCODE_SIMPLE_FIELD);
  last_field_id_ = field_id;
  endmessage_called_ = false;
}

// Allows a range of simple fields. |range_start| is the id of the first field
// in range, |range_len| the number of fields in the range.
// AddSimpleFieldRange(N,1) is semantically equivalent to AddSimpleField(N).
void FilterBytecodeGenerator::AddSimpleFieldRange(uint32_t range_start,
                                                  uint32_t range_len) {
  PERFETTO_CHECK(range_start > last_field_id_);
  PERFETTO_CHECK(range_len > 0);
  bytecode_.push_back(range_start << 3 |
                      ProtoFilter::FILTER_OPCODE_SIMPLE_FIELD_RANGE);
  bytecode_.push_back(range_len);
  last_field_id_ = range_start + range_len - 1;
  endmessage_called_ = false;
}

// Adds a nested field. |message_index| is the index of the message that the
// parser must recurse into. This implies that at least |message_index| + 1
// calls to EndMessage() will be made.
// The Serialize() method will fail if any field points to an out of range
// index.
void FilterBytecodeGenerator::AddNestedField(uint32_t field_id,
                                             uint32_t message_index) {
  PERFETTO_CHECK(field_id > last_field_id_);
  bytecode_.push_back(field_id << 3 | ProtoFilter::FILTER_OPCODE_NESTED_FIELD);
  bytecode_.push_back(message_index);
  last_field_id_ = field_id;
  max_msg_index_ = std::max(max_msg_index_, message_index);
  endmessage_called_ = false;
}

// Returns the proto-encoded bytes for a perfetto.protos.ProtoFilter message
// (see proto_filter.proto). The returned string can be passed to
// FilterBytecodeParser.Load().
std::string FilterBytecodeGenerator::Serialize() {
  PERFETTO_CHECK(endmessage_called_);
  PERFETTO_CHECK(max_msg_index_ < num_messages_);
  protozero::PackedVarInt words;
  for (uint32_t word : bytecode_)
    words.Append(word);

  protozero::HeapBuffered<ProtoFilter> filter;
  filter->set_bytecode(words);
  return filter.SerializeAsString();
}

}  // namespace protozero
