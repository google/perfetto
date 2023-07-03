/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/winscope/winscope_args_parser.h"

namespace perfetto {
namespace trace_processor {

WinscopeArgsParser::WinscopeArgsParser(ArgsTracker::BoundInserter& inserter,
                                       TraceStorage& storage)
    : inserter_{inserter}, storage_{storage} {}

void WinscopeArgsParser::AddInteger(const Key& key, int64_t value) {
  const auto flat_key_id =
      storage_.InternString(base::StringView(key.flat_key));
  const auto key_id = storage_.InternString(base::StringView(key.key));
  const auto variadic_val = Variadic::Integer(value);
  inserter_.AddArg(flat_key_id, key_id, variadic_val);
}

void WinscopeArgsParser::AddUnsignedInteger(const Key& key, uint64_t value) {
  const auto flat_key_id =
      storage_.InternString(base::StringView(key.flat_key));
  const auto key_id = storage_.InternString(base::StringView(key.key));
  const auto variadic_val = Variadic::UnsignedInteger(value);
  inserter_.AddArg(flat_key_id, key_id, variadic_val);
}

void WinscopeArgsParser::AddString(const Key& key,
                                   const protozero::ConstChars& value) {
  const auto flat_key_id =
      storage_.InternString(base::StringView(key.flat_key));
  const auto key_id = storage_.InternString(base::StringView(key.key));
  const auto variadic_val = Variadic::String(storage_.InternString(value));
  inserter_.AddArg(flat_key_id, key_id, variadic_val);
}

void WinscopeArgsParser::AddString(const Key& key, const std::string& value) {
  const auto flat_key_id =
      storage_.InternString(base::StringView(key.flat_key));
  const auto key_id = storage_.InternString(base::StringView(key.key));
  const auto variadic_val =
      Variadic::String(storage_.InternString(base::StringView(value)));
  inserter_.AddArg(flat_key_id, key_id, variadic_val);
}

void WinscopeArgsParser::AddDouble(const Key& key, double value) {
  const auto flat_key_id =
      storage_.InternString(base::StringView(key.flat_key));
  const auto key_id = storage_.InternString(base::StringView(key.key));
  const auto variadic_val = Variadic::Real(value);
  inserter_.AddArg(flat_key_id, key_id, variadic_val);
}

void WinscopeArgsParser::AddPointer(const Key& key, const void* value) {
  const auto flat_key_id =
      storage_.InternString(base::StringView(key.flat_key));
  const auto key_id = storage_.InternString(base::StringView(key.key));
  const auto variadic_val =
      Variadic::Pointer(reinterpret_cast<uintptr_t>(value));
  inserter_.AddArg(flat_key_id, key_id, variadic_val);
}

void WinscopeArgsParser::AddBoolean(const Key& key, bool value) {
  const auto flat_key_id =
      storage_.InternString(base::StringView(key.flat_key));
  const auto key_id = storage_.InternString(base::StringView(key.key));
  const auto variadic_val = Variadic::Boolean(value);
  inserter_.AddArg(flat_key_id, key_id, variadic_val);
}

bool WinscopeArgsParser::AddJson(const Key&, const protozero::ConstChars&) {
  PERFETTO_FATAL("Unexpected JSON value when parsing SurfaceFlinger data");
}

void WinscopeArgsParser::AddNull(const Key& key) {
  const auto flat_key_id =
      storage_.InternString(base::StringView(key.flat_key));
  const auto key_id = storage_.InternString(base::StringView(key.key));
  const auto variadic_val = Variadic::Null();
  inserter_.AddArg(flat_key_id, key_id, variadic_val);
}

size_t WinscopeArgsParser::GetArrayEntryIndex(const std::string& array_key) {
  return inserter_.GetNextArrayEntryIndex(
      storage_.InternString(base::StringView(array_key)));
}

size_t WinscopeArgsParser::IncrementArrayEntryIndex(
    const std::string& array_key) {
  return inserter_.IncrementArrayEntryIndex(
      storage_.InternString(base::StringView(array_key)));
}

PacketSequenceStateGeneration* WinscopeArgsParser::seq_state() {
  return nullptr;
}

InternedMessageView* WinscopeArgsParser::GetInternedMessageView(
    uint32_t field_id,
    uint64_t iid) {
  base::ignore_result(field_id);
  base::ignore_result(iid);
  return nullptr;
}

}  // namespace trace_processor
}  // namespace perfetto
