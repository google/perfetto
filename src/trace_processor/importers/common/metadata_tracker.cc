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

#include "src/trace_processor/importers/common/metadata_tracker.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/crash_keys.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

namespace {
base::CrashKey g_crash_key_uuid("trace_uuid");

using MachineId = tables::MachineTable::Id;

// Returns the (machine_id, trace_id) pair that should be associated with a
// metadata key based on its defined scope.
//
// For machine-scoped metadata, we default to MachineId(0) (the host machine)
// if the context doesn't specify one. This ensures host metadata is correctly
// linked to its machine table entry instead of having a NULL machine_id.
std::pair<std::optional<MachineId>, std::optional<uint32_t>> GetContextIds(
    TraceProcessorContext* context,
    metadata::KeyId key) {
  switch (metadata::kScopes[key]) {
    case metadata::Scope::kGlobal:
      return {std::nullopt, std::nullopt};
    case metadata::Scope::kMachine:
      return {context->machine_id().value_or(MachineId(0)), std::nullopt};
    case metadata::Scope::kTrace:
      return {std::nullopt, context->trace_id()};
    case metadata::Scope::kMachineAndTrace:
      return {context->machine_id().value_or(MachineId(0)),
              context->trace_id()};
    case metadata::Scope::kNumScopes:
      PERFETTO_FATAL("Invalid scope");
  }
  PERFETTO_FATAL("For GCC");
}

// Returns true if |possible_parent| is an ancestor of |child| in the trace file
// hierarchy. |nullopt| is considered the ultimate ancestor of everything.
bool IsAncestor(TraceStorage* storage,
                std::optional<uint32_t> possible_parent,
                uint32_t child) {
  if (!possible_parent.has_value()) {
    return true;
  }
  const auto& table = storage->trace_file_table();
  std::optional<uint32_t> current = child;
  while (current) {
    auto row = table.FindById(tables::TraceFileTable::Id(*current));
    if (!row) {
      break;
    }
    auto parent = row->parent_id();
    if (!parent) {
      break;
    }
    if (parent->value == *possible_parent) {
      return true;
    }
    current = parent->value;
  }
  return false;
}

}  // namespace

MetadataTracker::MetadataTracker(TraceProcessorContext* context)
    : context_(context) {
  for (uint32_t i = 0; i < kNumKeys; ++i) {
    key_ids_[i] = context->storage->InternString(metadata::kNames[i]);
  }
  for (uint32_t i = 0; i < kNumKeyTypes; ++i) {
    key_type_ids_[i] =
        context->storage->InternString(metadata::kKeyTypeNames[i]);
  }
}

MetadataId MetadataTracker::SetMetadata(metadata::KeyId key,
                                        Variadic value,
                                        std::optional<MachineId> machine_id,
                                        std::optional<uint32_t> trace_id) {
  PERFETTO_DCHECK(metadata::kKeyTypes[key] == metadata::KeyType::kSingle);
  PERFETTO_DCHECK(value.type == metadata::kValueTypes[key]);

  // When the trace_uuid is set, store a copy in a crash key, so in case of
  // a crash in the pipelines we can tell which trace caused the crash.
  if (key == metadata::trace_uuid && value.type == Variadic::kString) {
    auto uuid_string_view = context_->storage->GetString(value.string_value);
    g_crash_key_uuid.Set(uuid_string_view);
  }

  if (!machine_id.has_value() && !trace_id.has_value()) {
    std::tie(machine_id, trace_id) = GetContextIds(context_, key);
  }
  auto& metadata_table = *context_->storage->mutable_metadata_table();
  auto key_idx = static_cast<uint32_t>(key);
  auto name_id =
      context_->storage->string_pool().GetId(metadata::kNames[key_idx]);
  if (name_id) {
    for (auto it = metadata_table.IterateRows(); it; ++it) {
      if (it.name() == *name_id) {
        // Normal case: update if machine and trace IDs match.
        if (it.machine_id() == machine_id && it.trace_id() == trace_id) {
          WriteValue(it.row_number().row_number(), value);
          return it.id();
        }

        // Special case for trace_uuid:
        // We want to "promote" the UUID from a container (e.g. ZIP) to its
        // first leaf trace. This ensures a single identity is maintained for
        // the session's primary entry (the first trace processed), while
        // allowing sibling traces (which are NOT descendants of each other) to
        // have their own separate entries if they provide their own UUIDs.
        if (key == metadata::trace_uuid && trace_id.has_value()) {
          if (IsAncestor(context_->storage.get(), it.trace_id(), *trace_id)) {
            // Hijack the row from the ancestor container.
            it.set_trace_id(trace_id);
            WriteValue(it.row_number().row_number(), value);
            return it.id();
          }
        }
      }
    }
  }

  tables::MetadataTable::Row row;
  row.name = key_ids_[key_idx];
  row.key_type = key_type_ids_[static_cast<size_t>(metadata::KeyType::kSingle)];
  row.machine_id = machine_id;
  row.trace_id = trace_id;

  auto id_and_row = metadata_table.Insert(row);
  WriteValue(id_and_row.row, value);
  return id_and_row.id;
}

std::optional<SqlValue> MetadataTracker::GetMetadata(metadata::KeyId key) {
  // KeyType::kMulti not yet supported by this method:
  PERFETTO_CHECK(metadata::kKeyTypes[key] == metadata::KeyType::kSingle);

  auto [machine_id, trace_id] = GetContextIds(context_, key);
  auto& metadata_table = *context_->storage->mutable_metadata_table();
  auto key_idx = static_cast<uint32_t>(key);

  auto key_id =
      context_->storage->string_pool().GetId(metadata::kNames[key_idx]);
  if (!key_id) {
    return std::nullopt;
  }

  std::optional<tables::MetadataTable::RowReference> row;
  for (auto it = metadata_table.IterateRows(); it; ++it) {
    if (key_id == it.name()) {
      if (it.machine_id() == machine_id && it.trace_id() == trace_id) {
        row = it.ToRowReference();
        break;
      }
      // For trace_uuid, return the first entry if it's an ancestor of the
      // current context. This ensures GetMetadata(trace_uuid) works even
      // before promotion.
      if (key == metadata::trace_uuid && trace_id.has_value()) {
        if (IsAncestor(context_->storage.get(), it.trace_id(), *trace_id)) {
          row = it.ToRowReference();
          break;
        }
      }
    }
  }
  if (!row.has_value()) {
    return {};
  }

  auto value_type = metadata::kValueTypes[key];
  switch (value_type) {
    case Variadic::kInt: {
      return SqlValue::Long(*row->int_value());
    }
    case Variadic::kString:
      return SqlValue::String(
          context_->storage->GetString(*row->str_value()).c_str());
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
                                           Variadic value,
                                           std::optional<MachineId> machine_id,
                                           std::optional<uint32_t> trace_id) {
  PERFETTO_DCHECK(key < metadata::kNumKeys);
  PERFETTO_DCHECK(metadata::kKeyTypes[key] == metadata::KeyType::kMulti);
  PERFETTO_DCHECK(value.type == metadata::kValueTypes[key]);

  if (!machine_id.has_value() && !trace_id.has_value()) {
    std::tie(machine_id, trace_id) = GetContextIds(context_, key);
  }
  uint32_t key_idx = static_cast<uint32_t>(key);
  tables::MetadataTable::Row row;
  row.name = key_ids_[key_idx];
  row.key_type = key_type_ids_[static_cast<size_t>(metadata::KeyType::kMulti)];
  row.machine_id = machine_id;
  row.trace_id = trace_id;

  auto* metadata_table = context_->storage->mutable_metadata_table();
  auto id_and_row = metadata_table->Insert(row);
  WriteValue(id_and_row.row, value);
  return id_and_row.id;
}

MetadataId MetadataTracker::SetDynamicMetadata(StringId key, Variadic value) {
  tables::MetadataTable::Row row;
  row.name = key;
  row.key_type = key_type_ids_[static_cast<size_t>(metadata::KeyType::kSingle)];
  row.machine_id = context_->machine_id();
  row.trace_id = context_->trace_id();

  auto* metadata_table = context_->storage->mutable_metadata_table();
  auto id_and_row = metadata_table->Insert(row);
  WriteValue(id_and_row.row, value);
  return id_and_row.id;
}

void MetadataTracker::WriteValue(uint32_t row, Variadic value) {
  auto& metadata_table = *context_->storage->mutable_metadata_table();
  auto rr = metadata_table[row];
  switch (value.type) {
    case Variadic::Type::kInt:
      rr.set_int_value(value.int_value);
      break;
    case Variadic::Type::kString:
      rr.set_str_value(value.string_value);
      break;
    case Variadic::Type::kJson:
      rr.set_str_value(value.json_value);
      break;
    case Variadic::Type::kBool:
    case Variadic::Type::kPointer:
    case Variadic::Type::kUint:
    case Variadic::Type::kReal:
    case Variadic::Type::kNull:
      PERFETTO_FATAL("Unsupported value type");
  }
}

}  // namespace perfetto::trace_processor
