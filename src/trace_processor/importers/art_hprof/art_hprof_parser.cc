/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/art_hprof/art_hprof_parser.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/endian.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/art_hprof/art_heap_graph.h"
#include "src/trace_processor/importers/art_hprof/art_heap_graph_builder.h"
#include "src/trace_processor/importers/art_hprof/art_hprof_model.h"
#include "src/trace_processor/importers/art_hprof/art_hprof_types.h"
#include "src/trace_processor/importers/common/builtin_trace_importers.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/trace_type.h"

namespace perfetto::trace_processor::art_hprof {

ArtHprofParser::ArtHprofParser(TraceProcessorContext* context)
    : context_(context) {}

ArtHprofParser::~ArtHprofParser() = default;

base::Status ArtHprofParser::Parse(TraceBlobView blob) {
  bool is_init = false;
  if (!parser_) {
    byte_iterator_ = std::make_unique<TraceBlobViewIterator>();
    parser_ = std::make_unique<HeapGraphBuilder>(
        std::unique_ptr<ByteIterator>(byte_iterator_.release()), context_);
    is_init = true;
  }
  parser_->PushBlob(std::move(blob));

  if (is_init && !parser_->ParseHeader()) {
    context_->stats_tracker->IncrementStats(stats::hprof_header_errors);
  }

  parser_->Parse();

  return base::OkStatus();
}

base::Status ArtHprofParser::OnPushDataToSorter() {
  if (!parser_) {
    return base::OkStatus();
  }

  HeapGraph graph = parser_->BuildGraph();

  UniquePid upid = context_->process_tracker->GetOrCreateProcess(0);

  if (graph.GetClassCount() == 0 || graph.GetObjectCount() == 0) {
    return base::OkStatus();
  }

  int64_t ts = static_cast<int64_t>(graph.GetTimestamp());
  tables::HeapGraphTable::Row heap_graph_row;
  heap_graph_row.ts = ts;
  heap_graph_row.upid = upid;
  context_->storage->mutable_heap_graph_table()->Insert(heap_graph_row);

  PopulateClasses(graph);
  PopulateObjects(graph, ts, upid);
  PopulateReferences(graph);
  PopulateFieldValues(graph);

  graph.ClearAll();

  class_map_.Clear();
  class_object_map_.Clear();
  object_rows_ = {};
  class_name_map_.Clear();
  parser_->Clear();

  return base::OkStatus();
}

tables::HeapGraphClassTable::Id* ArtHprofParser::FindClassId(
    uint64_t class_id) const {
  return class_map_.Find(class_id);
}

tables::HeapGraphClassTable::Id* ArtHprofParser::FindClassObjectId(
    uint64_t obj_id) const {
  return class_object_map_.Find(obj_id);
}

StringId ArtHprofParser::InternClassName(const std::string& class_name) {
  return context_->storage->InternString(class_name);
}

// TraceBlobViewIterator implementation
ArtHprofParser::TraceBlobViewIterator::TraceBlobViewIterator()
    : current_offset_(0) {}

ArtHprofParser::TraceBlobViewIterator::~TraceBlobViewIterator() = default;

const uint8_t* ArtHprofParser::TraceBlobViewIterator::Peek(size_t length) {
  const uint8_t* ptr = reader_.ContiguousAt(current_offset_, length);
  if (PERFETTO_LIKELY(ptr != nullptr)) {
    return ptr;
  }
  // Slow path: the read straddles two chunks, so it has to be stitched
  // together into the scratch buffer.
  if (length > sizeof(scratch_))
    return nullptr;
  auto slice = reader_.SliceOff(current_offset_, length);
  if (!slice)
    return nullptr;
  std::memcpy(scratch_, slice->data(), length);
  return scratch_;
}

bool ArtHprofParser::TraceBlobViewIterator::ReadU1(uint8_t& value) {
  const uint8_t* d = Peek(1);
  if (!d)
    return false;
  value = *d;
  current_offset_ += 1;
  return true;
}

bool ArtHprofParser::TraceBlobViewIterator::ReadU2(uint16_t& value) {
  const uint8_t* d = Peek(2);
  if (!d)
    return false;
  uint16_t raw;
  std::memcpy(&raw, d, sizeof(raw));
  value = base::BE16ToHost(raw);
  current_offset_ += 2;
  return true;
}

bool ArtHprofParser::TraceBlobViewIterator::ReadU4(uint32_t& value) {
  const uint8_t* d = Peek(4);
  if (!d)
    return false;
  uint32_t raw;
  std::memcpy(&raw, d, sizeof(raw));
  value = base::BE32ToHost(raw);
  current_offset_ += 4;
  return true;
}

bool ArtHprofParser::TraceBlobViewIterator::ReadId(uint64_t& value,
                                                   uint32_t id_size) {
  if (id_size != 4 && id_size != 8)
    return false;

  const uint8_t* d = Peek(id_size);
  if (!d)
    return false;

  if (id_size == 4) {
    uint32_t raw;
    std::memcpy(&raw, d, sizeof(raw));
    value = base::BE32ToHost(raw);
  } else {
    uint64_t raw;
    std::memcpy(&raw, d, sizeof(raw));
    value = base::BE64ToHost(raw);
  }
  current_offset_ += id_size;
  return true;
}

bool ArtHprofParser::TraceBlobViewIterator::ReadString(std::string& str,
                                                       size_t length) {
  auto slice = reader_.SliceOff(current_offset_, length);
  if (!slice)
    return false;

  str.resize(length);
  std::memcpy(str.data(), slice->data(), length);
  current_offset_ += length;
  return true;
}

bool ArtHprofParser::TraceBlobViewIterator::ReadView(TraceBlobView& view,
                                                     size_t length) {
  auto slice = reader_.SliceOff(current_offset_, length);
  if (!slice)
    return false;

  view = std::move(*slice);
  current_offset_ += length;
  return true;
}

bool ArtHprofParser::TraceBlobViewIterator::ReadInto(uint8_t* dst,
                                                     size_t length) {
  if (const uint8_t* d = reader_.ContiguousAt(current_offset_, length)) {
    std::memcpy(dst, d, length);
    current_offset_ += length;
    return true;
  }
  auto slice = reader_.SliceOff(current_offset_, length);
  if (!slice)
    return false;

  std::memcpy(dst, slice->data(), length);
  current_offset_ += length;
  return true;
}

bool ArtHprofParser::TraceBlobViewIterator::SkipBytes(size_t count) {
  if (!reader_.ContiguousAt(current_offset_, count) &&
      !reader_.SliceOff(current_offset_, count)) {
    return false;
  }

  current_offset_ += count;
  return true;
}

size_t ArtHprofParser::TraceBlobViewIterator::GetPosition() const {
  return current_offset_;
}

bool ArtHprofParser::TraceBlobViewIterator::CanReadRecord() const {
  const size_t base_offset = current_offset_ + kRecordLengthOffset;
  uint8_t bytes[4];

  const uint8_t* len_ptr = reader_.ContiguousAt(base_offset, 4);
  std::optional<TraceBlobView> slice;
  if (!len_ptr) {
    slice = reader_.SliceOff(base_offset, 4);
    if (!slice) {
      return false;
    }
    len_ptr = slice->data();
  }

  memcpy(bytes, len_ptr, 4);

  uint32_t record_length = (static_cast<uint32_t>(bytes[0]) << 24) |
                           (static_cast<uint32_t>(bytes[1]) << 16) |
                           (static_cast<uint32_t>(bytes[2]) << 8) |
                           static_cast<uint32_t>(bytes[3]);

  // Check if we can read the full record (header + body) from the chunk.
  // If we can't we should fail so that we can receive another
  // chunk to continue.
  size_t record_size = kRecordHeaderSize + record_length;
  return reader_.ContiguousAt(current_offset_, record_size) != nullptr ||
         reader_.SliceOff(current_offset_, record_size).has_value();
}

void ArtHprofParser::TraceBlobViewIterator::PushBlob(TraceBlobView blob) {
  reader_.PushBack(std::move(blob));
}

void ArtHprofParser::TraceBlobViewIterator::Shrink() {
  reader_.PopFrontUntil(current_offset_);
}

void ArtHprofParser::PopulateClasses(const HeapGraph& graph) {
  auto& class_table = *context_->storage->mutable_heap_graph_class_table();

  for (auto it = graph.GetClasses().GetIterator(); it; ++it) {
    auto class_id = it.key();
    auto& class_def = it.value();

    StringId name_id = InternClassName(class_def.GetName());
    StringId kind_id = context_->storage->InternString(kUnknownClassKind);

    tables::HeapGraphClassTable::Row class_row;
    class_row.name = name_id;
    class_row.deobfuscated_name = std::nullopt;
    class_row.location = std::nullopt;
    class_row.superclass_id = std::nullopt;
    class_row.classloader_id = 0;
    class_row.kind = kind_id;

    tables::HeapGraphClassTable::Id table_id = class_table.Insert(class_row).id;
    class_map_[class_id] = table_id;
    class_name_map_[class_id] = class_def.GetName();
  }

  for (auto it = graph.GetClasses().GetIterator(); it; ++it) {
    auto class_id = it.key();
    auto& class_def = it.value();
    uint64_t super_id = class_def.GetSuperClassId();
    if (super_id != 0) {
      auto* current_id = FindClassId(class_id);
      auto* super_id_opt = FindClassId(super_id);

      if (current_id && super_id_opt) {
        auto ref = class_table[*current_id];
        ref.set_superclass_id(*super_id_opt);
      }
    }
  }

  // Classify reference types so MarkReachableObjects can skip their
  // "referent" field during BFS.
  {
    // Find base reference type class IDs
    base::FlatHashMap<uint64_t, const char*> base_ref_kinds;
    for (auto it = graph.GetClasses().GetIterator(); it; ++it) {
      const auto& name = it.value().GetName();
      if (name == "java.lang.ref.WeakReference") {
        base_ref_kinds[it.key()] = "KIND_WEAK_REFERENCE";
      } else if (name == "java.lang.ref.SoftReference") {
        base_ref_kinds[it.key()] = "KIND_SOFT_REFERENCE";
      } else if (name == "java.lang.ref.PhantomReference") {
        base_ref_kinds[it.key()] = "KIND_PHANTOM_REFERENCE";
      } else if (name == "java.lang.ref.FinalizerReference") {
        base_ref_kinds[it.key()] = "KIND_FINALIZER_REFERENCE";
      }
    }

    if (base_ref_kinds.size() > 0) {
      for (auto it = graph.GetClasses().GetIterator(); it; ++it) {
        uint64_t current = it.key();
        const char* kind_name = nullptr;
        for (int depth = 0; depth < 100 && current != 0; ++depth) {
          auto* kind = base_ref_kinds.Find(current);
          if (kind) {
            kind_name = *kind;
            break;
          }
          auto* cls = graph.GetClasses().Find(current);
          if (!cls)
            break;
          current = cls->GetSuperClassId();
        }
        if (kind_name) {
          auto* table_id = FindClassId(it.key());
          if (table_id) {
            StringId kind_id = context_->storage->InternString(kind_name);
            class_table[*table_id].set_kind(kind_id);
          }
        }
      }
    }
  }

  // Process class objects. Class objects share IDs with their ClassDefinition
  // entries, so iterate classes instead of scanning all objects.
  for (auto it = graph.GetClasses().GetIterator(); it; ++it) {
    auto class_id = it.key();
    const auto* obj = graph.GetObjects().Find(class_id);
    if (!obj || obj->GetObjectType() != ObjectType::kClass) {
      continue;
    }

    auto* class_name_it = class_name_map_.Find(obj->GetClassId());
    if (!class_name_it) {
      context_->stats_tracker->IncrementStats(stats::hprof_class_errors);
      continue;
    }

    StringId name_id =
        InternClassName("java.lang.Class<" + *class_name_it + ">");
    StringId kind_id = context_->storage->InternString(kUnknownClassKind);

    tables::HeapGraphClassTable::Row class_row;
    class_row.name = name_id;
    class_row.deobfuscated_name = std::nullopt;
    class_row.location = std::nullopt;
    class_row.superclass_id = std::nullopt;
    class_row.classloader_id = 0;
    class_row.kind = kind_id;

    tables::HeapGraphClassTable::Id table_id = class_table.Insert(class_row).id;
    class_object_map_[class_id] = table_id;
  }
}

void ArtHprofParser::PopulateObjects(const HeapGraph& graph,
                                     int64_t ts,
                                     UniquePid upid) {
  auto& object_table = *context_->storage->mutable_heap_graph_object_table();
  const auto& objects = graph.GetObjects();

  tables::HeapGraphClassTable::Id unknown_class_id;

  object_rows_.assign(objects.size(), kInvalidRow);

  for (ObjectIndex i = 0; i < objects.size(); ++i) {
    const auto& obj = objects.at(i);

    tables::HeapGraphClassTable::Id* type_id;

    if (obj.GetObjectType() == ObjectType::kClass) {
      type_id = FindClassObjectId(obj.GetId());
      if (!type_id) {
        context_->stats_tracker->IncrementStats(stats::hprof_class_errors);
        continue;
      }
    } else {
      type_id = FindClassId(obj.GetClassId());
      if (!type_id && obj.GetObjectType() != ObjectType::kPrimitiveArray) {
        context_->stats_tracker->IncrementStats(stats::hprof_class_errors);
        continue;
      }
    }

    tables::HeapGraphObjectTable::Row object_row;
    object_row.upid = upid;
    object_row.graph_sample_ts = ts;
    object_row.self_size =
        static_cast<int64_t>(obj.GetSelfSizeOverride().value_or(0));
    object_row.native_size = obj.GetNativeSize();
    object_row.reference_set_id = std::nullopt;
    object_row.reachable = obj.IsReachable();
    object_row.type_id = type_id ? *type_id : unknown_class_id;

    object_row.heap_type = obj.GetHeapType();

    if (obj.IsRoot() && obj.GetRootType().has_value()) {
      std::string root_type_str =
          HeapGraph::GetRootTypeName(obj.GetRootType().value());
      StringId root_type_id = context_->storage->InternString(
          base::StringView(root_type_str.data(), root_type_str.size()));
      object_row.root_type = root_type_id;
    }

    object_row.root_distance = obj.GetRootDistance();

    tables::HeapGraphObjectTable::Id table_id =
        object_table.Insert(object_row).id;
    object_rows_[i] = table_id.value;
  }
}

void ArtHprofParser::PopulateReferences(const HeapGraph& graph) {
  auto& object_table = *context_->storage->mutable_heap_graph_object_table();
  auto& reference_table =
      *context_->storage->mutable_heap_graph_reference_table();

  StringId unknown_type_id = context_->storage->InternString("unknown");
  const auto& objects = graph.GetObjects();

  // Name of the class of each object, used as the field type name of every
  // reference pointing at it.
  const auto& class_table = context_->storage->heap_graph_class_table();
  std::vector<StringId> class_names(objects.size(), unknown_type_id);
  for (ObjectIndex i = 0; i < objects.size(); ++i) {
    auto* cls_table_id = FindClassId(objects.at(i).GetClassId());
    if (cls_table_id) {
      class_names[i] = class_table[*cls_table_id].name();
    }
  }

  for (ObjectIndex i = 0; i < objects.size(); ++i) {
    const auto& obj = objects.at(i);

    const auto& refs = obj.GetReferences();
    if (refs.empty()) {
      continue;
    }

    if (object_rows_[i] == kInvalidRow) {
      continue;
    }
    tables::HeapGraphObjectTable::Id owner_table_id(object_rows_[i]);

    // reference_set_id = row index of first reference for this object,
    // matching the proto importer's convention.
    uint32_t reference_set_id =
        static_cast<uint32_t>(reference_table.row_count());
    object_table[owner_table_id].set_reference_set_id(reference_set_id);

    for (const auto& ref : refs) {
      std::optional<tables::HeapGraphObjectTable::Id> owned_table_id;
      StringId field_type_id = unknown_type_id;
      if (ref.target_index != kInvalidObjectIndex) {
        if (object_rows_[ref.target_index] != kInvalidRow) {
          owned_table_id =
              tables::HeapGraphObjectTable::Id(object_rows_[ref.target_index]);
        }
        field_type_id = class_names[ref.target_index];
      }

      tables::HeapGraphReferenceTable::Row reference_row;
      reference_row.reference_set_id = reference_set_id;
      reference_row.owner_id = owner_table_id;
      reference_row.owned_id = owned_table_id;
      reference_row.field_name = ref.field_name;
      reference_row.field_type_name = field_type_id;

      reference_table.Insert(reference_row);
    }
  }
}

namespace {
const char* FieldTypeName(FieldType type) {
  switch (type) {
    case FieldType::kBoolean:
      return "boolean";
    case FieldType::kByte:
      return "byte";
    case FieldType::kChar:
      return "char";
    case FieldType::kShort:
      return "short";
    case FieldType::kInt:
      return "int";
    case FieldType::kLong:
      return "long";
    case FieldType::kFloat:
      return "float";
    case FieldType::kDouble:
      return "double";
    case FieldType::kObject:
      return "object";
  }
  return "unknown";
}
}  // namespace

void ArtHprofParser::PopulateFieldValues(const HeapGraph& graph) {
  auto& data_table = *context_->storage->mutable_heap_graph_object_data_table();
  auto& prim_table = *context_->storage->mutable_heap_graph_primitive_table();
  auto& object_table = *context_->storage->mutable_heap_graph_object_table();

  const auto& objects = graph.GetObjects();
  for (ObjectIndex i = 0; i < objects.size(); ++i) {
    const auto& obj = objects.at(i);

    if (object_rows_[i] == kInvalidRow)
      continue;
    tables::HeapGraphObjectTable::Id owner_table_id(object_rows_[i]);

    bool has_array_data = obj.HasArrayData();
    bool has_string = obj.GetDecodedString().has_value();

    // Primitive fields are written as they are decoded; nothing is inserted
    // for objects which have none, so this is safe to do before deciding
    // whether the object needs a data row at all.
    uint32_t field_set_id = static_cast<uint32_t>(prim_table.row_count());
    uint32_t field_count =
        InsertPrimitiveFields(graph, obj, field_set_id, prim_table);

    if (field_count == 0 && !has_array_data && !has_string)
      continue;

    tables::HeapGraphObjectDataTable::Row data_row;

    if (has_string) {
      data_row.value_string = *obj.GetDecodedString();
    }

    if (field_count > 0) {
      data_row.field_set_id = field_set_id;
    }

    if (has_array_data) {
      InsertArrayData(obj, data_row);
    }

    auto data_id = data_table.Insert(data_row).id;

    // Set reverse FK on the object table for direct lookup.
    object_table[owner_table_id].set_object_data_id(data_id.value);
  }
}

uint32_t ArtHprofParser::InsertPrimitiveFields(
    const HeapGraph& graph,
    const Object& obj,
    uint32_t field_set_id,
    tables::HeapGraphPrimitiveTable& prim_table) {
  uint32_t count = 0;
  auto insert = [&](const Field& field) {
    if (field.GetType() == FieldType::kObject || !field.HasValue())
      return;

    tables::HeapGraphPrimitiveTable::Row row;
    row.field_set_id = field_set_id;
    row.field_name = field.GetName();
    row.field_type =
        context_->storage->InternString(FieldTypeName(field.GetType()));

    switch (field.GetType()) {
      case FieldType::kBoolean:
        row.bool_value = field.GetValue<bool>().value_or(false) ? 1u : 0u;
        break;
      case FieldType::kByte:
        row.byte_value =
            static_cast<int64_t>(field.GetValue<uint8_t>().value_or(0));
        break;
      case FieldType::kChar:
        row.char_value =
            static_cast<int64_t>(field.GetValue<uint16_t>().value_or(0));
        break;
      case FieldType::kShort:
        row.short_value =
            static_cast<int64_t>(field.GetValue<int16_t>().value_or(0));
        break;
      case FieldType::kInt:
        row.int_value =
            static_cast<int64_t>(field.GetValue<int32_t>().value_or(0));
        break;
      case FieldType::kLong:
        row.long_value = field.GetValue<int64_t>().value_or(0);
        break;
      case FieldType::kFloat:
        row.float_value =
            static_cast<double>(field.GetValue<float>().value_or(0.0f));
        break;
      case FieldType::kDouble:
        row.double_value = field.GetValue<double>().value_or(0.0);
        break;
      case FieldType::kObject:
        break;
    }

    prim_table.Insert(row);
    ++count;
  };

  if (obj.GetObjectType() == ObjectType::kClass) {
    // Class objects carry their static fields, parsed from the class dump.
    for (const auto& field : obj.GetFields()) {
      insert(field);
    }
  } else if (obj.GetObjectType() == ObjectType::kInstance) {
    const auto* layout = graph.GetClassFields(obj.GetClassId());
    if (layout) {
      ForEachFieldValue(context_, layout->fields, obj.GetRawData(),
                        obj.GetRawDataSize(), graph.GetIdSize(), insert);
    }
  }
  return count;
}

void ArtHprofParser::InsertArrayData(
    const Object& obj,
    tables::HeapGraphObjectDataTable::Row& data_row) {
  const TraceBlobView& data = obj.GetArrayData();
  if (data.size() == 0)
    return;

  int64_t hash = static_cast<int64_t>(
      base::murmur_internal::MurmurHashBytes(data.data(), data.size()));

  StringId type_id =
      context_->storage->InternString(FieldTypeName(obj.GetArrayElementType()));
  auto* blobs = context_->storage->mutable_hprof_array_blobs();
  uint32_t blob_id = static_cast<uint32_t>(blobs->size());

  // Shares the buffer decoded at parse time rather than copying it again.
  // That buffer is allocated per array and sized exactly to the array, so
  // holding onto it here does not retain any of the trace's own memory: the
  // parser deliberately does not hand out views into trace chunks for data
  // that outlives the import.
  TraceStorage::HprofArrayBlob array_blob;
  array_blob.data = data.slice_off(0, data.size());
  array_blob.element_type = static_cast<uint8_t>(obj.GetArrayElementType());
  array_blob.element_count = static_cast<uint32_t>(obj.GetArrayElementCount());
  blobs->push_back(std::move(array_blob));

  data_row.array_element_type = type_id;
  data_row.array_element_count =
      static_cast<uint32_t>(obj.GetArrayElementCount());
  data_row.array_data_id = blob_id;
  data_row.array_data_hash = hash;
}

}  // namespace perfetto::trace_processor::art_hprof

namespace perfetto::trace_processor {
namespace {

// Android ART HPROF heap dump.
class ArtHprofImporter : public TraceImporter<ArtHprofImporter> {
 public:
  ArtHprofImporter() : TraceImporter(MakeDescriptor()) {}
  ~ArtHprofImporter() override;

  bool Sniff(const uint8_t* data, size_t size) const override {
    static constexpr char kMagic[] = {'J', 'A', 'V', 'A', ' ', 'P',
                                      'R', 'O', 'F', 'I', 'L', 'E'};
    return size >= sizeof(kMagic) && memcmp(data, kMagic, sizeof(kMagic)) == 0;
  }

  base::StatusOr<std::unique_ptr<ChunkedTraceReader>> CreateReader(
      TraceProcessorContext* context,
      uint32_t) const override {
    return std::unique_ptr<ChunkedTraceReader>(
        std::make_unique<art_hprof::ArtHprofParser>(context));
  }

 private:
  static TraceTypeDescriptor MakeDescriptor() {
    TraceTypeDescriptor d;
    d.name = "art_hprof";
    d.detection_priority = 80;
    return d;
  }
};

ArtHprofImporter::~ArtHprofImporter() = default;

}  // namespace

std::unique_ptr<TraceImporterBase> CreateArtHprofImporter() {
  return std::make_unique<ArtHprofImporter>();
}

}  // namespace perfetto::trace_processor
