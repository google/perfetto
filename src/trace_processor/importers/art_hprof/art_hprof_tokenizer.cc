/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/importers/art_hprof/art_hprof_tokenizer.h"
#include "src/trace_processor/sorter/trace_sorter.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace perfetto::trace_processor::art_hprof {

// Constants
constexpr uint8_t HPROF_ROOT_RECORD_MIN = 0x01;
constexpr uint8_t HPROF_ROOT_RECORD_MAX = 0x0A;

// HprofParser implementation
HprofParser::~HprofParser() = default;

// Define the virtual destructor out-of-line to fix weak vtable warning
ByteIterator::~ByteIterator() = default;

HprofData HprofParser::Parse() {
  PERFETTO_DLOG("Beginning to parse HPROF");

  data_ = HprofData{};  // Reset the member variable instead of creating a local
                        // variable

  ParseHeader(data_);

  // Main parsing loop with centralized dispatch
  while (HasMoreData()) {
    RecordHeader header = ReadRecordHeader();
    ParseRecordByType(header, data_);
  }

  // Summary statistics
  PERFETTO_LOG(
      "Parsing Summary - String count: %zu, Class count: %zu, "
      "Heap dump count: %zu, Class instance count: %zu, "
      "Object array count: %zu, Primitive array count: %zu, "
      "Root count: %zu, Field reference count: %zu, "
      "Heap info count: %zu",
      data_.string_count, data_.class_count, data_.heap_dump_count,
      data_.class_instance_count, data_.object_array_count,
      data_.primitive_array_count, data_.root_count,
      data_.field_reference_count, data_.heap_info_count);

  return data_;
}

void HprofParser::ParseHeader(HprofData& data) {
  PERFETTO_DLOG("Parsing HPROF header");

  // Read format string until null terminator
  char c;
  data.header.format = "";
  while (byte_iterator_->ReadU1(reinterpret_cast<uint8_t&>(c)) && c != 0) {
    data.header.format.push_back(c);
  }

  // Read identifier size
  if (!byte_iterator_->ReadU4(data.header.identifier_size)) {
    PERFETTO_FATAL("Error: Failed to read ID size");
  }

  identifier_size_ = data.header.identifier_size;

  // Read timestamp (high and low 32 bits)
  uint32_t high_time, low_time;
  if (!byte_iterator_->ReadU4(high_time) || !byte_iterator_->ReadU4(low_time)) {
    PERFETTO_FATAL("Error: Failed to read timestamp");
  }

  data.header.timestamp = (static_cast<uint64_t>(high_time) << 32) | low_time;

  PERFETTO_DLOG("HPROF header: format=%s, idSize=%u",
                data.header.format.c_str(), identifier_size_);
}

bool HprofParser::HasMoreData() const {
  return byte_iterator_->IsValid() && !byte_iterator_->IsEof();
}

size_t HprofParser::GetPosition() const {
  return byte_iterator_->GetPosition();
}

bool HprofParser::IsEof() const {
  return byte_iterator_->IsEof();
}

HprofParser::RecordHeader HprofParser::ReadRecordHeader() {
  RecordHeader header;

  // Try to read the tag
  if (!byte_iterator_->ReadU1(header.tag)) {
    PERFETTO_FATAL("Failed to read record tag");
  }

  // Read time and length
  if (!byte_iterator_->ReadU4(header.time) ||
      !byte_iterator_->ReadU4(header.length)) {
    PERFETTO_FATAL("Failed to read record time/length");
  }

  PERFETTO_DLOG("Reading record with tag: 0x%x, time: %u, length: %u",
                static_cast<int>(header.tag), header.time, header.length);

  return header;
}

void HprofParser::SkipRecord(uint32_t length) {
  PERFETTO_DLOG("Skipping record payload of length %u", length);
  if (!byte_iterator_->SkipBytes(length)) {
    PERFETTO_FATAL("Failed to skip record payload");
  }
}

void HprofParser::ParseRecordByType(const RecordHeader& header,
                                    HprofData& data) {
  switch (header.tag) {
    case HPROF_UTF8:
      ParseUtf8Record(header, data);
      break;
    case HPROF_LOAD_CLASS:
      ParseLoadClassRecord(header, data);
      break;
    case HPROF_HEAP_DUMP:
    case HPROF_HEAP_DUMP_SEGMENT:
      ParseHeapDumpRecord(header, data);
      break;
    case HPROF_HEAP_DUMP_END:
      // End of a heap dump segment
      PERFETTO_DLOG("Encountered HEAP_DUMP_END tag");
      {
        HprofRecord record;
        record.tag = header.tag;
        record.time = header.time;
        record.length = header.length;
        record.data = std::monostate{};
        data.records.push_back(record);
      }
      break;
    default:
      // Generic record - skip the payload
      PERFETTO_DLOG("Skipping unknown record payload of length %u",
                    header.length);
      SkipRecord(header.length);
      {
        HprofRecord record;
        record.tag = header.tag;
        record.time = header.time;
        record.length = header.length;
        record.data = std::monostate{};
        data.records.push_back(record);
      }
      break;
  }
}

void HprofParser::ParseUtf8Record(const RecordHeader& header, HprofData& data) {
  PERFETTO_DLOG("Parsing UTF8 record");

  Utf8StringData utf8_data;
  uint64_t name_id;

  if (!byte_iterator_->ReadId(name_id, identifier_size_)) {
    PERFETTO_FATAL("Failed to read string ID");
  }
  utf8_data.name_id = name_id;

  size_t string_length = header.length - identifier_size_;
  if (!byte_iterator_->ReadString(utf8_data.utf8_string, string_length)) {
    PERFETTO_FATAL("Failed to read string data");
  }

  PERFETTO_DLOG("Read UTF8 string: ID=%" PRIu64 ", string='%s'", name_id,
                utf8_data.utf8_string.c_str());

  // Always store 0 as "???" like in the Java implementation
  if (name_id == 0) {
    utf8_data.utf8_string = "???";
  }

  HprofRecord record;
  record.tag = header.tag;
  record.time = header.time;
  record.length = header.length;
  record.data = utf8_data;
  data.records.push_back(record);

  // Store string for later reference (including the special case for ID 0)
  data.id_to_string_map[utf8_data.name_id] = utf8_data.utf8_string;
  data.string_count++;
}

void HprofParser::ParseLoadClassRecord(const RecordHeader& header,
                                       HprofData& data) {
  PERFETTO_DLOG("Parsing LOAD_CLASS record");

  LoadClassData class_data_record;

  if (!byte_iterator_->ReadU4(class_data_record.class_serial_num) ||
      !byte_iterator_->ReadId(class_data_record.class_object_id,
                              identifier_size_) ||
      !byte_iterator_->ReadU4(class_data_record.stack_trace_serial_num) ||
      !byte_iterator_->ReadId(class_data_record.class_name_id,
                              identifier_size_)) {
    PERFETTO_FATAL("Failed to read load class record data");
  }

  std::string raw_class_name;
  auto name_it = data.id_to_string_map.find(class_data_record.class_name_id);
  if (name_it != data.id_to_string_map.end()) {
    raw_class_name = name_it->second;
  } else {
    PERFETTO_FATAL("Class name string ID %" PRIu64
                   " not found in id_to_string_map "
                   "for LOAD_CLASS record with class_object_id %" PRIu64 ".",
                   class_data_record.class_name_id,
                   class_data_record.class_object_id);
  }

  // Normalize the class name (like Java implementation)
  std::string normalized_class_name = NormalizeClassName(raw_class_name);
  class_data_record.class_name = normalized_class_name;

  PERFETTO_DLOG("Class loaded: serial=%u, obj_id=%" PRIu64 ", name_id=%" PRIu64
                ", "
                "raw_name='%s', normalized_name='%s'",
                class_data_record.class_serial_num,
                class_data_record.class_object_id,
                class_data_record.class_name_id, raw_class_name.c_str(),
                normalized_class_name.c_str());

  // Update the class info with the normalized name
  ClassInfo& class_info = data.classes[class_data_record.class_object_id];
  class_info.name = normalized_class_name;
  class_info.class_object_id = class_data_record.class_object_id;

  // Check if this is a String class (similar to Java implementation)
  class_info.is_string_class = IsStringClass(normalized_class_name);

  // for (int i = 0; i < Type::TYPE_COUNT; i++) {
  //     Type type = static_cast<Type>(i);
  //     std::string type_array_name = GetTypeName(type) + "[]";
  //     if (normalized_class_name == type_array_name) {
  //         data.primitive_array_class_ids[type] =
  //         class_data_record.class_object_id; PERFETTO_DLOG("Found primitive
  //         array class: %s (ID: %" PRIu64 ")",
  //                       normalized_class_name.c_str(),
  //                       class_data_record.class_object_id);
  //         break;
  //     }
  // }

  // Special handling for java.lang.Class (like Java implementation)
  if (normalized_class_name == "java.lang.Class") {
    data.java_lang_class_object_id = class_data_record.class_object_id;
  }

  // Store the record
  HprofRecord record;
  record.tag = header.tag;
  record.time = header.time;
  record.length = header.length;
  record.data = class_data_record;
  data.records.push_back(record);

  // Map class serial to class ID
  data.class_serial_to_id[class_data_record.class_serial_num] =
      class_data_record.class_object_id;
  data.class_count++;
}

void HprofParser::ParseHeapDumpRecord(const RecordHeader& header,
                                      HprofData& data) {
  PERFETTO_DLOG("Parsing HEAP_DUMP or HEAP_DUMP_SEGMENT record");

  HeapDumpData heap_data;

  // Record the end position
  size_t end_pos = GetPosition() + header.length;

  // Parse heap dump sub-records
  while (GetPosition() < end_pos && !IsEof()) {
    uint8_t sub_tag;
    if (!byte_iterator_->ReadU1(sub_tag)) {
      if (IsEof()) {
        break;
      }
      PERFETTO_FATAL("Failed to read heap dump sub-record tag");
    }

    PERFETTO_DLOG("Parsing heap sub-record with tag: 0x%x",
                  static_cast<int>(sub_tag));

    // Centralized sub-record dispatch
    switch (sub_tag) {
      case HPROF_ROOT_JNI_GLOBAL:
        ParseRootJniGlobal(data);
        break;
      case HPROF_ROOT_JNI_LOCAL:
      case HPROF_ROOT_JAVA_FRAME:
      case HPROF_ROOT_THREAD_BLOCK:
        ParseRootWithThread(data, sub_tag);
        break;
      case HPROF_ROOT_NATIVE_STACK:
      case HPROF_ROOT_STICKY_CLASS:
      case HPROF_ROOT_MONITOR_USED:
      case HPROF_ROOT_INTERNED_STRING:
      case HPROF_ROOT_FINALIZING:
      case HPROF_ROOT_DEBUGGER:
      case HPROF_ROOT_VM_INTERNAL:
      case HPROF_ROOT_JNI_MONITOR:
      case HPROF_ROOT_UNKNOWN:
        ParseSimpleRoot(data, sub_tag);
        break;
      case HPROF_ROOT_THREAD_OBJ:
        ParseThreadObjectRoot(data);
        break;
      case HPROF_HEAP_DUMP_INFO:
        ParseHeapDumpInfo(data);
        break;
      case HPROF_CLASS_DUMP:
        ParseClassDump(data);
        break;
      case HPROF_INSTANCE_DUMP:
        ParseInstanceDump(data);
        break;
      case HPROF_OBJ_ARRAY_DUMP:
        ParseObjectArrayDump(data);
        break;
      case HPROF_PRIM_ARRAY_DUMP:
        ParsePrimitiveArrayDump(data);
        break;
      default:
        PERFETTO_DLOG("Unknown sub-record tag: 0x%x",
                      static_cast<int>(sub_tag));
        SkipUnknownSubRecord(sub_tag);
        break;
    }

    // Safety check: if we've gone past the end position or hit EOF, break
    if (GetPosition() >= end_pos || IsEof()) {
      break;
    }
  }

  HprofRecord record;
  record.tag = header.tag;
  record.time = header.time;
  record.length = header.length;
  record.data = heap_data;
  data.records.push_back(record);
  data.heap_dump_count++;
}

void HprofParser::SkipUnknownSubRecord(uint8_t sub_tag) {
  PERFETTO_DLOG("Skipping unknown sub-record with tag: 0x%x",
                static_cast<int>(sub_tag));

  // Simple root records with just an object ID
  if (sub_tag >= HPROF_ROOT_RECORD_MIN && sub_tag <= HPROF_ROOT_RECORD_MAX) {
    byte_iterator_->SkipBytes(identifier_size_);
  } else {
    // For other unknown tags, skip a byte
    byte_iterator_->SkipBytes(1);
  }
}

// The rest of the implementation remains the same as before,
// including helper methods and sub-record handling methods
// like ParseRootJniGlobal, ParseClassDump, etc.

void HprofParser::ParseRootJniGlobal(HprofData& data) {
  PERFETTO_DLOG("Parsing JNI GLOBAL root");

  RootRecordData record_data;
  record_data.root_type = HPROF_ROOT_JNI_GLOBAL;

  uint64_t global_ref_id;  // Temporary variable for the second ID
  if (!byte_iterator_->ReadId(record_data.object_id, identifier_size_) ||
      !byte_iterator_->ReadId(global_ref_id, identifier_size_)) {
    PERFETTO_FATAL("Failed to read JNI GLOBAL root");
  }

  HprofHeapRecord record;
  record.tag = static_cast<HprofHeapTag>(HPROF_ROOT_JNI_GLOBAL);
  record.data = record_data;

  // Add to current heap dump
  if (!data.records.empty() &&
      (std::holds_alternative<HeapDumpData>(data.records.back().data))) {
    auto& heap_dump = std::get<HeapDumpData>(data.records.back().data);
    heap_dump.records.push_back(record);
  }

  // Store root information in hprof
  data.root_objects[record_data.object_id] = HPROF_ROOT_JNI_GLOBAL;
  data.root_count++;
}

void HprofParser::ParseRootWithThread(HprofData& data, uint8_t sub_tag_value) {
  PERFETTO_DLOG("Parsing thread-related root");

  RootRecordData record_data;
  record_data.root_type = sub_tag_value;

  if (!byte_iterator_->ReadId(record_data.object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(record_data.thread_id) ||
      !byte_iterator_->ReadU4(record_data.frame_number)) {
    PERFETTO_FATAL("Failed to read thread-related root");
  }

  PERFETTO_DLOG(
      "Thread-related root: objectID=%" PRIu64 ", threadID=%u, frameNumber=%u",
      record_data.object_id, record_data.thread_id, record_data.frame_number);

  HprofHeapRecord record;
  record.tag = static_cast<HprofHeapTag>(record_data.root_type);
  record.data = record_data;

  // Add to current heap dump
  if (!data.records.empty() &&
      (std::holds_alternative<HeapDumpData>(data.records.back().data))) {
    auto& heap_dump = std::get<HeapDumpData>(data.records.back().data);
    heap_dump.records.push_back(record);
  }

  // Store root information in hprof
  data.root_objects[record_data.object_id] = record_data.root_type;
  data.root_count++;
}

void HprofParser::ParseSimpleRoot(HprofData& data, uint8_t sub_tag_value) {
  PERFETTO_DLOG("Parsing simple root");

  RootRecordData record_data;
  record_data.root_type = sub_tag_value;

  if (!byte_iterator_->ReadId(record_data.object_id, identifier_size_)) {
    PERFETTO_FATAL("Failed to read simple root");
  }

  PERFETTO_DLOG("Simple root: objectID=%" PRIu64, record_data.object_id);

  HprofHeapRecord record;
  record.tag = static_cast<HprofHeapTag>(record_data.root_type);
  record.data = record_data;

  // Add to current heap dump
  if (!data.records.empty() &&
      (std::holds_alternative<HeapDumpData>(data.records.back().data))) {
    auto& heap_dump = std::get<HeapDumpData>(data.records.back().data);
    heap_dump.records.push_back(record);
  }

  // Store root information in hprof
  data.root_objects[record_data.object_id] = record_data.root_type;
  data.root_count++;
}

void HprofParser::ParseThreadObjectRoot(HprofData& data) {
  PERFETTO_DLOG("Parsing thread object root");

  RootRecordData record_data;
  record_data.root_type = HPROF_ROOT_THREAD_OBJ;

  if (!byte_iterator_->ReadId(record_data.object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(record_data.thread_id) ||
      !byte_iterator_->ReadU4(record_data.frame_number)) {
    PERFETTO_FATAL("Failed to read thread object root");
  }

  PERFETTO_DLOG("Thread object root: objectID=%" PRIu64
                ", threadID=%u, stackTraceSerial=%u",
                record_data.object_id, record_data.thread_id,
                record_data.frame_number);

  HprofHeapRecord record;
  record.tag = HPROF_ROOT_THREAD_OBJ;
  record.data = record_data;

  // Add to current heap dump
  if (!data.records.empty() &&
      (std::holds_alternative<HeapDumpData>(data.records.back().data))) {
    auto& heap_dump = std::get<HeapDumpData>(data.records.back().data);
    heap_dump.records.push_back(record);
  }

  // Store root information in hprof
  data.root_objects[record_data.object_id] = HPROF_ROOT_THREAD_OBJ;
  data.root_count++;
}

void HprofParser::ParseHeapDumpInfo(HprofData& data) {
  PERFETTO_DLOG("Parsing heap dump info");

  HeapDumpInfoData heap_data;

  if (!byte_iterator_->ReadU4(heap_data.heap_id) ||
      !byte_iterator_->ReadId(heap_data.heap_name_string_id,
                              identifier_size_)) {
    PERFETTO_FATAL("Failed to read heap dump info");
  }

  // Look up the heap name string by its ID
  auto name_it = data.id_to_string_map.find(heap_data.heap_name_string_id);
  if (name_it != data.id_to_string_map.end()) {
    heap_data.heap_name = name_it->second;
  } else {
    PERFETTO_DLOG("WARNING: Heap name string ID %" PRIu64
                  " not found in string map",
                  heap_data.heap_name_string_id);
    // Fallback name based on heap ID
    heap_data.heap_name = "unknown-heap-" + std::to_string(heap_data.heap_id);
  }

  PERFETTO_DLOG("Heap dump info: heapID=%u, heapName='%s'", heap_data.heap_id,
                heap_data.heap_name.c_str());

  // Set current heap for subsequent objects
  current_heap_ = static_cast<HprofHeapId>(heap_data.heap_id);

  HprofHeapRecord record;
  record.tag = HPROF_HEAP_DUMP_INFO;
  record.data = heap_data;

  // Add to current heap dump
  if (!data.records.empty() &&
      (std::holds_alternative<HeapDumpData>(data.records.back().data))) {
    auto& heap_dump = std::get<HeapDumpData>(data.records.back().data);
    heap_dump.records.push_back(record);
  }

  data.heap_info_count++;
}

// Main method, now refactored
void HprofParser::ParseClassDump(HprofData& data) {
  PERFETTO_DLOG("Starting to parse class dump");

  ClassDumpData class_data;

  // Read the header fields
  uint64_t reserved1, reserved2;  // Temporary variables for reserved fields
  if (!byte_iterator_->ReadId(class_data.class_object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(class_data.stack_trace_serial_num) ||
      !byte_iterator_->ReadId(class_data.super_class_object_id,
                              identifier_size_) ||
      !byte_iterator_->ReadId(class_data.class_loader_object_id,
                              identifier_size_) ||
      !byte_iterator_->ReadId(class_data.signers_object_id, identifier_size_) ||
      !byte_iterator_->ReadId(class_data.protection_domain_object_id,
                              identifier_size_) ||
      !byte_iterator_->ReadId(reserved1, identifier_size_) ||
      !byte_iterator_->ReadId(reserved2, identifier_size_) ||
      !byte_iterator_->ReadU4(class_data.instance_size)) {
    PERFETTO_FATAL("Failed to read class dump header");
  }

  PERFETTO_DLOG("Class dump: classID=%" PRIu64 ", superClassID=%" PRIu64,
                class_data.class_object_id, class_data.super_class_object_id);

  // Update class info in global map
  auto& class_info = data.classes[class_data.class_object_id];
  class_info.super_class_id = class_data.super_class_object_id;
  class_info.instance_size = class_data.instance_size;

  // Check if this is a String class
  if (IsStringClass(class_info.name)) {
    class_info.is_string_class = true;
    class_data.is_string_class = true;
  }

  // Read constant pool
  uint16_t constant_pool_size;
  if (!byte_iterator_->ReadU2(constant_pool_size)) {
    PERFETTO_FATAL("Failed to read constant pool size");
  }

  PERFETTO_DLOG("Constant pool size: %u", constant_pool_size);

  for (uint16_t i = 0; i < constant_pool_size; i++) {
    uint16_t index;
    uint8_t type;
    if (!byte_iterator_->ReadU2(index) || !byte_iterator_->ReadU1(type)) {
      PERFETTO_FATAL("Failed to read constant pool entry");
    }

    // Skip the value based on type
    if (!byte_iterator_->SkipBytes(GetFieldTypeSize(type))) {
      PERFETTO_FATAL("Failed to skip constant pool value");
    }
  }

  // Read static fields
  uint16_t static_field_count;
  if (!byte_iterator_->ReadU2(static_field_count)) {
    PERFETTO_FATAL("Failed to read static field count");
  }

  PERFETTO_DLOG("Static field count: %u", static_field_count);
  class_data.static_fields.reserve(static_field_count);

  for (uint16_t i = 0; i < static_field_count; i++) {
    uint64_t name_string_id;
    uint8_t type;
    if (!byte_iterator_->ReadId(name_string_id, identifier_size_) ||
        !byte_iterator_->ReadU1(type)) {
      PERFETTO_FATAL("Failed to read static field entry");
    }

    FieldInfo field;
    field.type = type;

    auto name_it = data.id_to_string_map.find(name_string_id);
    if (name_it != data.id_to_string_map.end()) {
      field.name = name_it->second;
    } else {
      PERFETTO_DLOG("Static field name not found for ID %" PRIu64,
                    name_string_id);
    }

    class_data.static_fields.push_back(field);

    // Skip the value
    if (!byte_iterator_->SkipBytes(GetFieldTypeSize(type))) {
      PERFETTO_FATAL("Failed to skip static field value");
    }
  }

  // Read instance fields
  uint16_t instance_field_count;
  if (!byte_iterator_->ReadU2(instance_field_count)) {
    PERFETTO_FATAL("Failed to read instance field count");
  }

  PERFETTO_DLOG("Instance field count: %u", instance_field_count);
  class_data.instance_fields.reserve(instance_field_count);
  class_info.fields.clear();
  class_info.fields.reserve(instance_field_count);

  for (uint16_t i = 0; i < instance_field_count; i++) {
    uint64_t name_string_id;
    uint8_t type;
    if (!byte_iterator_->ReadId(name_string_id, identifier_size_) ||
        !byte_iterator_->ReadU1(type)) {
      PERFETTO_FATAL("Failed to read instance field entry");
    }

    FieldInfo field;
    field.type = type;

    auto name_it = data.id_to_string_map.find(name_string_id);
    if (name_it != data.id_to_string_map.end()) {
      field.name = name_it->second;
    } else {
      PERFETTO_DLOG("Instance field name not found for ID %" PRIu64,
                    name_string_id);
    }

    class_data.instance_fields.push_back(field);
    class_info.fields.push_back(field);

    if (type == TYPE_OBJECT) {
      data.field_reference_count++;
    }
  }

  // Create heap record
  HprofHeapRecord record;
  record.tag = HPROF_CLASS_DUMP;
  record.data = class_data;

  // Add to current heap dump
  if (!data.records.empty() &&
      (std::holds_alternative<HeapDumpData>(data.records.back().data))) {
    auto& heap_dump = std::get<HeapDumpData>(data.records.back().data);
    heap_dump.records.push_back(record);
  }
}

void HprofParser::ParseInstanceDump(HprofData& data) {
  PERFETTO_DLOG("Parsing instance dump");

  InstanceDumpData instance_data;
  uint32_t data_length;

  if (!byte_iterator_->ReadId(instance_data.object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(instance_data.stack_trace_serial_num) ||
      !byte_iterator_->ReadId(instance_data.class_object_id,
                              identifier_size_) ||
      !byte_iterator_->ReadU4(data_length)) {
    PERFETTO_FATAL("Failed to read instance dump header");
  }

  PERFETTO_DLOG(
      "Instance dump: objectID=%" PRIu64 ", classID=%" PRIu64 ", dataLength=%u",
      instance_data.object_id, instance_data.class_object_id, data_length);

  // Set heap ID (from current heap context)
  instance_data.heap_id = current_heap_;

  // Store object to class mapping
  data.object_to_class[instance_data.object_id] = instance_data.class_object_id;

  // Read instance data
  if (!byte_iterator_->ReadBytes(instance_data.raw_instance_data,
                                 data_length)) {
    PERFETTO_FATAL("Failed to read instance data");
  }

  PERFETTO_DLOG("Read %u bytes of instance data", data_length);

  // Process fields if we have class info
  auto class_it = data.classes.find(instance_data.class_object_id);
  if (class_it != data.classes.end()) {
    const ClassInfo& class_info = class_it->second;
    bool is_string_instance = class_info.is_string_class;

    PERFETTO_DLOG("Processing fields for class: %s%s", class_info.name.c_str(),
                  is_string_instance ? " (String class)" : "");

    // Extract and process all instance fields
    ExtractInstanceFields(instance_data, class_info);

    // After ExtractInstanceFields, try to extract string value if this is a
    // String class
    if (is_string_instance) {
      ExtractStringInstance(instance_data, class_info);
    }
  } else {
    PERFETTO_DLOG("Warning: Class info not found for class ID: %" PRIu64,
                  instance_data.class_object_id);
  }

  HprofHeapRecord record;
  record.tag = HPROF_INSTANCE_DUMP;
  record.data = instance_data;

  // Add to current heap dump
  if (!data.records.empty() &&
      (std::holds_alternative<HeapDumpData>(data.records.back().data))) {
    auto& heap_dump = std::get<HeapDumpData>(data.records.back().data);
    heap_dump.records.push_back(record);
  }

  data.class_instance_count++;
}

void HprofParser::ParseObjectArrayDump(HprofData& data) {
  PERFETTO_DLOG("Parsing object array dump");

  ObjArrayDumpData array_data;
  uint32_t size;

  if (!byte_iterator_->ReadId(array_data.array_object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(array_data.stack_trace_serial_num) ||
      !byte_iterator_->ReadU4(size) ||
      !byte_iterator_->ReadId(array_data.array_class_object_id,
                              identifier_size_)) {
    PERFETTO_FATAL("Failed to read object array dump header");
  }

  PERFETTO_DLOG(
      "Object array: objectID=%" PRIu64 ", classID=%" PRIu64 ", size=%u",
      array_data.array_object_id, array_data.array_class_object_id, size);

  // Set heap ID (from current heap context)
  array_data.heap_id = current_heap_;

  // Read elements
  array_data.elements.reserve(size);
  for (uint32_t i = 0; i < size; i++) {
    uint64_t element_id;
    if (!byte_iterator_->ReadId(element_id, identifier_size_)) {
      PERFETTO_FATAL("Failed to read array element");
    }

    array_data.elements.push_back(element_id);

    // Store reference from array to element (for non-null elements)
    if (element_id != 0) {
      ObjectReference ref;
      // For arrays, use array index format
      ref.field_name = "[" + std::to_string(i) + "]";
      ref.target_object_id = element_id;

      // Add to global map
      data.owner_to_owned[array_data.array_object_id].push_back(ref);
    }
  }

  HprofHeapRecord record;
  record.tag = HPROF_OBJ_ARRAY_DUMP;
  record.data = array_data;

  // Add to current heap dump
  if (!data.records.empty() &&
      (std::holds_alternative<HeapDumpData>(data.records.back().data))) {
    auto& heap_dump = std::get<HeapDumpData>(data.records.back().data);
    heap_dump.records.push_back(record);
  }

  data.object_array_count++;
}

void HprofParser::ParsePrimitiveArrayDump(HprofData& data) {
  PERFETTO_DLOG("Parsing primitive array dump");

  PrimArrayDumpData array_data;
  uint32_t size;

  if (!byte_iterator_->ReadId(array_data.array_object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(array_data.stack_trace_serial_num) ||
      !byte_iterator_->ReadU4(size) ||
      !byte_iterator_->ReadU1(array_data.element_type)) {
    PERFETTO_FATAL("Failed to read primitive array dump header");
  }

  PERFETTO_DLOG("Primitive array: objectID=%" PRIu64 ", type=%d, size=%u",
                array_data.array_object_id,
                static_cast<int>(array_data.element_type), size);

  // Set heap ID (from current heap context)
  array_data.heap_id = current_heap_;

  // Determine element size and read data
  size_t element_size = GetFieldTypeSize(array_data.element_type);
  size_t bytes_to_read = size * element_size;

  PERFETTO_DLOG("Reading %zu bytes of array data", bytes_to_read);

  if (!byte_iterator_->ReadBytes(array_data.elements, bytes_to_read)) {
    PERFETTO_FATAL("Failed to read primitive array data");
  }

  HprofHeapRecord record;
  record.tag = HPROF_PRIM_ARRAY_DUMP;
  record.data = array_data;

  // Add to current heap dump
  if (!data.records.empty() &&
      (std::holds_alternative<HeapDumpData>(data.records.back().data))) {
    auto& heap_dump = std::get<HeapDumpData>(data.records.back().data);
    heap_dump.records.push_back(record);
  }

  data.primitive_array_count++;
}

std::string HprofParser::NormalizeClassName(std::string name) {
  std::string original_name_for_error_reporting =
      name;  // Keep original for error messages
  int num_dimensions = 0;

  // Count array dimensions
  while (!name.empty() && name[0] == '[') {
    num_dimensions++;
    name = name.substr(1);
  }

  if (num_dimensions > 0) {
    if (name.empty()) {
      PERFETTO_FATAL(
          "Invalid array type signature in class name: became empty after "
          "stripping '['. Original: '%s'",
          original_name_for_error_reporting.c_str());
    }

    // Convert primitive type signature to name
    switch (name[0]) {
      case 'Z':
        name = "boolean";
        break;
      case 'B':
        name = "byte";
        break;
      case 'C':
        name = "char";
        break;
      case 'S':
        name = "short";
        break;
      case 'I':
        name = "int";
        break;
      case 'J':
        name = "long";
        break;
      case 'F':
        name = "float";
        break;
      case 'D':
        name = "double";
        break;
      case 'L':
        if (name.length() < 2 || name.back() != ';') {
          PERFETTO_FATAL(
              "Invalid L type signature in class name: '%s'. Original: '%s'",
              name.c_str(), original_name_for_error_reporting.c_str());
        }
        // Remove the 'L' prefix and the ';' suffix
        name = name.substr(1, name.length() - 2);
        break;
      default:
        PERFETTO_FATAL(
            "Invalid array component type signature in class name: '%s'. "
            "Original: '%s'",
            name.c_str(), original_name_for_error_reporting.c_str());
    }
  }

  // Replace all occurrences of '/' with '.'
  std::replace(name.begin(), name.end(), '/', '.');

  // Append array brackets at the end, matching Java's format
  for (int i = 0; i < num_dimensions; ++i) {
    name += "[]";
  }

  return name;
}

// All the remaining helper methods:
bool HprofParser::IsStringClass(
    const std::string& normalized_class_name) const {
  // Assumes normalized_class_name has already been processed by
  // NormalizeClassName
  return normalized_class_name == "java.lang.String";
}

size_t HprofParser::GetFieldTypeSize(uint8_t type) const {
  switch (type) {
    case TYPE_BOOLEAN:
    case TYPE_BYTE:
      return 1;
    case TYPE_CHAR:
    case TYPE_SHORT:
      return 2;
    case TYPE_FLOAT:
    case TYPE_INT:
      return 4;
    case TYPE_DOUBLE:
    case TYPE_LONG:
      return 8;
    case TYPE_OBJECT:
      return identifier_size_;
    default:
      return 0;
  }
}

int8_t HprofParser::ReadByteValue(const std::vector<uint8_t>& data,
                                  size_t offset) const {
  if (offset < data.size()) {
    return static_cast<int8_t>(data[offset]);
  }
  return 0;
}

bool HprofParser::ReadBooleanValue(const std::vector<uint8_t>& data,
                                   size_t offset) const {
  if (offset < data.size()) {
    return data[offset] != 0;
  }
  return false;
}

int16_t HprofParser::ReadShortValue(const std::vector<uint8_t>& data,
                                    size_t offset) const {
  if (offset + 1 < data.size()) {
    // Assuming big-endian byte order for HPROF files
    return static_cast<int16_t>((static_cast<int16_t>(data[offset]) << 8) |
                                static_cast<int16_t>(data[offset + 1]));
  }
  return 0;
}

char16_t HprofParser::ReadCharValue(const std::vector<uint8_t>& data,
                                    size_t offset) const {
  if (offset + 1 < data.size()) {
    // Assuming big-endian byte order for HPROF files
    return static_cast<char16_t>((static_cast<char16_t>(data[offset]) << 8) |
                                 static_cast<char16_t>(data[offset + 1]));
  }
  return 0;
}

int32_t HprofParser::ReadIntValue(const std::vector<uint8_t>& data,
                                  size_t offset) const {
  if (offset + 3 < data.size()) {
    // Assuming big-endian byte order for HPROF files
    return (static_cast<int32_t>(data[offset]) << 24) |
           (static_cast<int32_t>(data[offset + 1]) << 16) |
           (static_cast<int32_t>(data[offset + 2]) << 8) | data[offset + 3];
  }
  return 0;
}

float HprofParser::ReadFloatValue(const std::vector<uint8_t>& data,
                                  size_t offset) const {
  if (offset + 3 < data.size()) {
    int32_t int_value = ReadIntValue(data, offset);
    float result;
    std::memcpy(&result, &int_value, sizeof(float));
    return result;
  }
  return 0.0f;
}

int64_t HprofParser::ReadLongValue(const std::vector<uint8_t>& data,
                                   size_t offset) const {
  if (offset + 7 < data.size()) {
    // Assuming big-endian byte order for HPROF files
    return (static_cast<int64_t>(data[offset]) << 56) |
           (static_cast<int64_t>(data[offset + 1]) << 48) |
           (static_cast<int64_t>(data[offset + 2]) << 40) |
           (static_cast<int64_t>(data[offset + 3]) << 32) |
           (static_cast<int64_t>(data[offset + 4]) << 24) |
           (static_cast<int64_t>(data[offset + 5]) << 16) |
           (static_cast<int64_t>(data[offset + 6]) << 8) | data[offset + 7];
  }
  return 0;
}

double HprofParser::ReadDoubleValue(const std::vector<uint8_t>& data,
                                    size_t offset) const {
  if (offset + 7 < data.size()) {
    int64_t long_value = ReadLongValue(data, offset);
    double result;
    std::memcpy(&result, &long_value, sizeof(double));
    return result;
  }
  return 0.0;
}

uint64_t HprofParser::ReadObjectIDValue(const std::vector<uint8_t>& data,
                                        size_t offset,
                                        uint32_t id_size) const {
  if (id_size == 4 && offset + 3 < data.size()) {
    return static_cast<uint64_t>(ReadIntValue(data, offset));
  } else if (id_size == 8 && offset + 7 < data.size()) {
    return static_cast<uint64_t>(ReadLongValue(data, offset));
  }
  return 0;
}

void HprofParser::ExtractInstanceFields(InstanceDumpData& instance_data,
                                        const ClassInfo& class_info) {
  size_t offset = 0;
  size_t field_index = 0;

  // Get all fields from class hierarchy
  std::vector<FieldInfo> all_instance_fields =
      GetFieldsForClassHierarchy(instance_data.class_object_id);

  // Determine if class is an array type
  bool is_array_type = false;
  uint64_t current_class_id = instance_data.class_object_id;
  std::unordered_set<uint64_t> visited_classes;  // To detect cycles

  while (current_class_id != 0) {
    // Prevent infinite loop due to cycles
    if (visited_classes.find(current_class_id) != visited_classes.end()) {
      PERFETTO_ELOG(
          "Cycle detected while checking for array type for object %" PRIu64,
          instance_data.object_id);
      break;
    }
    visited_classes.insert(current_class_id);

    auto class_it = data_.classes.find(current_class_id);
    if (class_it != data_.classes.end()) {
      if (class_it->second.name.find("[]") != std::string::npos) {
        is_array_type = true;
        break;
      }
      current_class_id = class_it->second.super_class_id;
    } else {
      break;
    }
  }

  // Process all fields
  for (const auto& field_info : all_instance_fields) {
    // Check if we've reached the end of data
    if (offset >= instance_data.raw_instance_data.size()) {
      PERFETTO_DLOG(
          "Warning: Reached end of instance data (size %zu, offset %zu) "
          "while processing field '%s' (index %zu) for class %s (instance "
          "%" PRIu64 ")",
          instance_data.raw_instance_data.size(), offset,
          field_info.name.c_str(), field_index, class_info.name.c_str(),
          instance_data.object_id);
      break;
    }

    // Extract field value
    FieldValue value = ExtractFieldValue(instance_data.raw_instance_data,
                                         offset, field_info.type);

    // Create field value record
    FieldValueRecord record;
    record.field_name = field_info.name;
    record.value = value;
    instance_data.field_values.push_back(record);

    // Handle reference fields
    if (field_info.type == TYPE_OBJECT &&
        value.type == FieldValue::ValueType::OBJECT_ID &&
        std::get<uint64_t>(value.value) != 0) {
      ObjectReference ref;

      if (field_info.name.empty()) {
        PERFETTO_FATAL("Field info is empty");
      }

      std::string field_name = field_info.name;

      // Convert array-style naming if owner isn't an array
      if (!is_array_type && field_name.size() >= 2 && field_name[0] == '[' &&
          field_name[field_name.size() - 1] == ']') {
        PERFETTO_DLOG(
            "Converting array-style field name '%s' to regular field "
            "for non-array object %" PRIu64 " (class %s)",
            field_name.c_str(), instance_data.object_id,
            class_info.name.c_str());

        field_name = "field_" + field_name.substr(1, field_name.size() - 2);
      }

      ref.field_name = field_name;
      ref.target_object_id = std::get<uint64_t>(value.value);

      // Add reference to both instance and global map
      instance_data.references.push_back(ref);
      data_.owner_to_owned[instance_data.object_id].push_back(ref);
    }

    // Move to next field
    offset += GetFieldTypeSize(field_info.type);
    field_index++;
  }

  // Validate final offset matches data size
  if (offset != instance_data.raw_instance_data.size() &&
      !all_instance_fields.empty()) {
    PERFETTO_DLOG(
        "Warning: Mismatch after parsing instance fields for class %s "
        "(instance %" PRIu64
        "). "
        "Expected size %zu, parsed %zu bytes. Fields processed: %zu.",
        class_info.name.c_str(), instance_data.object_id,
        instance_data.raw_instance_data.size(), offset, field_index);
  }
}

void HprofParser::ExtractStringInstance(InstanceDumpData& instance_data,
                                        const ClassInfo& class_info) {
  if (!class_info.is_string_class) {
    return;
  }
  PERFETTO_DLOG("Extracting string value from String instance %" PRIu64,
                instance_data.object_id);
  // Find the "value" field which contains the char array reference
  uint64_t char_array_id = 0;
  for (const auto& field_value : instance_data.field_values) {
    if ((field_value.field_name == "value" ||
         field_value.field_name == "chars") &&
        field_value.value.type == FieldValue::ValueType::OBJECT_ID) {
      char_array_id = std::get<uint64_t>(field_value.value.value);
      break;
    }
  }
  if (char_array_id == 0) {
    PERFETTO_DLOG("String value field not found or null for object %" PRIu64,
                  instance_data.object_id);
    return;
  }
  // Add special reference for string value array
  ObjectReference ref;
  ref.field_name = "stringValue";
  ref.target_object_id = char_array_id;
  instance_data.references.push_back(ref);
  data_.owner_to_owned[instance_data.object_id].push_back(ref);
}

FieldValue HprofParser::ExtractFieldValue(const std::vector<uint8_t>& data,
                                          size_t offset,
                                          uint8_t field_type) {
  PERFETTO_DLOG("Extracting field of type %d at offset %zu",
                static_cast<int>(field_type), offset);

  switch (field_type) {
    case TYPE_BOOLEAN:
      return FieldValue(ReadBooleanValue(data, offset));
    case TYPE_BYTE:
      return FieldValue(ReadByteValue(data, offset));
    case TYPE_CHAR:
      return FieldValue(ReadCharValue(data, offset));
    case TYPE_SHORT:
      return FieldValue(ReadShortValue(data, offset));
    case TYPE_INT:
      return FieldValue(ReadIntValue(data, offset));
    case TYPE_FLOAT:
      return FieldValue(ReadFloatValue(data, offset));
    case TYPE_LONG:
      return FieldValue(ReadLongValue(data, offset));
    case TYPE_DOUBLE:
      return FieldValue(ReadDoubleValue(data, offset));
    case TYPE_OBJECT:
      return FieldValue(ReadObjectIDValue(data, offset, identifier_size_));
    default:
      PERFETTO_ELOG("Unknown field type: %d", static_cast<int>(field_type));
      return FieldValue();
  }
}

std::vector<FieldInfo> HprofParser::GetFieldsForClassHierarchy(
    uint64_t class_object_id) {
  std::vector<FieldInfo> all_fields;
  std::vector<uint64_t> class_ids_in_hierarchy;

  // Traverse class hierarchy, stopping on cycles or null
  uint64_t current_cid = class_object_id;
  while (current_cid != 0) {
    auto it = data_.classes.find(current_cid);
    if (it == data_.classes.end()) {
      PERFETTO_ELOG("Class ID %" PRIu64
                    " not found in hprof while building hierarchy for %" PRIu64,
                    current_cid, class_object_id);
      break;
    }

    // Check for cycles in hierarchy
    if (std::find(class_ids_in_hierarchy.begin(), class_ids_in_hierarchy.end(),
                  current_cid) != class_ids_in_hierarchy.end()) {
      PERFETTO_ELOG("Cycle detected in class hierarchy for class ID %" PRIu64,
                    current_cid);
      break;
    }

    class_ids_in_hierarchy.push_back(current_cid);

    // Check for self-reference
    if (it->second.super_class_id == current_cid) {
      PERFETTO_ELOG("Class ID %" PRIu64 " has self as superclass", current_cid);
      break;
    }

    current_cid = it->second.super_class_id;
  }

  // Reverse to get superclass-first order (fields are defined top-down)
  std::reverse(class_ids_in_hierarchy.begin(), class_ids_in_hierarchy.end());

  // Collect fields from entire hierarchy
  for (uint64_t cid_in_order : class_ids_in_hierarchy) {
    auto it = data_.classes.find(cid_in_order);
    if (it != data_.classes.end()) {
      all_fields.insert(all_fields.end(), it->second.fields.begin(),
                        it->second.fields.end());
    }
  }

  return all_fields;
}

}  // namespace perfetto::trace_processor::art_hprof
