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

// At the top of hprof.cpp
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "perfetto/base/logging.h"

namespace perfetto::trace_processor::art_hprof {
ByteIterator::~ByteIterator() = default;
const static std::unordered_map<std::string, FieldType>&
GetPrimitiveArrayNameMap() {
  static const auto* kMap = new std::unordered_map<std::string, FieldType>{
      {"boolean[]", FIELD_TYPE_BOOLEAN}, {"char[]", FIELD_TYPE_CHAR},
      {"float[]", FIELD_TYPE_FLOAT},     {"double[]", FIELD_TYPE_DOUBLE},
      {"byte[]", FIELD_TYPE_BYTE},       {"short[]", FIELD_TYPE_SHORT},
      {"int[]", FIELD_TYPE_INT},         {"long[]", FIELD_TYPE_LONG},
  };
  return *kMap;
}

// Field implementation
size_t Field::GetSize() const {
  switch (type_) {
    case FIELD_TYPE_OBJECT:
      return sizeof(uint64_t);  // ID size (variable)
    case FIELD_TYPE_BOOLEAN:
      return 1;
    case FIELD_TYPE_CHAR:
      return 2;
    case FIELD_TYPE_FLOAT:
      return 4;
    case FIELD_TYPE_DOUBLE:
      return 8;
    case FIELD_TYPE_BYTE:
      return 1;
    case FIELD_TYPE_SHORT:
      return 2;
    case FIELD_TYPE_INT:
      return 4;
    case FIELD_TYPE_LONG:
      return 8;
  }
}

// ArtHprofTokenizer implementation
ArtHprofTokenizer::ArtHprofTokenizer(TraceProcessorContext* ctx)
    : context_(ctx) {}

ArtHprofTokenizer::~ArtHprofTokenizer() = default;

base::Status ArtHprofTokenizer::Parse(TraceBlobView blob) {
  PERFETTO_LOG("TBV length: %zu. Size: %zu. Offset: %zu", blob.length(),
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
  PERFETTO_LOG("Zim EOF");
  HeapGraph graph = parser_->BuildGraph();
  context_->art_hprof_parser->ParseArtHprofEvent(
      0, ArtHprofEvent(std::move(graph)));
  return base::OkStatus();  // or error if reader_.empty(), your call
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

size_t ArtHprofTokenizer::TraceBlobViewIterator::GetPosition() {
  return current_offset_;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::IsEof() const {
  return !reader_.SliceOff(current_offset_, 1);
}

bool ArtHprofTokenizer::TraceBlobViewIterator::IsValid() const {
  return true;
}

// HeapGraph implementation
void HeapGraph::AddObject(HprofObject object) {
  objects_[object.id()] = object;
}

void HeapGraph::AddClass(ClassDefinition cls) {
  classes_[cls.id()] = cls;
}

void HeapGraph::AddString(uint64_t id, std::string string) {
  strings_[id] = string;
}

std::string HeapGraph::GetString(uint64_t id) const {
  auto it = strings_.find(id);
  if (it != strings_.end()) {
    return it->second;
  }
  return "[unknown string]";
}

const std::unordered_map<uint64_t, HprofObject>& HeapGraph::GetObjects() const {
  return objects_;
}

const std::unordered_map<uint64_t, ClassDefinition>& HeapGraph::GetClasses()
    const {
  return classes_;
}

size_t HeapGraph::GetObjectCount() const {
  return objects_.size();
}

size_t HeapGraph::GetClassCount() const {
  return classes_.size();
}

size_t HeapGraph::GetStringCount() const {
  return strings_.size();
}

// HprofParser implementation
HprofParser::HprofParser(std::unique_ptr<ByteIterator> iterator)
    : iterator_(std::move(iterator)) {}

HprofParser::~HprofParser() = default;

void HprofParser::Parse() {
  if (!ParseHeader()) {
    PERFETTO_FATAL("Failed to parse header");
  }

  PERFETTO_LOG("Format: %s", header_.format().c_str());
  PERFETTO_LOG("ID Size: %u", header_.id_size());
  PERFETTO_LOG("Timestamp: %" PRIu64, header_.timestamp());

  // Parse records until end of file
  size_t record_count = 0;
  while (iterator_->IsValid() && !iterator_->IsEof()) {
    record_count++;
    if (!ParseRecord()) {
      PERFETTO_LOG("Zim returning early");
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
  PERFETTO_LOG("Class dump: %zu", cli);
  PERFETTO_LOG("Record count: %zu", record_count);
}

HeapGraph HprofParser::BuildGraph() {
  FixupObjectReferencesAndRoots();

  // Build and return the heap graph
  HeapGraph graph = BuildHeapGraph();
  graph.PrintDetailedStats();
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
  uint8_t tag;
  if (!iterator_->ReadU1(tag)) {
    return false;
  }

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
    case HPROF_TAG_UTF8:
      return HandleUtf8Record(length);

    case HPROF_TAG_LOAD_CLASS:
      return HandleLoadClassRecord();

    case HPROF_TAG_HEAP_DUMP:
    case HPROF_TAG_HEAP_DUMP_SEGMENT:
      heap_dump_count_++;
      return ParseHeapDump(length);

    case HPROF_TAG_HEAP_DUMP_END:
      // Nothing to do for this tag
      return true;

    default:
      // Skip unknown tags
      return iterator_->SkipBytes(length);
  }
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
  std::string class_name = GetString(name_id);

  // Store class definition
  ClassDefinition class_def(class_obj_id, class_name);
  classes_[class_obj_id] = class_def;
  class_count_++;

  const auto& primitive_map = GetPrimitiveArrayNameMap();
  auto it = primitive_map.find(class_name);
  if (it != primitive_map.end()) {
    prim_array_class_ids_[static_cast<size_t>(it->second)] = class_obj_id;
    PERFETTO_LOG("Registered class ID %" PRIu64 " for primitive array type %s",
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
  uint8_t tag;
  if (!iterator_->ReadU1(tag)) {
    return false;
  }

  // Handle sub-record based on tag
  switch (tag) {
    case HPROF_HEAP_TAG_ROOT_JNI_GLOBAL:
    case HPROF_HEAP_TAG_ROOT_JNI_LOCAL:
    case HPROF_HEAP_TAG_ROOT_JAVA_FRAME:
    case HPROF_HEAP_TAG_ROOT_NATIVE_STACK:
    case HPROF_HEAP_TAG_ROOT_STICKY_CLASS:
    case HPROF_HEAP_TAG_ROOT_THREAD_BLOCK:
    case HPROF_HEAP_TAG_ROOT_MONITOR_USED:
    case HPROF_HEAP_TAG_ROOT_THREAD_OBJ:
    case HPROF_HEAP_TAG_ROOT_INTERNED_STRING:
    case HPROF_HEAP_TAG_ROOT_FINALIZING:
    case HPROF_HEAP_TAG_ROOT_DEBUGGER:
    case HPROF_HEAP_TAG_ROOT_VM_INTERNAL:
    case HPROF_HEAP_TAG_ROOT_JNI_MONITOR:
    case HPROF_HEAP_TAG_ROOT_UNKNOWN:
      return HandleRootRecord(static_cast<HprofHeapRootTag>(tag));

    case HPROF_HEAP_TAG_CLASS_DUMP:
      return HandleClassDumpRecord();

    case HPROF_HEAP_TAG_INSTANCE_DUMP:
      return HandleInstanceDumpRecord();

    case HPROF_HEAP_TAG_OBJ_ARRAY_DUMP:
      return HandleObjectArrayDumpRecord();

    case HPROF_HEAP_TAG_PRIM_ARRAY_DUMP:
      return HandlePrimitiveArrayDumpRecord();

    case HPROF_HEAP_TAG_HEAP_DUMP_INFO:
      return HandleHeapDumpInfoRecord();

    default:
      PERFETTO_LOG("Unknown HEAP_DUMP sub-record tag: 0x%02x", tag);
      return false;
  }
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
  cli++;
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
    uint8_t type;
    if (!iterator_->ReadU2(index))
      return false;
    if (!iterator_->ReadU1(type))
      return false;
    size_t size = GetFieldTypeSize(static_cast<FieldType>(type));
    if (!iterator_->SkipBytes(size))
      return false;
  }

  // Static fields
  // Ensure the class object exists in the heap graph
  HprofObject& class_obj = objects_[class_id];
  if (class_obj.id() == 0) {
    class_obj = HprofObject(class_id, class_id, current_heap_,
                            ObjectType::OBJECT_TYPE_CLASS);
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
    uint8_t type;

    if (!iterator_->ReadId(name_id, header_.id_size()))
      return false;
    if (!iterator_->ReadU1(type))
      return false;

    FieldType field_type = static_cast<FieldType>(type);
    std::string field_name = GetString(name_id);

    if (field_type == FIELD_TYPE_OBJECT) {
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
  for (uint16_t i = 0; i < instance_field_count; ++i) {
    uint64_t name_id;
    uint8_t type;
    if (!iterator_->ReadId(name_id, header_.id_size()))
      return false;
    if (!iterator_->ReadU1(type))
      return false;

    std::string field_name = GetString(name_id);
    fields.emplace_back(field_name, static_cast<FieldType>(type));
  }

  cls.SetInstanceFields(fields);
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
  HprofObject obj(object_id, class_id, current_heap_,
                  ObjectType::OBJECT_TYPE_INSTANCE);
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
                  ObjectType::OBJECT_TYPE_OBJECT_ARRAY};
  obj.SetArrayElements(std::move(elements));
  obj.SetArrayElementType(FieldType::FIELD_TYPE_OBJECT);
  obj.SetHeapType(current_heap_);

  auto pending_it = pending_roots_.find(obj.id());
  if (pending_it != pending_roots_.end()) {
    obj.SetRootType(pending_it->second);
    pending_roots_.erase(pending_it);
  }

  // Add object to collection
  objects_[array_id] = obj;
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
  uint8_t element_type_u8;
  if (!iterator_->ReadU1(element_type_u8)) {
    return false;
  }
  FieldType element_type = static_cast<FieldType>(element_type_u8);

  // Determine element size
  size_t type_size = GetFieldTypeSize(element_type);

  // Read array data
  std::vector<uint8_t> data;
  if (!iterator_->ReadBytes(data, element_count * type_size)) {
    return false;
  }

  // Lookup proper class ID for this primitive array type
  uint64_t class_id = 0;
  if (element_type >= prim_array_class_ids_.size()) {
    PERFETTO_FATAL("Invalid element type: %u", element_type_u8);
  } else {
    class_id = prim_array_class_ids_[static_cast<size_t>(element_type)];
    if (class_id == 0) {
      PERFETTO_FATAL("Unknown class ID for primitive array type: %u",
                     element_type_u8);
    }
  }

  // Create array object with correct class_id
  HprofObject obj{array_id, class_id, current_heap_,
                  ObjectType::OBJECT_TYPE_PRIMITIVE_ARRAY};
  obj.SetRawData(std::move(data));
  obj.SetArrayElementType(element_type);

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

bool HprofParser::HandleRootRecord(uint8_t tag) {
  // Object ID
  uint64_t object_id;
  if (!iterator_->ReadId(object_id, header_.id_size())) {
    return false;
  }

  switch (tag) {
    case HPROF_HEAP_TAG_ROOT_JNI_GLOBAL:
      if (!iterator_->SkipBytes(header_.id_size()))
        return false;
      break;

    case HPROF_HEAP_TAG_ROOT_JNI_LOCAL:
    case HPROF_HEAP_TAG_ROOT_JAVA_FRAME:
    case HPROF_HEAP_TAG_ROOT_JNI_MONITOR:
      if (!iterator_->SkipBytes(8))  // thread serial + frame index
        return false;
      break;

    case HPROF_HEAP_TAG_ROOT_NATIVE_STACK:
    case HPROF_HEAP_TAG_ROOT_THREAD_BLOCK:
      if (!iterator_->SkipBytes(4))  // thread serial
        return false;
      break;

    case HPROF_HEAP_TAG_ROOT_THREAD_OBJ:
      if (!iterator_->SkipBytes(8))  // thread serial + stack trace serial
        return false;
      break;

    default:
      // Most others (e.g. ROOT_UNKNOWN, STICKY_CLASS, etc.) have no extra data
      break;
  }

  root_count_++;
  pending_roots_[object_id] = static_cast<HprofHeapRootTag>(tag);
  return true;
}

void HprofObject::AddReference(const std::string& field_name,
                               uint64_t field_class_id,
                               uint64_t target_id) {
  references_.emplace_back(id_, field_name, field_class_id, target_id);
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

    if (field.type() == FieldType::FIELD_TYPE_OBJECT) {
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
    case FIELD_TYPE_OBJECT:
      return header_.id_size();
    case FIELD_TYPE_BOOLEAN:
      return 1;
    case FIELD_TYPE_CHAR:
      return 2;
    case FIELD_TYPE_FLOAT:
      return 4;
    case FIELD_TYPE_DOUBLE:
      return 8;
    case FIELD_TYPE_BYTE:
      return 1;
    case FIELD_TYPE_SHORT:
      return 2;
    case FIELD_TYPE_INT:
      return 4;
    case FIELD_TYPE_LONG:
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

// Convert root type ID to string
std::string HeapGraph::GetRootType(uint8_t root_type) {
  switch (root_type) {
    case HPROF_HEAP_TAG_ROOT_JNI_GLOBAL:
      return "jni_global";
    case HPROF_HEAP_TAG_ROOT_JNI_LOCAL:
      return "jni_local";
    case HPROF_HEAP_TAG_ROOT_JAVA_FRAME:
      return "java_frame";
    case HPROF_HEAP_TAG_ROOT_NATIVE_STACK:
      return "native_stack";
    case HPROF_HEAP_TAG_ROOT_STICKY_CLASS:
      return "sticky_class";
    case HPROF_HEAP_TAG_ROOT_THREAD_BLOCK:
      return "thread_block";
    case HPROF_HEAP_TAG_ROOT_MONITOR_USED:
      return "monitor_used";
    case HPROF_HEAP_TAG_ROOT_THREAD_OBJ:
      return "thread_object";
    case HPROF_HEAP_TAG_ROOT_INTERNED_STRING:
      return "interned_string";
    case HPROF_HEAP_TAG_ROOT_FINALIZING:
      return "finalizing";
    case HPROF_HEAP_TAG_ROOT_DEBUGGER:
      return "debugger";
    case HPROF_HEAP_TAG_ROOT_VM_INTERNAL:
      return "vm_internal";
    case HPROF_HEAP_TAG_ROOT_JNI_MONITOR:
      return "jni_monitor";
    default:
      return "unknown";
  }
}

HeapGraph HprofParser::BuildHeapGraph() {
  HeapGraph graph;

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

void HprofParser::FixupObjectReferencesAndRoots() {
  std::unordered_set<uint64_t> visited;

  std::function<void(HprofObject&)> process_object = [&](HprofObject& obj) {
    if (!visited.insert(obj.id()).second)
      return;

    if (obj.is_root())
      root_count_++;

    if (obj.object_type() == ObjectType::OBJECT_TYPE_INSTANCE &&
        !obj.raw_data().empty()) {
      auto cls_it = classes_.find(obj.class_id());
      if (cls_it != classes_.end()) {
        ExtractReferences(obj, cls_it->second);
      }
    }

    if (obj.object_type() == ObjectType::OBJECT_TYPE_OBJECT_ARRAY) {
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

void HeapGraph::PrintObjectTypeDistribution() const {
  std::unordered_map<ObjectType, size_t> type_counts;
  std::unordered_map<std::string, size_t> heap_counts;
  size_t total_size = 0;

  for (const auto& entry : objects_) {
    const auto& obj = entry.second;
    type_counts[obj.object_type()]++;
    heap_counts[obj.heap_type()]++;
    total_size += obj.GetSize();
  }

  PERFETTO_LOG("\n--- Object Type Distribution ---");
  PERFETTO_LOG("Class objects: %zu",
               type_counts[ObjectType::OBJECT_TYPE_CLASS]);
  PERFETTO_LOG("Instance objects: %zu",
               type_counts[ObjectType::OBJECT_TYPE_INSTANCE]);
  PERFETTO_LOG("Object arrays: %zu",
               type_counts[ObjectType::OBJECT_TYPE_OBJECT_ARRAY]);
  PERFETTO_LOG("Primitive arrays: %zu",
               type_counts[ObjectType::OBJECT_TYPE_PRIMITIVE_ARRAY]);

  PERFETTO_LOG("\n--- Heap Distribution ---");
  for (const auto& entry : heap_counts) {
    PERFETTO_LOG("Heap type %s: %zu objects", entry.first.c_str(),
                 entry.second);
  }

  PERFETTO_LOG("\n--- Size Statistics ---");
  PERFETTO_LOG("Total object size: %zu bytes", total_size);
  if (!objects_.empty()) {
    PERFETTO_LOG("Average object size: %zu bytes",
                 total_size / objects_.size());
  }
}

void HeapGraph::PrintRootDistribution() const {
  std::unordered_map<HprofHeapRootTag, size_t> root_counts;
  size_t total_roots = 0;

  for (const auto& entry : objects_) {
    const auto& obj = entry.second;
    if (obj.is_root() && obj.root_type().has_value()) {
      root_counts[obj.root_type().value()]++;
      total_roots++;
    }
  }

  PERFETTO_LOG("\n--- Root Distribution ---");
  PERFETTO_LOG("Total roots: %zu", total_roots);

  // Helper function to print a root type
  auto print_root_type = [&](HprofHeapRootTag tag, const char* name) {
    auto it = root_counts.find(tag);
    if (it != root_counts.end() && it->second > 0) {
      PERFETTO_LOG("%s: %zu", name, it->second);
    }
  };

  print_root_type(HPROF_HEAP_TAG_ROOT_JNI_GLOBAL, "JNI Global");
  print_root_type(HPROF_HEAP_TAG_ROOT_JNI_LOCAL, "JNI Local");
  print_root_type(HPROF_HEAP_TAG_ROOT_JAVA_FRAME, "Java Frame");
  print_root_type(HPROF_HEAP_TAG_ROOT_NATIVE_STACK, "Native Stack");
  print_root_type(HPROF_HEAP_TAG_ROOT_STICKY_CLASS, "Sticky Class");
  print_root_type(HPROF_HEAP_TAG_ROOT_THREAD_BLOCK, "Thread Block");
  print_root_type(HPROF_HEAP_TAG_ROOT_MONITOR_USED, "Monitor Used");
  print_root_type(HPROF_HEAP_TAG_ROOT_THREAD_OBJ, "Thread Object");
  print_root_type(HPROF_HEAP_TAG_ROOT_INTERNED_STRING,
                  "Interned String (Android)");
  print_root_type(HPROF_HEAP_TAG_ROOT_FINALIZING, "Finalizing (Android)");
  print_root_type(HPROF_HEAP_TAG_ROOT_DEBUGGER, "Debugger (Android)");
  print_root_type(HPROF_HEAP_TAG_ROOT_VM_INTERNAL, "VM Internal (Android)");
  print_root_type(HPROF_HEAP_TAG_ROOT_JNI_MONITOR, "JNI Monitor (Android)");
  print_root_type(HPROF_HEAP_TAG_ROOT_UNKNOWN, "Unknown");
}

void HeapGraph::PrintTopClasses(size_t top_n) const {
  std::unordered_map<uint64_t, size_t> instance_counts;
  std::unordered_map<uint64_t, size_t> class_total_size;

  for (const auto& entry : objects_) {
    const auto& obj = entry.second;
    if (obj.object_type() == ObjectType::OBJECT_TYPE_INSTANCE) {
      instance_counts[obj.class_id()]++;
      class_total_size[obj.class_id()] += obj.GetSize();
    }
  }

  std::vector<std::pair<uint64_t, size_t>> class_counts(instance_counts.begin(),
                                                        instance_counts.end());
  std::sort(class_counts.begin(), class_counts.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  if (class_counts.size() > top_n) {
    class_counts.resize(top_n);
  }

  PERFETTO_LOG("\n--- Top %zu Classes by Instance Count ---", top_n);
  PERFETTO_LOG("Rank | Count | Total Size | Class Name");
  PERFETTO_LOG("-----|-------|------------|------------");

  for (size_t i = 0; i < class_counts.size(); i++) {
    uint64_t class_id = class_counts[i].first;
    size_t count = class_counts[i].second;
    size_t total_size = class_total_size[class_id];

    std::string class_name = "[unknown]";
    auto it = classes_.find(class_id);
    if (it != classes_.end()) {
      class_name = it->second.name();
    }

    PERFETTO_LOG("%-4zu | %-5zu | %-10zu | %s", i + 1, count, total_size,
                 class_name.c_str());
  }
}

bool HeapGraph::ValidateReferences() const {
  size_t total_refs = 0;
  size_t invalid_refs = 0;
  size_t self_refs = 0;
  size_t null_owner_refs = 0;

  // Track root objects specifically
  size_t total_root_objects = 0;
  size_t roots_with_refs = 0;
  size_t roots_without_refs = 0;
  std::vector<uint64_t> root_ids_without_refs;

  for (const auto& entry : objects_) {
    const auto& obj = entry.second;

    // Check if this is a root object
    if (obj.is_root() && obj.root_type().has_value()) {
      total_root_objects++;
      if (obj.references().empty()) {
        roots_without_refs++;
        if (root_ids_without_refs.size() < 10) {
          root_ids_without_refs.push_back(obj.id());
        }
      } else {
        roots_with_refs++;
      }
    }

    // Regular reference validation
    for (const auto& ref : obj.references()) {
      total_refs++;

      if (ref.owner_id != obj.id()) {
        null_owner_refs++;
        if (null_owner_refs <= 10) {
          PERFETTO_LOG("Inconsistent owner: ref owner=%" PRIu64
                       ", obj ID=%" PRIu64,
                       ref.owner_id, obj.id());
        }
      }

      if (ref.owner_id == ref.target_id) {
        self_refs++;
      }

      if (objects_.find(ref.target_id) == objects_.end()) {
        invalid_refs++;
        if (invalid_refs <= 10) {
          PERFETTO_LOG("Invalid target: ref from %" PRIu64 " -> target %" PRIu64
                       " not found",
                       ref.owner_id, ref.target_id);
        }
      }
    }
  }

  PERFETTO_LOG("\n--- Reference Validation ---");
  PERFETTO_LOG("Total references: %zu", total_refs);
  PERFETTO_LOG("Invalid references (target not found): %zu", invalid_refs);
  PERFETTO_LOG("Self-references: %zu", self_refs);
  PERFETTO_LOG("Inconsistent owner IDs: %zu", null_owner_refs);

  // Root object validation
  PERFETTO_LOG("\n--- Root Object Validation ---");
  PERFETTO_LOG("Total root objects: %zu", total_root_objects);
  PERFETTO_LOG("Root objects with outgoing references: %zu", roots_with_refs);
  PERFETTO_LOG("Root objects without outgoing references: %zu",
               roots_without_refs);

  // Print some example root object IDs
  if (!root_ids_without_refs.empty()) {
    PERFETTO_LOG("Sample root object IDs without refs:");
    for (size_t i = 0; i < root_ids_without_refs.size(); i++) {
      uint64_t root_id = root_ids_without_refs[i];
      auto it = objects_.find(root_id);
      if (it != objects_.end()) {
        const auto& root_obj = it->second;
        std::string root_type = "unknown";
        if (root_obj.root_type().has_value()) {
          root_type = GetRootType(root_obj.root_type().value());
        }

        std::string class_name = "[unknown class]";
        auto class_it = classes_.find(root_obj.class_id());
        if (class_it != classes_.end()) {
          class_name = class_it->second.name();
        }

        PERFETTO_LOG("  Root ID: %" PRIu64 ", Type: %s, Class: %s", root_id,
                     root_type.c_str(), class_name.c_str());
      }
    }
  }

  // Check if references from root objects are being added properly
  bool roots_without_refs_problem =
      (total_root_objects > 0 && roots_with_refs == 0);
  if (roots_without_refs_problem) {
    PERFETTO_LOG(
        "\nWARNING: None of the root objects have outgoing references!");
    PERFETTO_LOG(
        "This suggests an issue in HandleRootRecord or ExtractReferences for "
        "root objects.");
    PERFETTO_LOG(
        "Check if references are being properly identified during parsing.");
  }

  return invalid_refs == 0 && null_owner_refs == 0 &&
         !roots_without_refs_problem;
}

void HeapGraph::PrintReferenceStats() const {
  size_t total_refs = 0;
  size_t max_refs_from_object = 0;
  uint64_t max_refs_object_id = 0;
  std::string max_refs_class_name = "[unknown]";

  std::unordered_map<uint64_t, size_t> refs_to_object;

  for (const auto& entry : objects_) {
    const auto& obj = entry.second;
    size_t ref_count = obj.references().size();
    total_refs += ref_count;

    for (const auto& ref : obj.references()) {
      refs_to_object[ref.target_id]++;
    }

    if (ref_count > max_refs_from_object) {
      max_refs_from_object = ref_count;
      max_refs_object_id = obj.id();

      auto class_it = classes_.find(obj.class_id());
      if (class_it != classes_.end()) {
        max_refs_class_name = class_it->second.name();
      }
    }
  }

  uint64_t most_referenced_id = 0;
  size_t most_referenced_count = 0;
  std::string most_referenced_class_name = "[unknown]";

  for (const auto& entry : refs_to_object) {
    if (entry.second > most_referenced_count) {
      most_referenced_count = entry.second;
      most_referenced_id = entry.first;

      auto obj_it = objects_.find(most_referenced_id);
      if (obj_it != objects_.end()) {
        auto class_it = classes_.find(obj_it->second.class_id());
        if (class_it != classes_.end()) {
          most_referenced_class_name = class_it->second.name();
        }
      }
    }
  }

  double avg_refs_per_obj =
      static_cast<double>(total_refs) / static_cast<double>(objects_.size());

  size_t objects_with_no_refs = 0;
  for (const auto& entry : objects_) {
    if (entry.second.references().empty()) {
      objects_with_no_refs++;
    }
  }

  PERFETTO_LOG("\n--- Reference Statistics ---");
  PERFETTO_LOG("Average references per object: %.2f", avg_refs_per_obj);
  PERFETTO_LOG("Objects with no outgoing references: %zu (%.1f%%)",
               objects_with_no_refs,
               (static_cast<double>(objects_with_no_refs) * 100.0 /
                static_cast<double>(objects_.size())));

  PERFETTO_LOG("\nObject with most outgoing references:");
  PERFETTO_LOG("  ID: 0x%" PRIx64, max_refs_object_id);
  PERFETTO_LOG("  Class: %s", max_refs_class_name.c_str());
  PERFETTO_LOG("  Reference count: %zu", max_refs_from_object);

  PERFETTO_LOG("\nMost referenced object:");
  PERFETTO_LOG("  ID: 0x%" PRIx64, most_referenced_id);
  PERFETTO_LOG("  Class: %s", most_referenced_class_name.c_str());
  PERFETTO_LOG("  Referenced by: %zu objects", most_referenced_count);
}

void HeapGraph::PrintDetailedStats() const {
  PERFETTO_LOG("\n======= HPROF Heap Analysis =======");

  PERFETTO_LOG("\n--- Basic Statistics ---");
  PERFETTO_LOG("Total objects: %zu", GetObjectCount());
  PERFETTO_LOG("Total classes: %zu", GetClassCount());
  PERFETTO_LOG("Total strings: %zu", GetStringCount());

  PrintObjectTypeDistribution();
  PrintRootDistribution();
  PrintTopClasses();
  ValidateReferences();
  PrintReferenceStats();

  PERFETTO_LOG("\n======= End of Analysis =======");
}
}  // namespace perfetto::trace_processor::art_hprof
