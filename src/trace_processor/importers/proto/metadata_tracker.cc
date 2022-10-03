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

#include "src/trace_processor/importers/proto/metadata_tracker.h"

#include "perfetto/ext/base/crash_keys.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

namespace {

base::CrashKey g_crash_key_uuid("trace_uuid");

}

MetadataTracker::MetadataTracker(TraceStorage* storage) : storage_(storage) {
  for (uint32_t i = 0; i < kNumKeys; ++i) {
    key_ids_[i] = storage->InternString(metadata::kNames[i]);
  }
  for (uint32_t i = 0; i < kNumKeyTypes; ++i) {
    key_type_ids_[i] = storage->InternString(metadata::kKeyTypeNames[i]);
  }
}

MetadataId MetadataTracker::SetMetadata(metadata::KeyId key, Variadic value) {
  PERFETTO_DCHECK(metadata::kKeyTypes[key] == metadata::KeyType::kSingle);
  PERFETTO_DCHECK(value.type == metadata::kValueTypes[key]);

  // When the trace_uuid is set, store a copy in a crash key, so in case of
  // a crash in the pipelines we can tell which trace caused the crash.
  if (key == metadata::trace_uuid && value.type == Variadic::kString) {
    auto uuid_string_view = storage_->GetString(value.string_value);
    g_crash_key_uuid.Set(uuid_string_view);
  }

  auto* metadata_table = storage_->mutable_metadata_table();
  uint32_t key_idx = static_cast<uint32_t>(key);
  base::Optional<uint32_t> opt_row =
      metadata_table->name().IndexOf(metadata::kNames[key_idx]);
  if (opt_row) {
    WriteValue(*opt_row, value);
    return metadata_table->id()[*opt_row];
  }

  tables::MetadataTable::Row row;
  row.name = key_ids_[key_idx];
  row.key_type = key_type_ids_[static_cast<size_t>(metadata::KeyType::kSingle)];

  auto id_and_row = metadata_table->Insert(row);
  WriteValue(id_and_row.row, value);
  return id_and_row.id;
}

SqlValue MetadataTracker::GetMetadata(metadata::KeyId key) {
  // KeyType::kMulti not yet supported by this method:
  PERFETTO_CHECK(metadata::kKeyTypes[key] == metadata::KeyType::kSingle);

  auto* metadata_table = storage_->mutable_metadata_table();
  uint32_t key_idx = static_cast<uint32_t>(key);
  uint32_t row =
      metadata_table->name().IndexOf(metadata::kNames[key_idx]).value();

  auto value_type = metadata::kValueTypes[key];
  switch (value_type) {
    case Variadic::kInt:
      return metadata_table->mutable_int_value()->Get(row);
    case Variadic::kString:
      return metadata_table->mutable_str_value()->Get(row);
    case Variadic::kNull:
      return SqlValue();
    case Variadic::kJson:
    case Variadic::kUint:
    case Variadic::kPointer:
    case Variadic::kReal:
    case Variadic::kBool:
      PERFETTO_FATAL("Invalid metadata value type %zu", value_type);
  }
  PERFETTO_FATAL("For GCC");
}

MetadataId MetadataTracker::AppendMetadata(metadata::KeyId key,
                                           Variadic value) {
  PERFETTO_DCHECK(key < metadata::kNumKeys);
  PERFETTO_DCHECK(metadata::kKeyTypes[key] == metadata::KeyType::kMulti);
  PERFETTO_DCHECK(value.type == metadata::kValueTypes[key]);

  uint32_t key_idx = static_cast<uint32_t>(key);
  tables::MetadataTable::Row row;
  row.name = key_ids_[key_idx];
  row.key_type = key_type_ids_[static_cast<size_t>(metadata::KeyType::kMulti)];

  auto* metadata_table = storage_->mutable_metadata_table();
  auto id_and_row = metadata_table->Insert(row);
  WriteValue(id_and_row.row, value);
  return id_and_row.id;
}

MetadataId MetadataTracker::SetDynamicMetadata(StringId key, Variadic value) {
  tables::MetadataTable::Row row;
  row.name = key;
  row.key_type = key_type_ids_[static_cast<size_t>(metadata::KeyType::kSingle)];

  auto* metadata_table = storage_->mutable_metadata_table();
  auto id_and_row = metadata_table->Insert(row);
  WriteValue(id_and_row.row, value);
  return id_and_row.id;
}

void MetadataTracker::WriteValue(uint32_t row, Variadic value) {
  auto* metadata_table = storage_->mutable_metadata_table();
  switch (value.type) {
    case Variadic::Type::kInt:
      metadata_table->mutable_int_value()->Set(row, value.int_value);
      break;
    case Variadic::Type::kString:
      metadata_table->mutable_str_value()->Set(row, value.string_value);
      break;
    case Variadic::Type::kJson:
      metadata_table->mutable_str_value()->Set(row, value.json_value);
      break;
    case Variadic::Type::kBool:
    case Variadic::Type::kPointer:
    case Variadic::Type::kUint:
    case Variadic::Type::kReal:
    case Variadic::Type::kNull:
      PERFETTO_FATAL("Unsupported value type");
  }
}

}  // namespace trace_processor
}  // namespace perfetto
