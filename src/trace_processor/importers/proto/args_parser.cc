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

#include "src/trace_processor/importers/proto/args_parser.h"

#include "perfetto/ext/base/base64.h"
#include "src/trace_processor/importers/json/json_utils.h"

namespace perfetto::trace_processor {

using BoundInserter = ArgsTracker::BoundInserter;

ArgsParser::ArgsParser(int64_t packet_timestamp,
                       BoundInserter& inserter,
                       TraceStorage& storage,
                       PacketSequenceStateGeneration* sequence_state,
                       bool support_json)
    : support_json_(support_json),
      packet_timestamp_(packet_timestamp),
      sequence_state_(sequence_state),
      inserter_(inserter),
      storage_(storage) {}

ArgsParser::~ArgsParser() = default;

void ArgsParser::AddInteger(const Key& key, int64_t value) {
  inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                   storage_.InternString(base::StringView(key.key)),
                   Variadic::Integer(value));
}

void ArgsParser::AddUnsignedInteger(const Key& key, uint64_t value) {
  inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                   storage_.InternString(base::StringView(key.key)),
                   Variadic::UnsignedInteger(value));
}

void ArgsParser::AddString(const Key& key, const protozero::ConstChars& value) {
  inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                   storage_.InternString(base::StringView(key.key)),
                   Variadic::String(storage_.InternString(value)));
}

void ArgsParser::AddString(const Key& key, const std::string& value) {
  inserter_.AddArg(
      storage_.InternString(base::StringView(key.flat_key)),
      storage_.InternString(base::StringView(key.key)),
      Variadic::String(storage_.InternString(base::StringView(value))));
}

void ArgsParser::AddDouble(const Key& key, double value) {
  inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                   storage_.InternString(base::StringView(key.key)),
                   Variadic::Real(value));
}

void ArgsParser::AddPointer(const Key& key, const void* value) {
  inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                   storage_.InternString(base::StringView(key.key)),
                   Variadic::Pointer(reinterpret_cast<uintptr_t>(value)));
}

void ArgsParser::AddBoolean(const Key& key, bool value) {
  inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                   storage_.InternString(base::StringView(key.key)),
                   Variadic::Boolean(value));
}

void ArgsParser::AddBytes(const Key& key, const protozero::ConstBytes& value) {
  std::string b64_data = base::Base64Encode(value.data, value.size);
  AddString(key, b64_data);
}

bool ArgsParser::AddJson(const Key& key, const protozero::ConstChars& value) {
  if (!support_json_)
    PERFETTO_FATAL("Unexpected JSON value when parsing data");

  auto json_value = json::ParseJsonString(value);
  if (!json_value)
    return false;
  return json::AddJsonValueToArgs(*json_value, base::StringView(key.flat_key),
                                  base::StringView(key.key), &storage_,
                                  &inserter_);
}

void ArgsParser::AddNull(const Key& key) {
  inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                   storage_.InternString(base::StringView(key.key)),
                   Variadic::Null());
}

size_t ArgsParser::GetArrayEntryIndex(const std::string& array_key) {
  return inserter_.GetNextArrayEntryIndex(
      storage_.InternString(base::StringView(array_key)));
}

size_t ArgsParser::IncrementArrayEntryIndex(const std::string& array_key) {
  return inserter_.IncrementArrayEntryIndex(
      storage_.InternString(base::StringView(array_key)));
}

int64_t ArgsParser::packet_timestamp() {
  return packet_timestamp_;
}

PacketSequenceStateGeneration* ArgsParser::seq_state() {
  return sequence_state_;
}

InternedMessageView* ArgsParser::GetInternedMessageView(uint32_t field_id,
                                                        uint64_t iid) {
  if (!sequence_state_)
    return nullptr;
  return sequence_state_->GetInternedMessageView(field_id, iid);
}

}  // namespace perfetto::trace_processor
