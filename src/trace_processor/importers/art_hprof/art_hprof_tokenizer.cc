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

#include "src/trace_processor/importers/art_hprof/art_hprof_event.h"
#include "src/trace_processor/sorter/trace_sorter.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <cinttypes>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "perfetto/base/logging.h"

namespace perfetto::trace_processor::art_hprof {

ByteIterator::~ByteIterator() = default;

// Helper map for primitive array types
const static std::unordered_map<std::string, FieldType>&
GetPrimitiveArrayNameMap() {
  static const auto* kMap = new std::unordered_map<std::string, FieldType>{
      {"boolean[]", FieldType::BOOLEAN}, {"char[]", FieldType::CHAR},
      {"float[]", FieldType::FLOAT},     {"double[]", FieldType::DOUBLE},
      {"byte[]", FieldType::BYTE},       {"short[]", FieldType::SHORT},
      {"int[]", FieldType::INT},         {"long[]", FieldType::LONG},
  };
  return *kMap;
}

// ArtHprofTokenizer implementation
ArtHprofTokenizer::ArtHprofTokenizer(TraceProcessorContext* ctx)
    : context_(ctx) {}

ArtHprofTokenizer::~ArtHprofTokenizer() = default;

base::Status ArtHprofTokenizer::Parse(TraceBlobView blob) {
  PERFETTO_DLOG("TBV length: %zu. Size: %zu. Offset: %zu", blob.length(),
                blob.size(), blob.offset());
  reader_.PushBack(std::move(blob));
  byte_iterator_ = std::make_unique<TraceBlobViewIterator>(std::move(reader_));
  if (!parser_) {
    parser_ = std::make_unique<HprofParser>(
        std::unique_ptr<ByteIterator>(byte_iterator_.release()));
  }
  parser_->Parse();

  return base::OkStatus();
}

base::Status ArtHprofTokenizer::NotifyEndOfFile() {
  PERFETTO_DLOG("EOF");
  HeapGraph graph = parser_->BuildGraph();
  context_->art_hprof_parser->ParseArtHprofEvent(
      static_cast<int64_t>(graph.GetTimestamp()),
      ArtHprofEvent(std::move(graph)));
  return base::OkStatus();
}

// TraceBlobViewIterator implementation
ArtHprofTokenizer::TraceBlobViewIterator::TraceBlobViewIterator(
    util::TraceBlobViewReader&& reader)
    : reader_(std::move(reader)), current_offset_(0) {}

ArtHprofTokenizer::TraceBlobViewIterator::~TraceBlobViewIterator() = default;

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadU1(uint8_t& value) {
  auto slice = reader_.SliceOff(current_offset_, 1);
  if (!slice)
    return false;
  value = *slice->data();
  current_offset_ += 1;
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadU2(uint16_t& value) {
  uint8_t b1, b2;
  if (!ReadU1(b1) || !ReadU1(b2))
    return false;
  value = static_cast<uint16_t>((static_cast<uint16_t>(b1) << 8) |
                                static_cast<uint16_t>(b2));
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadU4(uint32_t& value) {
  uint8_t b1, b2, b3, b4;
  if (!ReadU1(b1) || !ReadU1(b2) || !ReadU1(b3) || !ReadU1(b4))
    return false;
  value = (static_cast<uint32_t>(b1) << 24) |
          (static_cast<uint32_t>(b2) << 16) | (static_cast<uint32_t>(b3) << 8) |
          static_cast<uint32_t>(b4);
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadId(uint64_t& value,
                                                      uint32_t id_size) {
  if (id_size == 4) {
    uint32_t id;
    if (!ReadU4(id))
      return false;
    value = id;
    return true;
  } else if (id_size == 8) {
    uint32_t high, low;
    if (!ReadU4(high) || !ReadU4(low))
      return false;
    value = (static_cast<uint64_t>(high) << 32) | low;
    return true;
  }
  return false;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadString(std::string& str,
                                                          size_t length) {
  auto slice = reader_.SliceOff(current_offset_, length);
  if (!slice)
    return false;

  str.resize(length);
  std::memcpy(&str[0], slice->data(), length);
  current_offset_ += length;
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadBytes(
    std::vector<uint8_t>& data,
    size_t length) {
  auto slice = reader_.SliceOff(current_offset_, length);
  if (!slice)
    return false;

  data.resize(length);
  std::memcpy(data.data(), slice->data(), length);
  current_offset_ += length;
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::SkipBytes(size_t count) {
  auto slice = reader_.SliceOff(current_offset_, count);
  if (!slice)
    return false;

  current_offset_ += count;
  return true;
}

size_t ArtHprofTokenizer::TraceBlobViewIterator::GetPosition() const {
  return current_offset_;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::IsEof() const {
  return !reader_.SliceOff(current_offset_, 1);
}

bool ArtHprofTokenizer::TraceBlobViewIterator::IsValid() const {
  return true;
}

void HeapGraph::AddObject(HprofObject object) {
  objects_[object.id()] = std::move(object);
}

void HeapGraph::AddClass(ClassDefinition cls) {
  classes_[cls.id()] = std::move(cls);
}

void HeapGraph::AddString(uint64_t id, std::string string) {
  strings_[id] = std::move(string);
}

std::string HeapGraph::GetString(uint64_t id) const {
  auto it = strings_.find(id);
  if (it != strings_.end()) {
    return it->second;
  }
  return kUnknownString;
}

std::string_view HeapGraph::GetRootTypeName(HprofHeapRootTag root_type_id) {
  switch (root_type_id) {
    case HprofHeapRootTag::JNI_GLOBAL:
      return "JNI_GLOBAL";
    case HprofHeapRootTag::JNI_LOCAL:
      return "JNI_LOCAL";
    case HprofHeapRootTag::JAVA_FRAME:
      return "JAVA_FRAME";
    case HprofHeapRootTag::NATIVE_STACK:
      return "NATIVE_STACK";
    case HprofHeapRootTag::STICKY_CLASS:
      return "STICKY_CLASS";
    case HprofHeapRootTag::THREAD_BLOCK:
      return "THREAD_BLOCK";
    case HprofHeapRootTag::MONITOR_USED:
      return "MONITOR_USED";
    case HprofHeapRootTag::THREAD_OBJ:
      return "THREAD_OBJECT";
    case HprofHeapRootTag::INTERNED_STRING:
      return "INTERNED_STRING";
    case HprofHeapRootTag::FINALIZING:
      return "FINALIZING";
    case HprofHeapRootTag::DEBUGGER:
      return "DEBUGGER";
    case HprofHeapRootTag::VM_INTERNAL:
      return "VM_INTERNAL";
    case HprofHeapRootTag::JNI_MONITOR:
      return "JNI_MONITOR";
    case HprofHeapRootTag::UNKNOWN:
      return "UNKNOWN";
  }
}

HprofParser::HprofParser(std::unique_ptr<ByteIterator> iterator)
    : iterator_(std::move(iterator)) {}

HprofParser::~HprofParser() = default;

bool HprofParser::Parse() {
  if (!ParseHeader()) {
    PERFETTO_FATAL("Failed to parse header");
    return false;
  }

  PERFETTO_LOG("Format: %s", header_.format().c_str());
  PERFETTO_LOG("ID Size: %u", header_.id_size());
  PERFETTO_LOG("Timestamp: %" PRIu64, header_.timestamp());

  // Parse records until end of file
  size_t record_count = 0;
  while (iterator_->IsValid() && !iterator_->IsEof()) {
    record_count++;
    if (!ParseRecord()) {
      PERFETTO_LOG("Returning early from parsing");
      break;
    }
  }

  // Print stats
  PERFETTO_LOG("\nParsing complete.");
  PERFETTO_LOG("Strings: %zu", string_count_);
  PERFETTO_LOG("Classes: %zu", class_count_);
  PERFETTO_LOG("Heap dumps: %zu", heap_dump_count_);
  PERFETTO_LOG("Instances: %zu", instance_count_);
  PERFETTO_LOG("Object arrays: %zu", object_array_count_);
  PERFETTO_LOG("Primitive arrays: %zu", primitive_array_count_);
  PERFETTO_LOG("Roots: %zu", root_count_);
  PERFETTO_LOG("References: %zu", reference_count_);
  PERFETTO_LOG("Record count: %zu", record_count);

  return true;
}

HeapGraph HprofParser::BuildGraph() {
  FixupObjectReferencesAndRoots();

  // Build and return the heap graph
  HeapGraph graph(header_.timestamp());

  // Add strings
  for (const auto& entry : strings_) {
    graph.AddString(entry.first, entry.second);
  }

  // Add classes
  for (auto& entry : classes_) {
    graph.AddClass(entry.second);
  }

  // Add objects
  for (auto& entry : objects_) {
    graph.AddObject(entry.second);
  }

  return graph;
}

bool HprofParser::ParseHeader() {
  // Read format string (null-terminated)
  std::string format;
  uint8_t byte;
  while (iterator_->ReadU1(byte) && byte != 0) {
    format.push_back(static_cast<char>(byte));
  }

  header_.SetFormat(format);

  // Read identifier size
  uint32_t id_size;
  if (!iterator_->ReadU4(id_size)) {
    return false;
  }
  header_.SetIdSize(id_size);

  // Read timestamp (high and low 32-bit values)
  uint32_t high_time, low_time;
  if (!iterator_->ReadU4(high_time) || !iterator_->ReadU4(low_time)) {
    return false;
  }

  uint64_t timestamp = (static_cast<uint64_t>(high_time) << 32) | low_time;

  header_.SetTimestamp(timestamp);

  return true;
}

bool HprofParser::ParseRecord() {
  // Read record header
  uint8_t tag_value;
  if (!iterator_->ReadU1(tag_value)) {
    return false;
  }
  HprofTag tag = static_cast<HprofTag>(tag_value);

  uint32_t time;
  if (!iterator_->ReadU4(time)) {
    return false;
  }

  uint32_t length;
  if (!iterator_->ReadU4(length)) {
    return false;
  }

  // Handle record based on tag
  switch (tag) {
    case HprofTag::UTF8:
      return HandleUtf8Record(length);

    case HprofTag::LOAD_CLASS:
      return HandleLoadClassRecord();

    case HprofTag::HEAP_DUMP:
    case HprofTag::HEAP_DUMP_SEGMENT:
      heap_dump_count_++;
      return ParseHeapDump(length);

    case HprofTag::HEAP_DUMP_END:
      // Nothing to do for this tag
      return true;

    case HprofTag::FRAME:
    case HprofTag::TRACE:
      // Just skip these records
      return iterator_->SkipBytes(length);
  }

  // Skip unknown tags
  return iterator_->SkipBytes(length);
}

bool HprofParser::HandleUtf8Record(uint32_t length) {
  // String ID
  uint64_t id;
  if (!iterator_->ReadId(id, header_.id_size())) {
    return false;
  }

  // String data (length minus id_size)
  std::string str;
  if (!iterator_->ReadString(str, length - header_.id_size())) {
    return false;
  }

  strings_[id] = str;
  string_count_++;

  return true;
}

bool HprofParser::HandleLoadClassRecord() {
  // Serial number (not used)
  uint32_t serial_num;
  if (!iterator_->ReadU4(serial_num))
    return false;

  // Class object ID
  uint64_t class_obj_id;
  if (!iterator_->ReadId(class_obj_id, header_.id_size()))
    return false;

  // Stack trace serial number (not used)
  uint32_t stack_trace;
  if (!iterator_->ReadU4(stack_trace))
    return false;

  // Class name string ID
  uint64_t name_id;
  if (!iterator_->ReadId(name_id, header_.id_size()))
    return false;

  // Get class name from strings map
  std::string class_name = NormalizeClassName(GetString(name_id));

  // Store class definition
  ClassDefinition class_def(class_obj_id, class_name);
  classes_[class_obj_id] = class_def;
  class_count_++;

  const auto& primitive_map = GetPrimitiveArrayNameMap();
  auto it = primitive_map.find(class_name);
  if (it != primitive_map.end()) {
    prim_array_class_ids_[static_cast<size_t>(it->second)] = class_obj_id;
    PERFETTO_DLOG("Registered class ID %" PRIu64 " for primitive array type %s",
                  class_obj_id, class_name.c_str());
  }

  return true;
}

bool HprofParser::ParseHeapDump(size_t length) {
  size_t end_position = iterator_->GetPosition() + length;

  // Parse heap dump records until we reach the end of the segment
  while (iterator_->GetPosition() < end_position && !iterator_->IsEof()) {
    if (!ParseHeapDumpRecord()) {
      return false;
    }
  }

  // Ensure we're at the exact end position
  if (iterator_->GetPosition() != end_position) {
    size_t current = iterator_->GetPosition();
    if (current < end_position) {
      // Skip any remaining bytes
      iterator_->SkipBytes(end_position - current);
    } else {
      // We went too far, which is an error
      return false;
    }
  }

  return true;
}

bool HprofParser::ParseHeapDumpRecord() {
  // Read sub-record type
  uint8_t tag_value;
  if (!iterator_->ReadU1(tag_value)) {
    return false;
  }

  // First check if it's a root record by looking at the tag value
  // Root record values are from 0x01 to 0xFF (except for heap record values)
  bool is_heap_record =
      (tag_value == static_cast<uint8_t>(HprofHeapTag::CLASS_DUMP) ||
       tag_value == static_cast<uint8_t>(HprofHeapTag::INSTANCE_DUMP) ||
       tag_value == static_cast<uint8_t>(HprofHeapTag::OBJ_ARRAY_DUMP) ||
       tag_value == static_cast<uint8_t>(HprofHeapTag::PRIM_ARRAY_DUMP) ||
       tag_value == static_cast<uint8_t>(HprofHeapTag::HEAP_DUMP_INFO));

  if (!is_heap_record) {
    // Assuming it's a root record
    HprofHeapRootTag root_tag = static_cast<HprofHeapRootTag>(tag_value);
    return HandleRootRecord(root_tag);
  }

  // Handle heap records
  switch (static_cast<HprofHeapTag>(tag_value)) {
    case HprofHeapTag::CLASS_DUMP:
      return HandleClassDumpRecord();
    case HprofHeapTag::INSTANCE_DUMP:
      return HandleInstanceDumpRecord();
    case HprofHeapTag::OBJ_ARRAY_DUMP:
      return HandleObjectArrayDumpRecord();
    case HprofHeapTag::PRIM_ARRAY_DUMP:
      return HandlePrimitiveArrayDumpRecord();
    case HprofHeapTag::HEAP_DUMP_INFO:
      return HandleHeapDumpInfoRecord();
  }

  // This should be unreachable given the logic above, but keeping it for safety
  PERFETTO_LOG("Unknown HEAP_DUMP sub-record tag: 0x%02x", tag_value);
  return false;
}

bool HprofParser::HandleHeapDumpInfoRecord() {
  // Heap ID
  uint32_t heap_id;
  if (!iterator_->ReadU4(heap_id)) {
    return false;
  }

  // Heap name string ID
  uint64_t name_string_id;
  if (!iterator_->ReadId(name_string_id, header_.id_size())) {
    return false;
  }

  // Set current heap type
  current_heap_ = GetString(name_string_id);
  return true;
}

bool HprofParser::HandleClassDumpRecord() {
  // Class object ID
  uint64_t class_id;
  if (!iterator_->ReadId(class_id, header_.id_size()))
    return false;

  // Stack trace serial number (unused)
  uint32_t stack_trace;
  if (!iterator_->ReadU4(stack_trace))
    return false;

  // Super class ID
  uint64_t super_class_id;
  if (!iterator_->ReadId(super_class_id, header_.id_size()))
    return false;

  // Class loader ID, signers ID, protection domain ID
  uint64_t class_loader_id, signers_id, protection_domain_id;
  if (!iterator_->ReadId(class_loader_id, header_.id_size()))
    return false;
  if (!iterator_->ReadId(signers_id, header_.id_size()))
    return false;
  if (!iterator_->ReadId(protection_domain_id, header_.id_size()))
    return false;

  // Reserved (2 IDs)
  uint64_t reserved1, reserved2;
  if (!iterator_->ReadId(reserved1, header_.id_size()))
    return false;
  if (!iterator_->ReadId(reserved2, header_.id_size()))
    return false;

  // Instance size
  uint32_t instance_size;
  if (!iterator_->ReadU4(instance_size))
    return false;

  // Get class definition
  auto it = classes_.find(class_id);
  if (it == classes_.end()) {
    PERFETTO_FATAL("Class not found in LOAD_CLASS");
  }

  ClassDefinition& cls = it->second;
  cls.SetSuperClassId(super_class_id);
  cls.SetInstanceSize(instance_size);

  // Constant pool (ignored)
  uint16_t constant_pool_size;
  if (!iterator_->ReadU2(constant_pool_size))
    return false;
  for (uint16_t i = 0; i < constant_pool_size; ++i) {
    uint16_t index;
    uint8_t type_value;
    if (!iterator_->ReadU2(index))
      return false;
    if (!iterator_->ReadU1(type_value))
      return false;
    FieldType type = static_cast<FieldType>(type_value);
    size_t size = GetFieldTypeSize(type);
    if (!iterator_->SkipBytes(size))
      return false;
  }

  // Static fields
  // Ensure the class object exists in the heap graph
  HprofObject& class_obj = objects_[class_id];
  if (class_obj.id() == 0) {
    class_obj =
        HprofObject(class_id, class_id, current_heap_, ObjectType::CLASS);
    class_obj.SetHeapType(current_heap_);
  }

  auto pending_it = pending_roots_.find(class_obj.id());
  if (pending_it != pending_roots_.end()) {
    class_obj.SetRootType(pending_it->second);
    pending_roots_.erase(pending_it);
  }

  uint16_t static_field_count;
  if (!iterator_->ReadU2(static_field_count))
    return false;

  for (uint16_t i = 0; i < static_field_count; ++i) {
    uint64_t name_id;
    uint8_t type_value;

    if (!iterator_->ReadId(name_id, header_.id_size()))
      return false;
    if (!iterator_->ReadU1(type_value))
      return false;

    FieldType field_type = static_cast<FieldType>(type_value);
    std::string field_name = GetString(name_id);

    if (field_type == FieldType::OBJECT) {
      uint64_t target_id = 0;
      if (!iterator_->ReadId(target_id, header_.id_size()))
        return false;

      if (target_id != 0) {
        // Optional: infer the class of the referenced object
        uint64_t field_class_id = 0;
        auto it_o = objects_.find(target_id);
        if (it_o != objects_.end()) {
          field_class_id = it_o->second.class_id();
        }

        class_obj.AddReference(field_name, field_class_id, target_id);
        reference_count_++;
      }
    } else {
      size_t type_size = GetFieldTypeSize(field_type);
      if (!iterator_->SkipBytes(type_size))
        return false;
    }
  }

  // Instance fields
  uint16_t instance_field_count;
  if (!iterator_->ReadU2(instance_field_count))
    return false;

  std::vector<Field> fields;
  fields.reserve(instance_field_count);

  for (uint16_t i = 0; i < instance_field_count; ++i) {
    uint64_t name_id;
    uint8_t type_value;
    if (!iterator_->ReadId(name_id, header_.id_size()))
      return false;
    if (!iterator_->ReadU1(type_value))
      return false;

    std::string field_name = GetString(name_id);
    fields.emplace_back(field_name, static_cast<FieldType>(type_value));
  }

  cls.SetInstanceFields(std::move(fields));
  return true;
}

bool HprofParser::HandleInstanceDumpRecord() {
  // Object ID
  uint64_t object_id;
  if (!iterator_->ReadId(object_id, header_.id_size())) {
    return false;
  }

  // Stack trace serial number (not used)
  uint32_t stack_trace;
  if (!iterator_->ReadU4(stack_trace)) {
    return false;
  }

  // Class ID
  uint64_t class_id;
  if (!iterator_->ReadId(class_id, header_.id_size())) {
    return false;
  }

  // Instance data length
  uint32_t data_length;
  if (!iterator_->ReadU4(data_length)) {
    return false;
  }

  // Read instance data
  std::vector<uint8_t> data;
  if (!iterator_->ReadBytes(data, data_length)) {
    return false;
  }

  // Preserve root metadata if this object was already seen as a root
  bool was_root = false;
  std::optional<HprofHeapRootTag> root_type;

  auto it = objects_.find(object_id);
  if (it != objects_.end()) {
    was_root = it->second.is_root();
    root_type = it->second.root_type();
  }

  // Overwrite or create object
  HprofObject obj(object_id, class_id, current_heap_, ObjectType::INSTANCE);
  obj.SetRawData(std::move(data));
  obj.SetHeapType(current_heap_);

  if (was_root && root_type.has_value()) {
    obj.SetRootType(root_type.value());
  }

  auto pending_it = pending_roots_.find(object_id);
  if (pending_it != pending_roots_.end()) {
    obj.SetRootType(pending_it->second);
    pending_roots_.erase(pending_it);
  }

  objects_[object_id] = std::move(obj);
  instance_count_++;
  return true;
}

bool HprofParser::HandleObjectArrayDumpRecord() {
  // Array ID
  uint64_t array_id;
  if (!iterator_->ReadId(array_id, header_.id_size())) {
    return false;
  }

  // Stack trace serial number
  uint32_t stack_trace;
  if (!iterator_->ReadU4(stack_trace)) {
    return false;
  }

  // Number of elements
  uint32_t element_count;
  if (!iterator_->ReadU4(element_count)) {
    return false;
  }

  // Array class ID
  uint64_t array_class_id;
  if (!iterator_->ReadId(array_class_id, header_.id_size())) {
    return false;
  }

  // Read elements
  std::vector<uint64_t> elements;
  elements.reserve(element_count);

  for (uint32_t i = 0; i < element_count; i++) {
    uint64_t element_id;
    if (!iterator_->ReadId(element_id, header_.id_size())) {
      return false;
    }
    elements.push_back(element_id);
  }

  // Create array object
  HprofObject obj{array_id, array_class_id, current_heap_,
                  ObjectType::OBJECT_ARRAY};
  obj.SetArrayElements(std::move(elements));
  obj.SetArrayElementType(FieldType::OBJECT);
  obj.SetHeapType(current_heap_);

  auto pending_it = pending_roots_.find(obj.id());
  if (pending_it != pending_roots_.end()) {
    obj.SetRootType(pending_it->second);
    pending_roots_.erase(pending_it);
  }

  // Add object to collection
  objects_[array_id] = std::move(obj);
  object_array_count_++;

  return true;
}

bool HprofParser::HandlePrimitiveArrayDumpRecord() {
  // Array ID
  uint64_t array_id;
  if (!iterator_->ReadId(array_id, header_.id_size())) {
    return false;
  }

  // Stack trace serial number
  uint32_t stack_trace;
  if (!iterator_->ReadU4(stack_trace)) {
    return false;
  }

  // Number of elements
  uint32_t element_count;
  if (!iterator_->ReadU4(element_count)) {
    return false;
  }

  // Element type
  uint8_t element_type_value;
  if (!iterator_->ReadU1(element_type_value)) {
    return false;
  }
  FieldType element_type = static_cast<FieldType>(element_type_value);

  // Determine element size
  size_t type_size = GetFieldTypeSize(element_type);

  // Read array data
  std::vector<uint8_t> data;
  if (!iterator_->ReadBytes(data, element_count * type_size)) {
    return false;
  }

  // Lookup proper class ID for this primitive array type
  uint64_t class_id = 0;
  size_t element_type_index = static_cast<size_t>(element_type);
  if (element_type_index >= prim_array_class_ids_.size()) {
    PERFETTO_FATAL("Invalid element type: %u", element_type_value);
  } else {
    class_id = prim_array_class_ids_[element_type_index];
    if (class_id == 0) {
      PERFETTO_FATAL("Unknown class ID for primitive array type: %u",
                     element_type_value);
    }
  }

  // Create array object with correct class_id
  HprofObject obj{array_id, class_id, current_heap_,
                  ObjectType::PRIMITIVE_ARRAY};
  obj.SetRawData(std::move(data));
  obj.SetArrayElementType(element_type);
  obj.SetHeapType(current_heap_);

  auto pending_it = pending_roots_.find(obj.id());
  if (pending_it != pending_roots_.end()) {
    obj.SetRootType(pending_it->second);
    pending_roots_.erase(pending_it);
  }

  // Add to heap
  objects_[array_id] = std::move(obj);
  primitive_array_count_++;

  return true;
}

bool HprofParser::HandleRootRecord(HprofHeapRootTag tag) {
  // Object ID
  uint64_t object_id;
  if (!iterator_->ReadId(object_id, header_.id_size())) {
    return false;
  }

  switch (tag) {
    case HprofHeapRootTag::JNI_GLOBAL:
      if (!iterator_->SkipBytes(header_.id_size()))
        return false;
      break;

    case HprofHeapRootTag::JNI_LOCAL:
    case HprofHeapRootTag::JAVA_FRAME:
    case HprofHeapRootTag::JNI_MONITOR:
      if (!iterator_->SkipBytes(8))  // thread serial + frame index
        return false;
      break;

    case HprofHeapRootTag::NATIVE_STACK:
    case HprofHeapRootTag::THREAD_BLOCK:
      if (!iterator_->SkipBytes(4))  // thread serial
        return false;
      break;

    case HprofHeapRootTag::THREAD_OBJ:
      if (!iterator_->SkipBytes(8))  // thread serial + stack trace serial
        return false;
      break;

    case HprofHeapRootTag::STICKY_CLASS:
    case HprofHeapRootTag::MONITOR_USED:
    case HprofHeapRootTag::INTERNED_STRING:
    case HprofHeapRootTag::FINALIZING:
    case HprofHeapRootTag::DEBUGGER:
    case HprofHeapRootTag::VM_INTERNAL:
    case HprofHeapRootTag::UNKNOWN:
      // Most others have no extra data
      break;
  }

  root_count_++;
  pending_roots_[object_id] = tag;
  return true;
}

std::string HprofParser::NormalizeClassName(const std::string& name) {
  // Count the number of array dimensions
  int num_dimensions = 0;
  std::string normalized_name = name;

  while (!normalized_name.empty() && normalized_name[0] == '[') {
    num_dimensions++;
    normalized_name = normalized_name.substr(1);
  }

  if (num_dimensions > 0) {
    // If there was an array type signature to start, then interpret the
    // class name as a type signature.
    if (normalized_name.empty()) {
      PERFETTO_FATAL("Invalid type signature: empty after array dimensions");
    }

    char type_char = normalized_name[0];
    switch (type_char) {
      case 'Z':
        normalized_name = "boolean";
        break;
      case 'B':
        normalized_name = "byte";
        break;
      case 'C':
        normalized_name = "char";
        break;
      case 'S':
        normalized_name = "short";
        break;
      case 'I':
        normalized_name = "int";
        break;
      case 'J':
        normalized_name = "long";
        break;
      case 'F':
        normalized_name = "float";
        break;
      case 'D':
        normalized_name = "double";
        break;
      case 'L':
        // Remove the leading 'L' and trailing ';'
        if (normalized_name.back() != ';') {
          PERFETTO_FATAL("Invalid object type signature: missing semicolon");
        }
        normalized_name =
            normalized_name.substr(1, normalized_name.length() - 2);
        break;
      default:
        PERFETTO_FATAL("Invalid type signature in class name: %s",
                       normalized_name.c_str());
    }
  }

  // Replace forward slashes with dots
  std::replace(normalized_name.begin(), normalized_name.end(), '/', '.');

  // Add back array dimensions
  for (int i = 0; i < num_dimensions; ++i) {
    normalized_name += "[]";
  }

  return normalized_name;
}

bool HprofParser::ExtractReferences(HprofObject& obj,
                                    const ClassDefinition& cls) {
  const std::vector<uint8_t>& data = obj.raw_data();
  if (data.empty()) {
    return true;
  }

  std::vector<Field> fields = GetFieldsForClassHierarchy(cls.id());

  size_t offset = 0;
  for (const auto& field : fields) {
    if (offset >= data.size()) {
      break;
    }

    if (field.type() == FieldType::OBJECT) {
      uint64_t target_id = 0;

      if (header_.id_size() == 4 && offset + 4 <= data.size()) {
        target_id = (static_cast<uint64_t>(data[offset]) << 24) |
                    (static_cast<uint64_t>(data[offset + 1]) << 16) |
                    (static_cast<uint64_t>(data[offset + 2]) << 8) |
                    static_cast<uint64_t>(data[offset + 3]);
        offset += 4;
      } else if (header_.id_size() == 8 && offset + 8 <= data.size()) {
        target_id = 0;
        for (int i = 0; i < 8; i++) {
          target_id = (target_id << 8) | data[offset + static_cast<size_t>(i)];
        }
        offset += 8;
      } else {
        PERFETTO_DLOG("Invalid ID size or insufficient data");
        break;
      }

      if (target_id != 0) {
        uint64_t field_class_id = 0;
        auto it = objects_.find(target_id);
        if (it != objects_.end()) {
          field_class_id = it->second.class_id();
        }

        obj.AddReference(field.name(), field_class_id, target_id);
        reference_count_++;
      }
    } else {
      offset += field.GetSize();
    }
  }

  return true;
}

std::vector<Field> HprofParser::GetFieldsForClassHierarchy(uint64_t class_id) {
  std::vector<Field> result;

  // Follow class hierarchy to collect all fields
  uint64_t current_class_id = class_id;
  while (current_class_id != 0) {
    auto it = classes_.find(current_class_id);
    if (it == classes_.end()) {
      break;
    }

    const auto& cls = it->second;
    const auto& fields = cls.instance_fields();

    // Add fields from this class
    result.insert(result.end(), fields.begin(), fields.end());

    // Move up to superclass
    current_class_id = cls.super_class_id();
  }

  return result;
}

size_t HprofParser::GetFieldTypeSize(FieldType type) const {
  switch (type) {
    case FieldType::OBJECT:
      return header_.id_size();
    case FieldType::BOOLEAN:
    case FieldType::BYTE:
      return 1;
    case FieldType::CHAR:
    case FieldType::SHORT:
      return 2;
    case FieldType::FLOAT:
    case FieldType::INT:
      return 4;
    case FieldType::DOUBLE:
    case FieldType::LONG:
      return 8;
  }
}

std::string HprofParser::GetString(uint64_t id) const {
  auto it = strings_.find(id);
  if (it != strings_.end()) {
    return it->second;
  }
  return "[unknown string ID: " + std::to_string(id) + "]";
}

void HprofParser::FixupObjectReferencesAndRoots() {
  std::unordered_set<uint64_t> visited;

  std::function<void(HprofObject&)> process_object = [&](HprofObject& obj) {
    if (!visited.insert(obj.id()).second)
      return;

    if (obj.is_root())
      root_count_++;

    if (obj.object_type() == ObjectType::INSTANCE && !obj.raw_data().empty()) {
      auto cls_it = classes_.find(obj.class_id());
      if (cls_it != classes_.end()) {
        ExtractReferences(obj, cls_it->second);
      }
    }

    if (obj.object_type() == ObjectType::OBJECT_ARRAY) {
      const auto& elements = obj.array_elements();
      for (size_t i = 0; i < elements.size(); ++i) {
        uint64_t element_id = elements[i];
        if (element_id != 0) {
          std::string ref_name = "[" + std::to_string(i) + "]";
          obj.AddReference(ref_name, 0, element_id);
          reference_count_++;
        }
      }
    }

    for (const auto& ref : obj.references()) {
      auto it = objects_.find(ref.target_id);
      if (it != objects_.end()) {
        process_object(it->second);
      }
    }
  };

  for (auto& [id, obj] : objects_) {
    if (obj.is_root()) {
      process_object(obj);
    }
  }
}

void HeapGraph::PrintStats() const {
  PERFETTO_LOG("\n======= HPROF Heap Analysis =======");

  // Basic statistics
  PERFETTO_LOG("Total objects: %zu", GetObjectCount());
  PERFETTO_LOG("Total classes: %zu", GetClassCount());

  // Object type distribution
  std::unordered_map<ObjectType, size_t> type_counts;
  std::unordered_map<std::string, size_t> heap_counts;
  size_t total_size = 0;
  size_t root_count = 0;

  for (const auto& entry : objects_) {
    const auto& obj = entry.second;
    type_counts[obj.object_type()]++;
    heap_counts[obj.heap_type()]++;
    total_size += obj.GetSize();

    if (obj.is_root() && obj.root_type().has_value()) {
      root_count++;
    }
  }

  PERFETTO_LOG("Class objects: %zu", type_counts[ObjectType::CLASS]);
  PERFETTO_LOG("Instance objects: %zu", type_counts[ObjectType::INSTANCE]);
  PERFETTO_LOG("Object arrays: %zu", type_counts[ObjectType::OBJECT_ARRAY]);
  PERFETTO_LOG("Primitive arrays: %zu",
               type_counts[ObjectType::PRIMITIVE_ARRAY]);
  PERFETTO_LOG("Root objects: %zu", root_count);

  // Size statistics
  if (!objects_.empty()) {
    PERFETTO_LOG("Total heap size: %zu bytes", total_size);
    PERFETTO_LOG("Average object size: %zu bytes",
                 total_size / objects_.size());
  }

  // Print heaps (downgraded to DLOG)
  PERFETTO_DLOG("\n--- Heap Distribution ---");
  for (const auto& entry : heap_counts) {
    PERFETTO_DLOG("Heap type %s: %zu objects", entry.first.c_str(),
                  entry.second);
  }

  // Reference statistics
  size_t total_refs = 0;
  for (const auto& entry : objects_) {
    total_refs += entry.second.references().size();
  }
  PERFETTO_LOG("Total references: %zu", total_refs);
  if (!objects_.empty()) {
    PERFETTO_LOG(
        "Average references per object: %.2f",
        static_cast<double>(total_refs) / static_cast<double>(objects_.size()));
  }

  // Top classes (limited to top 5)
  PERFETTO_LOG("\n--- Top 5 Classes by Instance Count ---");

  std::unordered_map<uint64_t, size_t> instance_counts;
  for (const auto& entry : objects_) {
    const auto& obj = entry.second;
    if (obj.object_type() == ObjectType::INSTANCE) {
      instance_counts[obj.class_id()]++;
    }
  }

  std::vector<std::pair<uint64_t, size_t>> class_counts(instance_counts.begin(),
                                                        instance_counts.end());
  std::sort(class_counts.begin(), class_counts.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  const size_t top_n = std::min(size_t{5}, class_counts.size());
  for (size_t i = 0; i < top_n; i++) {
    uint64_t class_id = class_counts[i].first;
    size_t count = class_counts[i].second;

    std::string class_name = "[unknown]";
    auto it = classes_.find(class_id);
    if (it != classes_.end()) {
      class_name = it->second.name();
    }

    PERFETTO_LOG("%zu. %s: %zu instances", i + 1, class_name.c_str(), count);
  }

  PERFETTO_LOG("\n======= End of Analysis =======");
}

bool HeapGraph::ValidateReferences() const {
  size_t invalid_refs = 0;
  size_t self_refs = 0;
  size_t roots_with_refs = 0;
  size_t roots_without_refs = 0;

  for (const auto& entry : objects_) {
    const auto& obj = entry.second;

    // Check if this is a root object
    if (obj.is_root() && obj.root_type().has_value()) {
      if (obj.references().empty()) {
        roots_without_refs++;
      } else {
        roots_with_refs++;
      }
    }

    // Validate references
    for (const auto& ref : obj.references()) {
      if (ref.owner_id != obj.id()) {
        PERFETTO_DLOG("Inconsistent owner: ref owner=%" PRIu64
                      ", obj ID=%" PRIu64,
                      ref.owner_id, obj.id());
      }

      if (ref.owner_id == ref.target_id) {
        self_refs++;
      }

      if (objects_.find(ref.target_id) == objects_.end()) {
        invalid_refs++;
      }
    }
  }

  // Only log issues if we found any
  if (invalid_refs > 0) {
    PERFETTO_LOG("WARNING: Found %zu invalid references (target not found)",
                 invalid_refs);
  }

  if (self_refs > 0) {
    PERFETTO_DLOG("Self-references: %zu", self_refs);
  }

  // Root object validation - only warn if we have a potential issue
  if (roots_with_refs == 0 && roots_without_refs > 0) {
    PERFETTO_LOG("WARNING: %zu root objects have no outgoing references!",
                 roots_without_refs);
    PERFETTO_LOG("This may cause issues in heap analysis and visualization.");
  }

  return invalid_refs == 0;
}
}  // namespace perfetto::trace_processor::art_hprof
