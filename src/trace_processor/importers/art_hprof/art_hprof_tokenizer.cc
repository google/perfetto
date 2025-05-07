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

constexpr uint8_t HPROF_ROOT_RECORD_MIN = 0x01;
constexpr uint8_t HPROF_ROOT_RECORD_MAX = 0x0A;

// HprofParser implementation
HprofParser::~HprofParser() = default;

// Define the virtual destructor out-of-line to fix weak vtable warning
ByteIterator::~ByteIterator() = default;

HprofData HprofParser::Parse() {
  PERFETTO_DLOG("Beginning to parse HPROF");

  data_ = HprofData{};  // Reset the member variable

  ParseHeader();

  while (HasMoreData()) {
    RecordHeader header = ReadRecordHeader();
    ParseRecordByType(header);
  }

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

void HprofParser::ParseHeader() {
  PERFETTO_DLOG("Parsing HPROF header");

  char c;
  data_.header.format = "";
  while (byte_iterator_->ReadU1(reinterpret_cast<uint8_t&>(c)) && c != 0) {
    data_.header.format.push_back(c);
  }

  if (!byte_iterator_->ReadU4(data_.header.identifier_size)) {
    PERFETTO_FATAL("Error: Failed to read ID size");
  }

  identifier_size_ = data_.header.identifier_size;

  uint32_t high_time, low_time;
  if (!byte_iterator_->ReadU4(high_time) || !byte_iterator_->ReadU4(low_time)) {
    PERFETTO_FATAL("Error: Failed to read timestamp");
  }

  data_.header.timestamp = (static_cast<uint64_t>(high_time) << 32) | low_time;

  PERFETTO_DLOG("HPROF header: format=%s, idSize=%u",
                data_.header.format.c_str(), identifier_size_);
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

  if (!byte_iterator_->ReadU1(header.tag)) {
    PERFETTO_FATAL("Failed to read record tag");
  }

  if (!byte_iterator_->ReadU4(header.time) ||
      !byte_iterator_->ReadU4(header.length)) {
    PERFETTO_FATAL("Failed to read record time/length");
  }

  PERFETTO_DLOG("Reading record with tag: 0x%x, time: %u, length: %u",
                static_cast<int>(header.tag), header.time, header.length);

  return header;
}

void HprofParser::SkipRecord(uint32_t length) {
  if (!byte_iterator_->SkipBytes(length)) {
    PERFETTO_FATAL("Failed to skip record payload");
  }
}

template <typename T>
void HprofParser::AddMainRecord(const RecordHeader& header, T&& record_data) {
  HprofRecord record;
  record.tag = header.tag;
  record.time = header.time;
  record.length = header.length;
  record.data = std::forward<T>(record_data);
  data_.records.push_back(record);
}

template <typename T>
void HprofParser::AddHeapRecord(HprofHeapTag tag, T&& record_data) {
  HprofHeapRecord heap_record;
  heap_record.tag = tag;
  heap_record.data = std::forward<T>(record_data);

  if (!data_.records.empty() &&
      std::holds_alternative<HeapDumpData>(data_.records.back().data)) {
    std::get<HeapDumpData>(data_.records.back().data)
        .records.push_back(heap_record);
  } else {
    // This should not happen if the parsing logic is correct, as heap records
    // should only appear within a HEAP_DUMP or HEAP_DUMP_SEGMENT.
    PERFETTO_FATAL("Heap record encountered outside of a heap dump segment");
  }
}

std::string HprofParser::GetString(uint64_t id) const {
  auto it = data_.id_to_string_map.find(id);
  if (it != data_.id_to_string_map.end()) {
    return it->second;
  }
  PERFETTO_DLOG("Warning: String ID %" PRIu64 " not found", id);
  return "???";
}

void HprofParser::ParseRecordByType(const RecordHeader& header) {
  uint32_t count = 0;  // Used in HPROF_TRACE case
  switch (header.tag) {
    case HPROF_UTF8:
      ParseUtf8Record(header);
      break;
    case HPROF_LOAD_CLASS:
      ParseLoadClassRecord(header);
      break;
    case HPROF_HEAP_DUMP:
    case HPROF_HEAP_DUMP_SEGMENT:
      ParseHeapDumpRecord(header);
      break;
    case HPROF_HEAP_DUMP_END:
      AddMainRecord(header, std::monostate{});
      break;
    case HPROF_FRAME:
      SkipRecord(identifier_size_);  // stack frame id
      SkipRecord(identifier_size_);  // method frame id
      SkipRecord(identifier_size_);  // source file string id
      SkipRecord(identifier_size_);  // source file string id
      SkipRecord(4);                 // class serial number
      SkipRecord(4);                 // line number
      break;
    case HPROF_TRACE:
      SkipRecord(4);                         // trace serial number
      SkipRecord(4);                         // thread serial number
      byte_iterator_->ReadU4(count);         // number of frames
      SkipRecord(identifier_size_ * count);  // frame ids
      break;
    default:
      PERFETTO_LOG("Skipping unknown record payload of length %u and tag %u",
                   header.length, header.tag);
      SkipRecord(header.length);
      AddMainRecord(header, std::monostate{});
      break;
  }
}

void HprofParser::ParseUtf8Record(const RecordHeader& header) {
  PERFETTO_DLOG("Parsing UTF8 record");

  Utf8StringData utf8_data;

  if (!byte_iterator_->ReadId(utf8_data.name_id, identifier_size_)) {
    PERFETTO_FATAL("Failed to read string ID");
  }

  size_t string_length = header.length - identifier_size_;
  if (!byte_iterator_->ReadString(utf8_data.utf8_string, string_length)) {
    PERFETTO_FATAL("Failed to read string data");
  }

  // Always store 0 as "???" like in the Java implementation
  if (utf8_data.name_id == 0) {
    utf8_data.utf8_string = "???";
  }

  PERFETTO_DLOG("Read UTF8 string: ID=%" PRIu64 ", string='%s'",
                utf8_data.name_id, utf8_data.utf8_string.c_str());

  AddMainRecord(header, utf8_data);

  data_.id_to_string_map[utf8_data.name_id] = utf8_data.utf8_string;
  data_.string_count++;
}

void HprofParser::ParseLoadClassRecord(const RecordHeader& header) {
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

  class_data_record.class_name = GetString(class_data_record.class_name_id);

  PERFETTO_DLOG(
      "Class loaded: serial=%u, obj_id=%" PRIu64 ", name_id=%" PRIu64
      ", name='%s'",
      class_data_record.class_serial_num, class_data_record.class_object_id,
      class_data_record.class_name_id, class_data_record.class_name.c_str());

  // Update the class info with the name
  ClassInfo& class_info = data_.classes[class_data_record.class_object_id];
  class_info.name = class_data_record.class_name;
  class_info.class_object_id = class_data_record.class_object_id;

  class_info.is_string_class = IsStringClass(class_info.name);

  if (class_info.name == "java.lang.Class") {
    data_.java_lang_class_object_id = class_data_record.class_object_id;
  }

  AddMainRecord(header, class_data_record);
  data_.class_count++;
}

void HprofParser::ParseHeapDumpRecord(const RecordHeader& header) {
  PERFETTO_DLOG("Parsing HEAP_DUMP or HEAP_DUMP_SEGMENT record");

  // Create the HeapDumpData record immediately
  HeapDumpData heap_data;
  AddMainRecord(header, heap_data);

  size_t end_pos = GetPosition() + header.length;

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

    switch (sub_tag) {
      case HPROF_ROOT_JNI_GLOBAL:
        ParseRootJniGlobal();
        break;
      case HPROF_ROOT_JNI_LOCAL:
      case HPROF_ROOT_JAVA_FRAME:
      case HPROF_ROOT_THREAD_BLOCK:  // HPROF_ROOT_THREAD_BLOCK also includes
                                     // thread/frame info
        ParseThreadObjectRoot(sub_tag);
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
        ParseSimpleRoot(sub_tag);
        break;
      case HPROF_ROOT_THREAD_OBJ:
        // This specific tag has object ID, thread ID, AND frame number
        ParseThreadObjectRoot(sub_tag);
        break;
      case HPROF_HEAP_DUMP_INFO:
        ParseHeapDumpInfo();
        break;
      case HPROF_CLASS_DUMP:
        ParseClassDump();
        break;
      case HPROF_INSTANCE_DUMP:
        ParseInstanceDump();
        break;
      case HPROF_OBJ_ARRAY_DUMP:
        ParseObjectArrayDump();
        break;
      case HPROF_PRIM_ARRAY_DUMP:
        ParsePrimitiveArrayDump();
        break;
      default:
        PERFETTO_LOG("Unknown sub-record tag: 0x%x", static_cast<int>(sub_tag));
        SkipUnknownSubRecord(sub_tag);
        break;
    }

    if (GetPosition() > end_pos && !IsEof()) {
      PERFETTO_FATAL("Parsed past expected end of heap dump segment");
    }

    if (GetPosition() == end_pos && !IsEof() &&
        sub_tag != HPROF_HEAP_DUMP_END && header.tag != HPROF_HEAP_DUMP) {
      // Android HPROF sometimes puts a HEAP_DUMP_END *after* the expected
      // end_pos if this is a segment. We check here to avoid errors if we are
      // exactly at the end and the next tag *might* be HEAP_DUMP_END. This is a
      // heuristic based on observed behavior, actual spec compliance might
      // require a peek function or different loop condition. For simplicity
      // here, we just check if we are *past* the end.
    }

    // If we've reached or passed the expected end of the current heap dump
    // segment, break.
    if (GetPosition() >= end_pos || IsEof()) {
      break;
    }
  }

  data_.heap_dump_count++;
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

void HprofParser::ParseRootJniGlobal() {
  PERFETTO_DLOG("Parsing JNI GLOBAL root");

  RootRecordData record_data;
  record_data.root_type = HPROF_ROOT_JNI_GLOBAL;

  uint64_t global_ref_id;  // Temporary variable for the second ID
  if (!byte_iterator_->ReadId(record_data.object_id, identifier_size_) ||
      !byte_iterator_->ReadId(global_ref_id, identifier_size_)) {
    PERFETTO_FATAL("Failed to read JNI GLOBAL root");
  }

  AddHeapRecord(HPROF_ROOT_JNI_GLOBAL, record_data);

  data_.root_objects[record_data.object_id] = HPROF_ROOT_JNI_GLOBAL;
  data_.root_count++;
}

void HprofParser::ParseThreadObjectRoot(uint8_t sub_tag_value) {
  PERFETTO_DLOG("Parsing thread object root with tag 0x%x", sub_tag_value);

  RootRecordData record_data;
  record_data.root_type = sub_tag_value;

  if (!byte_iterator_->ReadId(record_data.object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(record_data.thread_id)) {
    PERFETTO_FATAL(
        "Failed to read thread object root header (object/thread id)");
  }

  // HPROF_ROOT_THREAD_OBJ, HPROF_ROOT_JNI_LOCAL, HPROF_ROOT_JAVA_FRAME have
  // frame number
  if (sub_tag_value == HPROF_ROOT_THREAD_OBJ ||
      sub_tag_value == HPROF_ROOT_JNI_LOCAL ||
      sub_tag_value == HPROF_ROOT_JAVA_FRAME) {
    if (!byte_iterator_->ReadU4(record_data.frame_number)) {
      PERFETTO_FATAL("Failed to read thread object root frame number");
    }
    PERFETTO_DLOG("Thread-related root (tag 0x%x): objectID=%" PRIu64
                  ", threadID=%u, frameNumber=%u",
                  sub_tag_value, record_data.object_id, record_data.thread_id,
                  record_data.frame_number);
  } else {
    PERFETTO_DLOG("Thread-related root (tag 0x%x): objectID=%" PRIu64
                  ", threadID=%u",
                  sub_tag_value, record_data.object_id, record_data.thread_id);
  }

  AddHeapRecord(static_cast<HprofHeapTag>(record_data.root_type), record_data);

  data_.root_objects[record_data.object_id] = record_data.root_type;
  data_.root_count++;
}

void HprofParser::ParseSimpleRoot(uint8_t sub_tag_value) {
  PERFETTO_DLOG("Parsing simple root with tag 0x%x", sub_tag_value);

  RootRecordData record_data;
  record_data.root_type = sub_tag_value;

  if (!byte_iterator_->ReadId(record_data.object_id, identifier_size_)) {
    PERFETTO_FATAL("Failed to read simple root");
  }

  PERFETTO_DLOG("Simple root (tag 0x%x): objectID=%" PRIu64, sub_tag_value,
                record_data.object_id);

  AddHeapRecord(static_cast<HprofHeapTag>(record_data.root_type), record_data);

  data_.root_objects[record_data.object_id] = record_data.root_type;
  data_.root_count++;
}

void HprofParser::ParseHeapDumpInfo() {
  PERFETTO_DLOG("Parsing heap dump info");

  HeapDumpInfoData heap_data;

  if (!byte_iterator_->ReadU4(heap_data.heap_id) ||
      !byte_iterator_->ReadId(heap_data.heap_name_string_id,
                              identifier_size_)) {
    PERFETTO_FATAL("Failed to read heap dump info");
  }

  heap_data.heap_name = GetString(heap_data.heap_name_string_id);
  if (heap_data.heap_name == "???") {
    // Fallback name based on heap ID if string ID not found
    heap_data.heap_name = "unknown-heap-" + std::to_string(heap_data.heap_id);
  }

  PERFETTO_DLOG("Heap dump info: heapID=%u, heapName='%s'", heap_data.heap_id,
                heap_data.heap_name.c_str());

  current_heap_ = static_cast<HprofHeapId>(heap_data.heap_id);

  AddHeapRecord(HPROF_HEAP_DUMP_INFO, heap_data);
  data_.heap_info_count++;
}

void HprofParser::ParseClassDump() {
  PERFETTO_DLOG("Starting to parse class dump");

  ClassDumpData class_data;

  uint64_t reserved1, reserved2;
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

  class_data.heap_id = current_heap_;

  ClassInfo& class_info = data_.classes[class_data.class_object_id];
  class_info.super_class_id = class_data.super_class_object_id;
  class_info.instance_size = class_data.instance_size;

  // Name might have been populated by LOAD_CLASS record, but set object ID
  // here explicitly as this is the definitive class definition.
  class_info.class_object_id = class_data.class_object_id;

  // Check if this is a String class (name might be available from LOAD_CLASS)
  if (IsStringClass(class_info.name)) {
    class_info.is_string_class = true;
    class_data.is_string_class = true;
  }

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
    if (!byte_iterator_->SkipBytes(GetFieldTypeSize(type))) {
      PERFETTO_FATAL("Failed to skip constant pool value");
    }
  }

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
    field.name = GetString(name_string_id);
    field.type = type;
    class_data.static_fields.push_back(field);

    if (!byte_iterator_->SkipBytes(GetFieldTypeSize(type))) {
      PERFETTO_FATAL("Failed to skip static field value");
    }
  }

  uint16_t numInstanceFields;
  if (!byte_iterator_->ReadU2(numInstanceFields)) {
    PERFETTO_FATAL("Failed to read number of instance fields");
  }

  PERFETTO_DLOG("Instance field count: %u", numInstanceFields);
  class_data.instance_fields.reserve(numInstanceFields);
  class_info.fields.clear();
  class_info.fields.reserve(numInstanceFields);

  for (uint16_t i = 0; i < numInstanceFields; ++i) {
    uint64_t fieldNameId;
    if (!byte_iterator_->ReadId(fieldNameId, identifier_size_)) {
      PERFETTO_FATAL("Failed to read instance field name ID");
    }

    uint8_t type;
    if (!byte_iterator_->ReadU1(type)) {
      PERFETTO_FATAL("Failed to read instance field type");
    }

    FieldInfo field;
    field.name = GetString(fieldNameId);
    field.type = type;
    class_data.instance_fields.push_back(field);
    class_info.fields.push_back(field);

    if (type == TYPE_OBJECT) {
      data_.field_reference_count++;
    }
  }

  if (data_.java_lang_class_object_id == 0) {
    PERFETTO_LOG("Warning: No class definition found for java.lang.Class");
  }

  AddHeapRecord(HPROF_CLASS_DUMP, class_data);
}

void HprofParser::ParseInstanceDump() {
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
  data_.object_to_class[instance_data.object_id] =
      instance_data.class_object_id;

  // Read instance data
  if (!byte_iterator_->ReadBytes(instance_data.raw_instance_data,
                                 data_length)) {
    PERFETTO_FATAL("Failed to read instance data");
  }

  // Get class info and process fields
  auto class_it = data_.classes.find(instance_data.class_object_id);
  if (class_it == data_.classes.end()) {
    PERFETTO_DLOG("Warning: Class info not found for class ID: %" PRIu64,
                  instance_data.class_object_id);
    // Add record even if class info is missing
    AddHeapRecord(HPROF_INSTANCE_DUMP, instance_data);
    data_.class_instance_count++;
    return;
  }

  const ClassInfo& class_info = class_it->second;
  bool is_string_class = class_info.is_string_class;

  PERFETTO_DLOG("Processing instance of class: %s%s", class_info.name.c_str(),
                is_string_class ? " (String class)" : "");

  // Get all instance fields from class hierarchy
  std::vector<FieldInfo> instance_fields =
      GetFieldsForClassHierarchy(instance_data.class_object_id);

  // Process instance fields
  size_t offset = 0;
  for (const auto& field : instance_fields) {
    if (offset >= data_length) {
      PERFETTO_DLOG("Warning: Instance data too short for field '%s'",
                    field.name.c_str());
      break;
    }

    // Handle reference fields (TYPE_OBJECT)
    if (field.type == TYPE_OBJECT) {
      uint64_t target_id = 0;

      // Read object ID based on identifier size
      if (identifier_size_ == 4 && offset + 4 <= data_length) {
        uint32_t id_val = 0;
        memcpy(&id_val, &instance_data.raw_instance_data[offset], 4);
        // Convert from big-endian
        id_val = ((id_val & 0xFF000000) >> 24) | ((id_val & 0x00FF0000) >> 8) |
                 ((id_val & 0x0000FF00) << 8) | ((id_val & 0x000000FF) << 24);
        target_id = id_val;
      } else if (identifier_size_ == 8 && offset + 8 <= data_length) {
        uint64_t id_val = 0;
        memcpy(&id_val, &instance_data.raw_instance_data[offset], 8);
        // Convert from big-endian
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(
            &instance_data.raw_instance_data[offset]);
        id_val = (static_cast<uint64_t>(bytes[0]) << 56) |
                 (static_cast<uint64_t>(bytes[1]) << 48) |
                 (static_cast<uint64_t>(bytes[2]) << 40) |
                 (static_cast<uint64_t>(bytes[3]) << 32) |
                 (static_cast<uint64_t>(bytes[4]) << 24) |
                 (static_cast<uint64_t>(bytes[5]) << 16) |
                 (static_cast<uint64_t>(bytes[6]) << 8) |
                 static_cast<uint64_t>(bytes[7]);
        target_id = id_val;
      }

      // Add reference if target is non-null
      if (target_id != 0) {
        ObjectReference ref;
        ref.field_name = field.name;
        ref.target_object_id = target_id;

        // Add reference to instance and global maps
        instance_data.references.push_back(ref);
        data_.owner_to_owned[instance_data.object_id].push_back(ref);

        PERFETTO_DLOG("Added reference: owner=%" PRIu64
                      ", field=%s, target=%" PRIu64,
                      instance_data.object_id, field.name.c_str(), target_id);
      }
    }

    // Move to next field
    offset += GetFieldTypeSize(field.type);
  }

  // Special handling for String class
  if (is_string_class) {
    // Look for char array reference in "value" field
    for (const auto& ref : instance_data.references) {
      if (ref.field_name == "value") {
        // Add special stringValue reference
        ObjectReference string_value_ref;
        string_value_ref.field_name = "stringValue";
        string_value_ref.target_object_id = ref.target_object_id;

        instance_data.references.push_back(string_value_ref);
        data_.owner_to_owned[instance_data.object_id].push_back(
            string_value_ref);

        PERFETTO_DLOG("Added stringValue reference for String object %" PRIu64,
                      instance_data.object_id);
        break;
      }
    }
  }

  // Add the instance dump record
  AddHeapRecord(HPROF_INSTANCE_DUMP, instance_data);
  data_.class_instance_count++;
}

void HprofParser::ParseObjectArrayDump() {
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

  array_data.heap_id = current_heap_;
  data_.object_to_class[array_data.array_object_id] =
      array_data.array_class_object_id;

  array_data.elements.reserve(size);
  for (uint32_t i = 0; i < size; i++) {
    uint64_t element_id;
    if (!byte_iterator_->ReadId(element_id, identifier_size_)) {
      PERFETTO_FATAL("Failed to read array element");
    }
    array_data.elements.push_back(element_id);

    if (element_id != 0) {
      // Create reference for non-null array element
      ObjectReference ref;
      ref.field_name = "[" + std::to_string(i) + "]";
      ref.target_object_id = element_id;

      data_.owner_to_owned[array_data.array_object_id].push_back(ref);

      data_.field_reference_count++;
    }
  }

  AddHeapRecord(HPROF_OBJ_ARRAY_DUMP, array_data);
  data_.object_array_count++;
}

void HprofParser::ParsePrimitiveArrayDump() {
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

  array_data.heap_id = current_heap_;

  size_t element_size = GetFieldTypeSize(array_data.element_type);
  size_t bytes_to_read = size * element_size;

  PERFETTO_DLOG("Reading %zu bytes of array data", bytes_to_read);

  array_data.elements.resize(bytes_to_read);
  if (!byte_iterator_->ReadBytes(array_data.elements, bytes_to_read)) {
    PERFETTO_FATAL("Failed to read primitive array data");
  }

  AddHeapRecord(HPROF_PRIM_ARRAY_DUMP, array_data);
  data_.primitive_array_count++;
}

bool HprofParser::IsStringClass(const std::string& class_name) const {
  return class_name == "java.lang.String";
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
      PERFETTO_FATAL("Unknown HPROF field type: %u", type);
      return 0;  // Should not be reached
  }
}

std::vector<FieldInfo> HprofParser::GetFieldsForClassHierarchy(
    uint64_t class_object_id) {
  std::vector<FieldInfo> all_fields;
  std::unordered_set<uint64_t> visited_classes;

  // Start with the current class
  uint64_t current_class_id = class_object_id;

  // Build the class hierarchy (superclass-first order)
  std::vector<uint64_t> class_hierarchy;

  while (current_class_id != 0) {
    // Check for cycles
    if (visited_classes.count(current_class_id) > 0) {
      PERFETTO_DLOG("Cycle detected in class hierarchy for class %" PRIu64,
                    class_object_id);
      break;
    }

    visited_classes.insert(current_class_id);

    // Find class info
    auto it = data_.classes.find(current_class_id);
    if (it == data_.classes.end()) {
      break;  // Class not found
    }

    // Add to hierarchy
    class_hierarchy.push_back(current_class_id);

    // Move to superclass
    current_class_id = it->second.super_class_id;
  }

  // Reverse to get superclass-first order
  std::reverse(class_hierarchy.begin(), class_hierarchy.end());

  // Collect fields from all classes in the hierarchy
  for (uint64_t class_id : class_hierarchy) {
    auto it = data_.classes.find(class_id);
    if (it != data_.classes.end()) {
      const std::vector<FieldInfo>& fields = it->second.fields;
      all_fields.insert(all_fields.end(), fields.begin(), fields.end());
    }
  }

  return all_fields;
}
}  // namespace perfetto::trace_processor::art_hprof
