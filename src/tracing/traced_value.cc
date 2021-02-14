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

#include "perfetto/tracing/traced_value.h"

#include "perfetto/base/logging.h"
#include "perfetto/tracing/debug_annotation.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"

namespace perfetto {

namespace internal {

TracedValue CreateTracedValueFromProto(
    protos::pbzero::DebugAnnotation* context) {
  return TracedValue::CreateFromProto(context);
}

}  // namespace internal

// static
TracedValue TracedValue::CreateFromProto(
    protos::pbzero::DebugAnnotation* context) {
  return TracedValue(context, nullptr);
}

void TracedValue::WriteInt64(int64_t value) && {
  PERFETTO_DCHECK(checked_scope_.is_active());
  if (nested_context_) {
    nested_context_->set_int_value(value);
  } else {
    root_context_->set_int_value(value);
  }
}

void TracedValue::WriteUInt64(uint64_t value) && {
  PERFETTO_DCHECK(checked_scope_.is_active());
  if (nested_context_) {
    nested_context_->set_int_value(static_cast<int64_t>(value));
  } else {
    root_context_->set_uint_value(value);
  }
}

void TracedValue::WriteDouble(double value) && {
  PERFETTO_DCHECK(checked_scope_.is_active());
  if (nested_context_) {
    nested_context_->set_double_value(value);
  } else {
    root_context_->set_double_value(value);
  }
}

void TracedValue::WriteBoolean(bool value) && {
  PERFETTO_DCHECK(checked_scope_.is_active());
  if (nested_context_) {
    nested_context_->set_bool_value(value);
  } else {
    root_context_->set_bool_value(value);
  }
}

void TracedValue::WriteString(const char* value) && {
  PERFETTO_DCHECK(checked_scope_.is_active());
  if (nested_context_) {
    nested_context_->set_string_value(value);
  } else {
    root_context_->set_string_value(value);
  }
}

void TracedValue::WriteString(const char* value, size_t len) && {
  PERFETTO_DCHECK(checked_scope_.is_active());
  if (nested_context_) {
    nested_context_->set_string_value(value, len);
  } else {
    root_context_->set_string_value(value, len);
  }
}

void TracedValue::WriteString(const std::string& value) && {
  PERFETTO_DCHECK(checked_scope_.is_active());
  if (nested_context_) {
    nested_context_->set_string_value(value);
  } else {
    root_context_->set_string_value(value);
  }
}

void TracedValue::WritePointer(const void* value) && {
  PERFETTO_DCHECK(checked_scope_.is_active());
  if (nested_context_) {
    nested_context_->set_int_value(reinterpret_cast<int64_t>(value));
  } else {
    root_context_->set_pointer_value(reinterpret_cast<uint64_t>(value));
  }
}

TracedDictionary TracedValue::WriteDictionary() && {
  // Note: this passes |checked_scope_.is_active_| bit to the parent to be
  // picked up later by the new TracedDictionary.
  PERFETTO_DCHECK(checked_scope_.is_active());
  checked_scope_.Reset();

  if (nested_context_) {
    PERFETTO_DCHECK(!nested_context_->is_finalized());
    nested_context_->set_nested_type(
        protos::pbzero::DebugAnnotation_NestedValue_NestedType_DICT);
    return TracedDictionary(nested_context_, checked_scope_.parent_scope());
  } else {
    PERFETTO_DCHECK(!root_context_->is_finalized());
    protos::pbzero::DebugAnnotation::NestedValue* value =
        root_context_->set_nested_value();
    value->set_nested_type(
        protos::pbzero::DebugAnnotation_NestedValue_NestedType_DICT);
    return TracedDictionary(value, checked_scope_.parent_scope());
  }
}

TracedArray TracedValue::WriteArray() && {
  // Note: this passes |checked_scope_.is_active_| bit to the parent to be
  // picked up later by the new TracedDictionary.
  PERFETTO_DCHECK(checked_scope_.is_active());
  checked_scope_.Reset();

  if (nested_context_) {
    PERFETTO_DCHECK(!nested_context_->is_finalized());
    nested_context_->set_nested_type(
        protos::pbzero::DebugAnnotation_NestedValue_NestedType_ARRAY);
    return TracedArray(nested_context_, checked_scope_.parent_scope());
  } else {
    PERFETTO_DCHECK(!root_context_->is_finalized());
    protos::pbzero::DebugAnnotation::NestedValue* value =
        root_context_->set_nested_value();
    value->set_nested_type(
        protos::pbzero::DebugAnnotation_NestedValue_NestedType_ARRAY);
    return TracedArray(value, checked_scope_.parent_scope());
  }
}

TracedValue TracedArray::AppendItem() {
  PERFETTO_DCHECK(checked_scope_.is_active());
  return TracedValue(value_->add_array_values(), &checked_scope_);
}

TracedDictionary TracedArray::AppendDictionary() {
  PERFETTO_DCHECK(checked_scope_.is_active());
  return AppendItem().WriteDictionary();
}

TracedArray TracedArray::AppendArray() {
  PERFETTO_DCHECK(checked_scope_.is_active());
  return AppendItem().WriteArray();
}

TracedValue TracedDictionary::AddItem(const char* key) {
  PERFETTO_DCHECK(checked_scope_.is_active());
  value_->add_dict_keys(key);
  return TracedValue(value_->add_dict_values(), &checked_scope_);
}

TracedDictionary TracedDictionary::AddDictionary(const char* key) {
  PERFETTO_DCHECK(checked_scope_.is_active());
  return AddItem(key).WriteDictionary();
}

TracedArray TracedDictionary::AddArray(const char* key) {
  PERFETTO_DCHECK(checked_scope_.is_active());
  return AddItem(key).WriteArray();
}

}  // namespace perfetto
