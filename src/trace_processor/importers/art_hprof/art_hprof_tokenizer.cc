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
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace perfetto::trace_processor::art_hprof {

// Constants
constexpr uint32_t kHprofHeaderMagic = 0x4A415641;  // "JAVA"
constexpr uint32_t kHprofHeaderLength = 20;

// AndroidHeapStats implementation
void HprofAst::AndroidHeapStats::AddObject(size_t size) {
  object_count++;
  total_bytes += size;
}

// HprofParser implementation
HprofParser::~HprofParser() = default;

// Define the virtual destructor out-of-line to fix weak vtable warning
ByteIterator::~ByteIterator() = default;

HprofAst HprofParser::Parse() {
  PERFETTO_DLOG("Beginning to parse HPROF");

  if (!ParseHeader()) {
    PERFETTO_FATAL("Failed to parse HPROF header");
  }

  ParseRecords();

  // Post-processing: detect String classes and mark them
  if (detect_string_class_) {
    PERFETTO_DLOG("Post-processing: Detecting String classes");

    for (auto& [class_id, class_info] : ast_.classes) {
      if (IsStringClass(class_info.name)) {
        class_info.is_string_class = true;
        PERFETTO_DLOG("Detected String class: %s", class_info.name.c_str());

        // Check for count field which could indicate string compression
        for (const auto& field : class_info.fields) {
          if (field.name == "count" && field.type == TYPE_INT) {
            ast_.use_string_compression = true;
            class_info.use_string_compression = true;
            PERFETTO_DLOG("Detected string compression in: %s",
                          class_info.name.c_str());
            break;
          }
        }
      }
    }
  }

  // Summary statistics
  PERFETTO_DLOG(
      "Parsing Summary - String count: %zu, Class count: %zu, "
      "Heap dump count: %zu, Class instance count: %zu, "
      "Object array count: %zu, Primitive array count: %zu, "
      "Root count: %zu, Field reference count: %zu, "
      "Heap info count: %zu",
      ast_.string_count, ast_.class_count, ast_.heap_dump_count,
      ast_.class_instance_count, ast_.object_array_count,
      ast_.primitive_array_count, ast_.root_count, ast_.field_reference_count,
      ast_.heap_info_count);

  return ast_;
}

bool HprofParser::IsStringClass(const std::string& class_name) const {
  return class_name == "java.lang.String" || class_name == "java/lang/String" ||
         class_name == "Ljava/lang/String;";
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
    auto it = ast_.classes.find(current_cid);
    if (it == ast_.classes.end()) {
      PERFETTO_ELOG("Class ID %" PRIu64
                    " not found in AST while building hierarchy for %" PRIu64,
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
    auto it = ast_.classes.find(cid_in_order);
    if (it != ast_.classes.end()) {
      all_fields.insert(all_fields.end(), it->second.fields.begin(),
                        it->second.fields.end());
    }
  }

  return all_fields;
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

    auto class_it = ast_.classes.find(current_class_id);
    if (class_it != ast_.classes.end()) {
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
    if (field_info.type == TYPE_OBJECT && value.type == FieldValue::OBJECT_ID &&
        value.object_id_value != 0) {
      ObjectReference ref;

      // Create appropriate field name
      if (!field_info.name.empty()) {
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
      } else {
        // Fallback for fields without names
        ref.field_name = "field_" + std::to_string(field_index);
        PERFETTO_DLOG(
            "Warning: Unresolved field name for field index %zu in class %s "
            "(instance %" PRIu64 ")",
            field_index, class_info.name.c_str(), instance_data.object_id);
      }

      ref.target_object_id = value.object_id_value;

      // Add reference to both instance and global map
      instance_data.references.push_back(ref);
      ast_.owner_to_owned[instance_data.object_id].push_back(ref);
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
        field_value.value.type == FieldValue::OBJECT_ID) {
      char_array_id = field_value.value.object_id_value;
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
  ast_.owner_to_owned[instance_data.object_id].push_back(ref);
}

void HprofParser::UpdateHeapStats(HprofHeapId heap_id, size_t object_size) {
  ast_.android_heap_stats[heap_id].AddObject(object_size);
}

void HprofParser::SkipUnknownSubRecord(
    uint8_t sub_tag,
    [[maybe_unused]] std::streampos end_pos) {
  PERFETTO_DLOG("Skipping unknown sub-record with tag: 0x%x",
                static_cast<int>(sub_tag));

  // Simple root records with just an object ID
  if (sub_tag >= 0x01 && sub_tag <= 0x0a) {
    byte_iterator_->SkipBytes(identifier_size_);
  } else {
    // For other unknown tags, skip a byte
    byte_iterator_->SkipBytes(1);
  }
}

bool HprofParser::ParseHeader() {
  PERFETTO_DLOG("Parsing HPROF header");

  // Read format string until null terminator
  char c;
  ast_.header.format = "";
  while (byte_iterator_->ReadU1(reinterpret_cast<uint8_t&>(c)) && c != 0) {
    ast_.header.format.push_back(c);
  }

  // Read identifier size
  if (!byte_iterator_->ReadU4(ast_.header.identifier_size)) {
    PERFETTO_ELOG("Error: Failed to read ID size");
    return false;
  }

  identifier_size_ = ast_.header.identifier_size;

  // Read timestamp (high and low 32 bits)
  uint32_t high_time, low_time;
  if (!byte_iterator_->ReadU4(high_time) || !byte_iterator_->ReadU4(low_time)) {
    PERFETTO_ELOG("Error: Failed to read timestamp");
    return false;
  }

  ast_.header.timestamp = (static_cast<uint64_t>(high_time) << 32) | low_time;

  PERFETTO_DLOG("HPROF header: format=%s, idSize=%u",
                ast_.header.format.c_str(), identifier_size_);
  return true;
}

void HprofParser::ParseRecords() {
  PERFETTO_DLOG("Parsing HPROF records");

  while (byte_iterator_->IsValid() && !byte_iterator_->IsEof()) {
    // Try to read the tag
    uint8_t tag;
    if (!byte_iterator_->ReadU1(tag)) {
      if (byte_iterator_->IsEof()) {
        break;
      }
      PERFETTO_FATAL("Failed to read record tag");
    }

    // Read time and length
    uint32_t time, length;
    if (!byte_iterator_->ReadU4(time) || !byte_iterator_->ReadU4(length)) {
      PERFETTO_FATAL("Failed to read record time/length");
    }

    // Parse the record based on its tag
    ParseRecord(tag, time, length);
  }

  PERFETTO_DLOG("Finished parsing records");
}

void HprofParser::ParseRecord(uint8_t tag, uint32_t time, uint32_t length) {
  PERFETTO_DLOG("Parsing record with tag: 0x%x, time: %u, length: %u",
                static_cast<int>(tag), time, length);

  HprofRecord record;
  record.tag = tag;
  record.time = time;
  record.length = length;

  switch (tag) {
    case HPROF_UTF8:
      ParseUtf8Record(record);
      break;
    case HPROF_LOAD_CLASS:
      ParseLoadClassRecord(record);
      break;
    case HPROF_HEAP_DUMP:
    case HPROF_HEAP_DUMP_SEGMENT:
      ParseHeapDumpRecord(record);
      break;
    case HPROF_HEAP_DUMP_END:
      // End of a heap dump segment
      PERFETTO_DLOG("Encountered HEAP_DUMP_END tag");
      record.data = std::monostate{};
      ast_.records.push_back(record);
      break;
    default:
      // Generic record - skip the payload
      PERFETTO_DLOG("Skipping unknown record payload of length %u", length);
      byte_iterator_->SkipBytes(length);
      record.data = std::monostate{};
      ast_.records.push_back(record);
      break;
  }
}

void HprofParser::ParseUtf8Record(HprofRecord& record) {
  PERFETTO_DLOG("Parsing UTF8 record");

  Utf8StringData data;
  uint64_t name_id;

  if (!byte_iterator_->ReadId(name_id, identifier_size_)) {
    PERFETTO_FATAL("Failed to read string ID");
  }
  data.name_id = name_id;

  size_t string_length = record.length - identifier_size_;
  if (!byte_iterator_->ReadString(data.utf8_string, string_length)) {
    PERFETTO_FATAL("Failed to read string data");
  }

  PERFETTO_DLOG("Read UTF8 string: ID=%" PRIu64 ", string='%s'", name_id,
                data.utf8_string.c_str());

  record.data = data;
  ast_.records.push_back(record);

  // Store string for later reference
  ast_.id_to_string_map[data.name_id] = data.utf8_string;
  ast_.string_count++;
}

void HprofParser::ParseLoadClassRecord(HprofRecord& record) {
  PERFETTO_DLOG("Parsing LOAD_CLASS record");

  LoadClassData data;

  if (!byte_iterator_->ReadU4(data.class_serial_num) ||
      !byte_iterator_->ReadId(data.class_object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(data.stack_trace_serial_num) ||
      !byte_iterator_->ReadId(data.class_name_id, identifier_size_)) {
    PERFETTO_FATAL("Failed to read load class record");
  }

  // Resolve class name if possible
  auto name_it = ast_.id_to_string_map.find(data.class_name_id);
  if (name_it != ast_.id_to_string_map.end()) {
    data.class_name = name_it->second;

    // Normalize class name (replace / with .)
    std::replace(data.class_name.begin(), data.class_name.end(), '/', '.');

    PERFETTO_DLOG("Class loaded: serial=%u, id=%" PRIu64 ", name='%s'",
                  data.class_serial_num, data.class_object_id,
                  data.class_name.c_str());

    // Store class info
    ClassInfo& class_info = ast_.classes[data.class_object_id];
    class_info.name = data.class_name;
    class_info.class_object_id = data.class_object_id;
    class_info.is_string_class = IsStringClass(data.class_name);
  } else {
    PERFETTO_DLOG("Class loaded but name not resolved: serial=%u, id=%" PRIu64,
                  data.class_serial_num, data.class_object_id);
  }

  record.data = data;
  ast_.records.push_back(record);
  ast_.class_serial_to_id[data.class_serial_num] = data.class_object_id;
  ast_.class_count++;
}

void HprofParser::ParseHeapDumpRecord(HprofRecord& record) {
  PERFETTO_DLOG("Parsing HEAP_DUMP or HEAP_DUMP_SEGMENT record");

  HeapDumpData data;

  // Record the end position
  std::streampos end_pos = byte_iterator_->GetPosition();
  end_pos += record.length;

  // Parse heap dump sub-records
  while (byte_iterator_->GetPosition() < end_pos) {
    uint8_t sub_tag;
    if (!byte_iterator_->ReadU1(sub_tag)) {
      if (byte_iterator_->IsEof()) {
        break;
      }
      PERFETTO_FATAL("Failed to read heap dump sub-record tag");
    }

    PERFETTO_DLOG("Parsing heap sub-record with tag: 0x%x",
                  static_cast<int>(sub_tag));

    // Try to parse the sub-record, continue even if it fails
    if (!ParseHeapSubRecord(sub_tag, data.records)) {
      // Skip to the next sub-record based on tag type
      SkipUnknownSubRecord(sub_tag, end_pos);
    }

    // Safety check: if we've gone past the end position or hit EOF, break
    if (byte_iterator_->GetPosition() >= end_pos || byte_iterator_->IsEof()) {
      break;
    }
  }

  record.data = data;
  ast_.records.push_back(record);
  ast_.heap_dump_count++;
}

bool HprofParser::ParseHeapSubRecord(
    uint8_t sub_tag,
    std::vector<HprofHeapRecord>& sub_records) {
  HprofHeapRecord record;
  record.tag = static_cast<HprofHeapTag>(sub_tag);

  switch (sub_tag) {
    case HPROF_ROOT_JNI_GLOBAL:
      ParseRootJniGlobal(record);
      // Store root information in AST
      if (std::holds_alternative<RootRecordData>(record.data)) {
        const auto& root_data = std::get<RootRecordData>(record.data);
        ast_.root_objects[root_data.object_id] = sub_tag;
      }
      break;
    case HPROF_ROOT_JNI_LOCAL:
    case HPROF_ROOT_JAVA_FRAME:
    case HPROF_ROOT_THREAD_BLOCK:
      ParseRootWithThread(record);
      // Store root information in AST
      if (std::holds_alternative<RootRecordData>(record.data)) {
        const auto& root_data = std::get<RootRecordData>(record.data);
        ast_.root_objects[root_data.object_id] = sub_tag;
      }
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
      ParseSimpleRoot(record);
      // Store root information in AST
      if (std::holds_alternative<RootRecordData>(record.data)) {
        const auto& root_data = std::get<RootRecordData>(record.data);
        ast_.root_objects[root_data.object_id] = sub_tag;
      }
      break;
    case HPROF_ROOT_THREAD_OBJ:
      ParseThreadObjectRoot(record);
      // Store root information in AST
      if (std::holds_alternative<RootRecordData>(record.data)) {
        const auto& root_data = std::get<RootRecordData>(record.data);
        ast_.root_objects[root_data.object_id] = sub_tag;
      }
      break;
    case HPROF_HEAP_DUMP_INFO:
      ParseHeapDumpInfo(record);
      break;
    case HPROF_CLASS_DUMP:
      ParseClassDump(record);
      break;
    case HPROF_INSTANCE_DUMP:
      ParseInstanceDump(record);
      break;
    case HPROF_OBJ_ARRAY_DUMP:
      ParseObjectArrayDump(record);
      break;
    case HPROF_PRIM_ARRAY_DUMP:
      ParsePrimitiveArrayDump(record);
      break;
    default:
      PERFETTO_ELOG("Unknown heap dump sub-tag: 0x%x",
                    static_cast<int>(sub_tag));
      return false;  // Skip this sub-record but continue parsing
  }

  sub_records.push_back(record);
  return true;
}

void HprofParser::ParseRootJniGlobal(HprofHeapRecord& record) {
  PERFETTO_DLOG("Parsing JNI GLOBAL root");

  RootRecordData data;
  data.root_type = record.tag;

  uint64_t global_ref_id;  // Temporary variable for the second ID
  if (!byte_iterator_->ReadId(data.object_id, identifier_size_) ||
      !byte_iterator_->ReadId(global_ref_id, identifier_size_)) {
    PERFETTO_FATAL("Failed to read JNI GLOBAL root");
  }

  record.data = data;
  ast_.root_count++;
}

void HprofParser::ParseRootWithThread(HprofHeapRecord& record) {
  PERFETTO_DLOG("Parsing thread-related root of type 0x%x",
                static_cast<int>(record.tag));

  RootRecordData data;
  data.root_type = record.tag;

  if (!byte_iterator_->ReadId(data.object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(data.thread_id) ||
      !byte_iterator_->ReadU4(data.frame_number)) {
    PERFETTO_FATAL("Failed to read frame root");
  }

  PERFETTO_DLOG("Thread-related root: objectID=%" PRIu64
                ", threadID=%u, frameNumber=%u",
                data.object_id, data.thread_id, data.frame_number);

  record.data = data;
  ast_.root_count++;
}

void HprofParser::ParseSimpleRoot(HprofHeapRecord& record) {
  PERFETTO_DLOG("Parsing simple root of type 0x%x",
                static_cast<int>(record.tag));

  RootRecordData data;
  data.root_type = record.tag;

  if (!byte_iterator_->ReadId(data.object_id, identifier_size_)) {
    PERFETTO_FATAL("Failed to read simple root");
  }

  PERFETTO_DLOG("Simple root: objectID=%" PRIu64, data.object_id);

  record.data = data;
  ast_.root_count++;
}

void HprofParser::ParseThreadObjectRoot(HprofHeapRecord& record) {
  PERFETTO_DLOG("Parsing thread object root");

  RootRecordData data;
  data.root_type = record.tag;

  if (!byte_iterator_->ReadId(data.object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(data.thread_id) ||
      !byte_iterator_->ReadU4(data.frame_number)) {
    PERFETTO_FATAL("Failed to read thread object root");
  }

  PERFETTO_DLOG("Thread object root: objectID=%" PRIu64
                ", threadID=%u, stackTraceSerial=%u",
                data.object_id, data.thread_id, data.frame_number);

  record.data = data;
  ast_.root_count++;
}

void HprofParser::ParseHeapDumpInfo(HprofHeapRecord& record) {
  PERFETTO_DLOG("Parsing heap dump info");

  HeapDumpInfoData data;

  if (!byte_iterator_->ReadU4(data.heap_id) ||
      !byte_iterator_->ReadId(data.heap_name_string_id, identifier_size_)) {
    PERFETTO_FATAL("Failed to read heap dump info");
  }

  auto name_it = ast_.id_to_string_map.find(data.heap_name_string_id);
  if (name_it != ast_.id_to_string_map.end()) {
    data.heap_name = name_it->second;
  }

  PERFETTO_DLOG("Heap dump info: heapID=%u, heapName='%s'", data.heap_id,
                data.heap_name.c_str());

  // Set current heap for subsequent objects
  current_heap_ = static_cast<HprofHeapId>(data.heap_id);

  record.data = data;
  ast_.heap_info_count++;
}

void HprofParser::ParseClassDump(HprofHeapRecord& record) {
  PERFETTO_DLOG("Starting to parse class dump");

  ClassDumpData data;

  uint64_t reserved1, reserved2;  // Temporary variables for reserved fields
  if (!byte_iterator_->ReadId(data.class_object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(data.stack_trace_serial_num) ||
      !byte_iterator_->ReadId(data.super_class_object_id, identifier_size_) ||
      !byte_iterator_->ReadId(data.class_loader_object_id, identifier_size_) ||
      !byte_iterator_->ReadId(data.signers_object_id, identifier_size_) ||
      !byte_iterator_->ReadId(data.protection_domain_object_id,
                              identifier_size_) ||
      !byte_iterator_->ReadId(reserved1, identifier_size_) ||
      !byte_iterator_->ReadId(reserved2, identifier_size_) ||
      !byte_iterator_->ReadU4(data.instance_size)) {
    PERFETTO_FATAL("Failed to read class dump header");
  }

  PERFETTO_DLOG("Class dump header: classID=%" PRIu64 ", superClassID=%" PRIu64
                ", instanceSize=%u",
                data.class_object_id, data.super_class_object_id,
                data.instance_size);

  // Get existing class info if any
  auto class_it = ast_.classes.find(data.class_object_id);
  if (class_it != ast_.classes.end()) {
    PERFETTO_DLOG("Found existing class info for ID %" PRIu64 " with name '%s'",
                  data.class_object_id, class_it->second.name.c_str());
  } else {
    PERFETTO_DLOG("No existing class info found for ID %" PRIu64,
                  data.class_object_id);
  }

  // Update class info
  auto& class_info = ast_.classes[data.class_object_id];
  class_info.super_class_id = data.super_class_object_id;
  class_info.instance_size = data.instance_size;
  data.is_string_class = class_info.is_string_class;

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

    // Skip value based on type
    size_t type_size = GetFieldTypeSize(type);

    PERFETTO_DLOG("Skipping constant pool entry: index=%u, type=%u, size=%zu",
                  index, type, type_size);

    if (!byte_iterator_->SkipBytes(type_size)) {
      PERFETTO_FATAL("Failed to skip constant pool value");
    }
  }

  // Read static fields
  uint16_t static_field_count;
  if (!byte_iterator_->ReadU2(static_field_count)) {
    PERFETTO_FATAL("Failed to read static field count");
  }

  PERFETTO_DLOG("Static field count: %u", static_field_count);

  data.static_fields.reserve(static_field_count);
  for (uint16_t i = 0; i < static_field_count; i++) {
    uint64_t name_string_id;
    uint8_t type;
    if (!byte_iterator_->ReadId(name_string_id, identifier_size_) ||
        !byte_iterator_->ReadU1(type)) {
      PERFETTO_FATAL("Failed to read static field");
    }

    FieldInfo field;
    field.type = type;

    auto name_it = ast_.id_to_string_map.find(name_string_id);
    if (name_it != ast_.id_to_string_map.end()) {
      field.name = name_it->second;
      PERFETTO_DLOG("Static field %u: name_id=%" PRIu64 ", name='%s', type=%u",
                    i, name_string_id, field.name.c_str(), type);
    } else {
      PERFETTO_DLOG("WARNING: Static field %u: name_id=%" PRIu64
                    " NOT FOUND in string map, type=%u",
                    i, name_string_id, type);
    }

    data.static_fields.push_back(field);

    // Skip value based on type
    if (!byte_iterator_->SkipBytes(GetFieldTypeSize(type))) {
      PERFETTO_FATAL("Failed to skip static field value");
    }
  }

  // Read instance fields
  uint16_t instance_field_count;
  if (!byte_iterator_->ReadU2(instance_field_count)) {
    PERFETTO_FATAL("Failed to read instance field count");
  }

  PERFETTO_DLOG("Instance field count: %u for class ID %" PRIu64,
                instance_field_count, data.class_object_id);

  size_t fields_with_names = 0;
  size_t fields_without_names = 0;

  data.instance_fields.reserve(instance_field_count);
  for (uint16_t i = 0; i < instance_field_count; i++) {
    uint64_t name_string_id;
    uint8_t type;
    if (!byte_iterator_->ReadId(name_string_id, identifier_size_) ||
        !byte_iterator_->ReadU1(type)) {
      PERFETTO_FATAL("Failed to read instance field");
    }

    FieldInfo field;
    field.type = type;

    auto name_it = ast_.id_to_string_map.find(name_string_id);
    if (name_it != ast_.id_to_string_map.end()) {
      field.name = name_it->second;
      fields_with_names++;
      PERFETTO_DLOG("Instance field %u: name_id=%" PRIu64
                    ", name='%s', type=%u",
                    i, name_string_id, field.name.c_str(), type);
    } else {
      fields_without_names++;
      PERFETTO_DLOG("WARNING: Instance field %u: name_id=%" PRIu64
                    " NOT FOUND in string map, type=%u",
                    i, name_string_id, type);
    }

    data.instance_fields.push_back(field);

    // Add field to class info
    class_info.fields.push_back(field);

    // Track reference fields
    if (type == TYPE_OBJECT) {
      ast_.field_reference_count++;
      PERFETTO_DLOG("Added reference field: class=%" PRIu64 ", field='%s'",
                    data.class_object_id, field.name.c_str());
    }
  }

  PERFETTO_DLOG("Finished parsing class %" PRIu64
                ": %zu fields with names, %zu fields without names",
                data.class_object_id, fields_with_names, fields_without_names);

  // Check if fields were added to class info
  PERFETTO_DLOG("Class %" PRIu64 " now has %zu fields in class_info",
                data.class_object_id, class_info.fields.size());

  record.data = data;
}

void HprofParser::ParseInstanceDump(HprofHeapRecord& record) {
  PERFETTO_DLOG("Parsing instance dump");

  InstanceDumpData data;
  uint32_t data_length;

  if (!byte_iterator_->ReadId(data.object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(data.stack_trace_serial_num) ||
      !byte_iterator_->ReadId(data.class_object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(data_length)) {
    PERFETTO_FATAL("Failed to read instance dump header");
  }

  PERFETTO_DLOG("Instance dump: objectID=%" PRIu64 ", classID=%" PRIu64
                ", dataLength=%u",
                data.object_id, data.class_object_id, data_length);

  // Set heap ID (from current heap context)
  data.heap_id = current_heap_;

  // Store object to class mapping
  ast_.object_to_class[data.object_id] = data.class_object_id;

  // Read instance data
  if (!byte_iterator_->ReadBytes(data.raw_instance_data, data_length)) {
    PERFETTO_FATAL("Failed to read instance data");
  }

  PERFETTO_DLOG("Read %u bytes of instance data", data_length);

  // Process fields if we have class info
  auto class_it = ast_.classes.find(data.class_object_id);
  if (class_it != ast_.classes.end()) {
    const ClassInfo& class_info = class_it->second;
    bool is_string_instance = class_info.is_string_class;

    PERFETTO_DLOG("Processing fields for class: %s%s", class_info.name.c_str(),
                  is_string_instance ? " (String class)" : "");

    // Extract and process all instance fields
    ExtractInstanceFields(data, class_info);

    // After ExtractInstanceFields, try to extract string value if this is a
    // String class
    if (is_string_instance) {
      ExtractStringInstance(data, class_info);
    }

    // Update heap statistics
    UpdateHeapStats(current_heap_, data_length);
  } else {
    PERFETTO_DLOG("Warning: Class info not found for class ID: %" PRIu64,
                  data.class_object_id);
  }

  record.data = data;
  ast_.class_instance_count++;
}

void HprofParser::ParseObjectArrayDump(HprofHeapRecord& record) {
  PERFETTO_DLOG("Parsing object array dump");

  ObjArrayDumpData data;
  uint32_t size;

  if (!byte_iterator_->ReadId(data.array_object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(data.stack_trace_serial_num) ||
      !byte_iterator_->ReadU4(size) ||
      !byte_iterator_->ReadId(data.array_class_object_id, identifier_size_)) {
    PERFETTO_FATAL("Failed to read object array dump header");
  }

  PERFETTO_DLOG("Object array: objectID=%" PRIu64 ", classID=%" PRIu64
                ", size=%u",
                data.array_object_id, data.array_class_object_id, size);

  // Set heap ID (from current heap context)
  data.heap_id = current_heap_;

  // Read elements
  data.elements.reserve(size);
  for (uint32_t i = 0; i < size; i++) {
    uint64_t element_id;
    if (!byte_iterator_->ReadId(element_id, identifier_size_)) {
      PERFETTO_FATAL("Failed to read array element");
    }

    data.elements.push_back(element_id);

    // Store reference from array to element (for non-null elements)
    if (element_id != 0) {
      ObjectReference ref;
      // For arrays, use array index format
      ref.field_name = "[" + std::to_string(i) + "]";
      ref.target_object_id = element_id;

      // Add to global map
      ast_.owner_to_owned[data.array_object_id].push_back(ref);
    }
  }

  // Update heap statistics
  UpdateHeapStats(current_heap_, size * identifier_size_);

  record.data = data;
  ast_.object_array_count++;
}

void HprofParser::ParsePrimitiveArrayDump(HprofHeapRecord& record) {
  PERFETTO_DLOG("Parsing primitive array dump");

  PrimArrayDumpData data;
  uint32_t size;

  if (!byte_iterator_->ReadId(data.array_object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(data.stack_trace_serial_num) ||
      !byte_iterator_->ReadU4(size) ||
      !byte_iterator_->ReadU1(data.element_type)) {
    PERFETTO_FATAL("Failed to read primitive array dump header");
  }

  PERFETTO_DLOG("Primitive array: objectID=%" PRIu64 ", type=%d, size=%u",
                data.array_object_id, static_cast<int>(data.element_type),
                size);

  // Set heap ID (from current heap context)
  data.heap_id = current_heap_;

  // Determine element size and read data
  size_t element_size = GetFieldTypeSize(data.element_type);
  size_t bytes_to_read = size * element_size;

  PERFETTO_DLOG("Reading %zu bytes of array data", bytes_to_read);

  if (!byte_iterator_->ReadBytes(data.elements, bytes_to_read)) {
    PERFETTO_FATAL("Failed to read primitive array data");
  }

  // Update heap statistics
  UpdateHeapStats(current_heap_, bytes_to_read);

  record.data = data;
  ast_.primitive_array_count++;
}

// HprofAstConverter implementation
HeapGraphIR HprofAstConverter::ConvertToIR(const HprofAst& ast) {
  PERFETTO_DLOG("Converting AST to HeapGraph IR");

  HeapGraphIR ir;

  // Reset diagnostics
  diagnostics_ = ConversionDiagnostics{};

  // Conversion steps with detailed tracking
  ConvertClasses(ast, ir);
  ConvertObjects(ast, ir);
  ConvertReferences(ast, ir);

  // Print detailed diagnostics
  PrintConversionDiagnostics();

  return ir;
}

void HprofAstConverter::ConvertClasses(const HprofAst& ast, HeapGraphIR& ir) {
  PERFETTO_DLOG("Converting classes to IR");

  std::unordered_set<uint64_t> processed_class_ids;

  for (const auto& [class_id, class_info] : ast.classes) {
    diagnostics_.total_processed_classes++;

    // Prevent duplicate class processing
    if (processed_class_ids.count(class_id)) {
      continue;
    }

    processed_class_ids.insert(class_id);
    diagnostics_.unique_classes_processed++;

    // Track class kind
    std::string kind = DetermineClassKind(class_info.name);
    diagnostics_.class_kind_counts[kind]++;

    PERFETTO_DLOG("Converting class: id=%" PRIu64 ", name='%s', kind='%s'",
                  class_id, class_info.name.c_str(), kind.c_str());

    // Create HeapGraphClass and add to IR
    HeapGraphClass hg_class;
    hg_class.name = class_info.name;
    hg_class.class_object_id = class_id;
    hg_class.kind = kind;

    // Add superclass reference if exists
    if (class_info.super_class_id != 0) {
      hg_class.superclass_id = class_info.super_class_id;
      PERFETTO_DLOG("  With superclass: %" PRIu64, class_info.super_class_id);
    }

    ir.classes.push_back(std::move(hg_class));
  }

  PERFETTO_DLOG("Converted %zu classes to IR", ir.classes.size());
}

std::string HprofAstConverter::RootTypeToString(uint8_t root_type) {
  switch (root_type) {
    case HPROF_ROOT_JNI_GLOBAL:
      return "jni_global";
    case HPROF_ROOT_JNI_LOCAL:
      return "jni_local";
    case HPROF_ROOT_JAVA_FRAME:
      return "java_frame";
    case HPROF_ROOT_NATIVE_STACK:
      return "native_stack";
    case HPROF_ROOT_STICKY_CLASS:
      return "sticky_class";
    case HPROF_ROOT_THREAD_BLOCK:
      return "thread_block";
    case HPROF_ROOT_MONITOR_USED:
      return "monitor_used";
    case HPROF_ROOT_THREAD_OBJ:
      return "thread_object";
    case HPROF_ROOT_INTERNED_STRING:
      return "interned_string";
    case HPROF_ROOT_FINALIZING:
      return "finalizing";
    case HPROF_ROOT_DEBUGGER:
      return "debugger";
    case HPROF_ROOT_VM_INTERNAL:
      return "vm_internal";
    case HPROF_ROOT_JNI_MONITOR:
      return "jni_monitor";
    case HPROF_ROOT_UNKNOWN:
    default:
      return "unknown";
  }
}

void HprofAstConverter::ConvertObjects(const HprofAst& ast, HeapGraphIR& ir) {
  PERFETTO_DLOG("Converting objects from AST to IR");

  size_t instance_objects = 0;
  size_t obj_array_objects = 0;
  size_t prim_array_objects = 0;
  size_t root_objects = 0;
  size_t skipped_objects = 0;

  // Track which object IDs have been processed to avoid duplicates
  std::unordered_set<uint64_t> processed_object_ids;

  // First, find or create primitive array class IDs
  // This is a workaround for primitive arrays that don't have proper class IDs
  std::unordered_map<uint8_t, uint64_t> primitive_type_to_class_id;
  uint64_t next_synthetic_class_id =
      UINT64_MAX;  // Use high values for synthetic IDs

  // Create synthetic class objects for primitive arrays
  for (uint8_t primitive_type = TYPE_BOOLEAN; primitive_type <= TYPE_LONG;
       primitive_type++) {
    // Skip TYPE_OBJECT (2) and TYPE_ARRAY (3) if they exist
    if (primitive_type == TYPE_OBJECT || primitive_type == 3) {
      continue;
    }

    // Create a synthetic class ID for this primitive type
    uint64_t synthetic_class_id = next_synthetic_class_id--;
    primitive_type_to_class_id[primitive_type] = synthetic_class_id;

    // Create and add class to IR
    std::string class_name;
    switch (primitive_type) {
      case TYPE_BOOLEAN:
        class_name = "boolean[]";
        break;
      case TYPE_CHAR:
        class_name = "char[]";
        break;
      case TYPE_FLOAT:
        class_name = "float[]";
        break;
      case TYPE_DOUBLE:
        class_name = "double[]";
        break;
      case TYPE_BYTE:
        class_name = "byte[]";
        break;
      case TYPE_SHORT:
        class_name = "short[]";
        break;
      case TYPE_INT:
        class_name = "int[]";
        break;
      case TYPE_LONG:
        class_name = "long[]";
        break;
      default:
        class_name = "unknown[]";
        break;
    }

    HeapGraphClass hg_class;
    hg_class.name = class_name;
    hg_class.class_object_id = synthetic_class_id;
    hg_class.kind = "system";  // Primitive arrays are system classes

    ir.classes.push_back(std::move(hg_class));
    PERFETTO_DLOG("Created synthetic class for primitive type %d: ID=%" PRIu64
                  ", name=%s",
                  primitive_type, synthetic_class_id, class_name.c_str());
  }

  // Process all records in the AST for objects
  for (const auto& record : ast.records) {
    // We're only interested in heap dump records
    if (record.tag != HPROF_HEAP_DUMP &&
        record.tag != HPROF_HEAP_DUMP_SEGMENT) {
      continue;
    }

    // Process heap dump records
    if (std::holds_alternative<HeapDumpData>(record.data)) {
      const auto& heap_dump_data = std::get<HeapDumpData>(record.data);

      for (const auto& sub_record : heap_dump_data.records) {
        uint64_t object_id = 0;
        uint64_t type_id = 0;
        uint32_t ref_set_id = 0;
        int64_t self_size = 0;
        std::optional<std::string> heap_type;
        bool skip_object = false;

        // Process based on record type
        if (sub_record.tag == HPROF_INSTANCE_DUMP &&
            std::holds_alternative<InstanceDumpData>(sub_record.data)) {
          // Handle instance dump
          const auto& instance_data =
              std::get<InstanceDumpData>(sub_record.data);
          object_id = instance_data.object_id;
          type_id = instance_data.class_object_id;

          // Check if class ID exists in the AST classes
          if (type_id == 0 || ast.classes.find(type_id) == ast.classes.end()) {
            skipped_objects++;
            continue;
          }

          self_size =
              static_cast<int64_t>(instance_data.raw_instance_data.size());

          // Set heap type based on heap ID
          switch (instance_data.heap_id) {
            case HPROF_HEAP_APP:
              heap_type = "app";
              break;
            case HPROF_HEAP_ZYGOTE:
              heap_type = "zygote";
              break;
            case HPROF_HEAP_IMAGE:
              heap_type = "image";
              break;
            case HPROF_HEAP_JIT:
              heap_type = "jit";
              break;
            case HPROF_HEAP_APP_CACHE:
              heap_type = "app-cache";
              break;
            case HPROF_HEAP_SYSTEM:
              heap_type = "system";
              break;
            case HPROF_HEAP_DEFAULT:
              heap_type = "default";
              break;
          }

          instance_objects++;
        } else if (sub_record.tag == HPROF_OBJ_ARRAY_DUMP &&
                   std::holds_alternative<ObjArrayDumpData>(sub_record.data)) {
          // Handle object array dump
          const auto& array_data = std::get<ObjArrayDumpData>(sub_record.data);
          object_id = array_data.array_object_id;
          type_id = array_data.array_class_object_id;

          // Check if class ID exists in the AST classes
          if (type_id == 0 || ast.classes.find(type_id) == ast.classes.end()) {
            skipped_objects++;
            continue;
          }

          self_size = static_cast<int64_t>(array_data.elements.size() *
                                           8);  // Approximate size

          // Set heap type based on heap ID
          switch (array_data.heap_id) {
            case HPROF_HEAP_APP:
              heap_type = "app";
              break;
            case HPROF_HEAP_ZYGOTE:
              heap_type = "zygote";
              break;
            case HPROF_HEAP_IMAGE:
              heap_type = "image";
              break;
            case HPROF_HEAP_JIT:
              heap_type = "jit";
              break;
            case HPROF_HEAP_APP_CACHE:
              heap_type = "app-cache";
              break;
            case HPROF_HEAP_SYSTEM:
              heap_type = "system";
              break;
            case HPROF_HEAP_DEFAULT:
              heap_type = "default";
              break;
          }

          obj_array_objects++;
        } else if (sub_record.tag == HPROF_PRIM_ARRAY_DUMP &&
                   std::holds_alternative<PrimArrayDumpData>(sub_record.data)) {
          // Handle primitive array dump
          const auto& prim_array_data =
              std::get<PrimArrayDumpData>(sub_record.data);
          object_id = prim_array_data.array_object_id;

          // For primitive arrays, use our synthetic class ID based on the
          // primitive type
          uint8_t element_type = prim_array_data.element_type;
          auto type_it = primitive_type_to_class_id.find(element_type);
          if (type_it != primitive_type_to_class_id.end()) {
            type_id = type_it->second;
          } else {
            // Unknown primitive type - create a new synthetic class
            uint64_t synthetic_class_id = next_synthetic_class_id--;
            primitive_type_to_class_id[element_type] = synthetic_class_id;
            type_id = synthetic_class_id;

            // Create and add class to IR
            std::string class_name =
                "unknown_primitive_" + std::to_string(element_type) + "[]";

            HeapGraphClass hg_class;
            hg_class.name = class_name;
            hg_class.class_object_id = synthetic_class_id;
            hg_class.kind = "system";

            ir.classes.push_back(std::move(hg_class));
            PERFETTO_DLOG(
                "Created synthetic class for unknown primitive type %d: "
                "ID=%" PRIu64,
                element_type, synthetic_class_id);
          }

          self_size = static_cast<int64_t>(prim_array_data.elements.size());

          // Set heap type based on heap ID
          switch (prim_array_data.heap_id) {
            case HPROF_HEAP_APP:
              heap_type = "app";
              break;
            case HPROF_HEAP_ZYGOTE:
              heap_type = "zygote";
              break;
            case HPROF_HEAP_IMAGE:
              heap_type = "image";
              break;
            case HPROF_HEAP_JIT:
              heap_type = "jit";
              break;
            case HPROF_HEAP_APP_CACHE:
              heap_type = "app-cache";
              break;
            case HPROF_HEAP_SYSTEM:
              heap_type = "system";
              break;
            case HPROF_HEAP_DEFAULT:
              heap_type = "default";
              break;
          }

          prim_array_objects++;
        } else {
          // Skip other record types
          continue;
        }

        // Skip if object ID is 0 or already processed
        if (object_id == 0 || processed_object_ids.count(object_id) > 0) {
          continue;
        }

        // Skip if we're supposed to skip this object (due to missing class ID)
        if (skip_object) {
          continue;
        }

        // Mark as processed
        processed_object_ids.insert(object_id);

        // Create and add object to IR
        HeapGraphObject hg_object;
        hg_object.object_id = object_id;
        hg_object.type_id = type_id;
        hg_object.self_size = self_size;
        hg_object.heap_type = heap_type;

        // Check if this object is a root and add root type
        auto root_it = ast.root_objects.find(object_id);
        if (root_it != ast.root_objects.end()) {
          hg_object.root_type = RootTypeToString(root_it->second);
          root_objects++;

          // Log root objects (limited to avoid spam)
          if (root_objects <= 10 || root_objects % 1000 == 0) {
            PERFETTO_DLOG("Found root object: ID=%" PRIu64 ", type=%s",
                          object_id, hg_object.root_type->c_str());
          }
        }

        // Generate reference set ID
        ref_set_id = next_reference_set_id_++;
        object_to_reference_set_id_[object_id] = ref_set_id;
        hg_object.reference_set_id = ref_set_id;

        // Log sample object conversions
        if (ir.objects.size() < 10 || ir.objects.size() % 10000 == 0) {
          PERFETTO_DLOG("Converting object to IR: ID=%" PRIu64 ", type=%" PRIu64
                        ", size=%" PRId64 "%s",
                        object_id, type_id, self_size,
                        hg_object.root_type.has_value()
                            ? (", root_type=" + *hg_object.root_type).c_str()
                            : "");
        }

        ir.objects.push_back(std::move(hg_object));
      }
    }
  }

  PERFETTO_DLOG(
      "Converted %zu objects to IR (%zu instances, %zu obj arrays, %zu prim "
      "arrays, %zu roots, %zu skipped)",
      ir.objects.size(), instance_objects, obj_array_objects,
      prim_array_objects, root_objects, skipped_objects);
}

void HprofAstConverter::ConvertReferences(const HprofAst& ast,
                                          HeapGraphIR& ir) {
  PERFETTO_DLOG("Converting %zu reference owner-to-owned entries to IR",
                ast.owner_to_owned.size());

  // Track reference conversion statistics
  size_t total_references = 0;
  size_t refs_with_valid_owner = 0;
  size_t refs_with_valid_owned = 0;

  // Keep track of which objects have been added to IR
  std::unordered_set<uint64_t> objects_in_ir;
  for (const auto& obj : ir.objects) {
    objects_in_ir.insert(obj.object_id);
  }

  PERFETTO_DLOG("Found %zu objects in IR", objects_in_ir.size());

  // Process each owner and its references
  for (const auto& [owner_id, owned_list] : ast.owner_to_owned) {
    // Check if owner exists in IR objects
    if (objects_in_ir.find(owner_id) == objects_in_ir.end()) {
      if (total_references < 10 || total_references % 10000 == 0) {
        PERFETTO_DLOG("Owner ID %" PRIu64 " from AST not found in IR objects",
                      owner_id);
      }
      continue;
    }

    refs_with_valid_owner++;

    // Find the reference set ID for the owner
    uint32_t reference_set_id = 0;
    auto ref_set_id_it = object_to_reference_set_id_.find(owner_id);
    if (ref_set_id_it != object_to_reference_set_id_.end()) {
      reference_set_id = ref_set_id_it->second;
    } else {
      PERFETTO_DLOG("No reference set ID found for owner %" PRIu64, owner_id);
      continue;
    }

    // Find if the owner is an array
    bool is_array = false;
    uint64_t class_id = 0;
    auto owner_class_it = ast.object_to_class.find(owner_id);
    if (owner_class_it != ast.object_to_class.end()) {
      class_id = owner_class_it->second;
      auto class_info_it = ast.classes.find(class_id);
      if (class_info_it != ast.classes.end()) {
        // Check if class name indicates an array
        const std::string& class_name = class_info_it->second.name;
        is_array =
            (!class_name.empty() &&
             class_name[0] ==
                 '[') ||  // Handles "[I", "[Ljava.lang.String;" etc.
            (class_name.find("[]") !=
             std::string::npos);  // Handles "int[]", "java.lang.String[]" etc.
      }
    }

    // Process all references from this owner
    for (const auto& owned_ref : owned_list) {
      total_references++;

      HeapGraphReference hg_ref;
      hg_ref.reference_set_id = reference_set_id;
      hg_ref.owner_id = owner_id;

      // Set the field name properly based on whether this is an array or a
      // regular object
      if (is_array) {
        // Keep the array index format for arrays
        hg_ref.field_name = owned_ref.field_name;
      } else {
        // For regular objects, make sure we don't have array index format
        std::string field_name = owned_ref.field_name;
        if (field_name.size() >= 2 && field_name[0] == '[' &&
            field_name[field_name.size() - 1] == ']') {
          // This looks like an array index but the owner is not an array
          // This is probably a bug in the parsing phase
          PERFETTO_DLOG(
              "Warning: Found array index field name '%s' for non-array object "
              "%" PRIu64,
              field_name.c_str(), owner_id);
        }
        hg_ref.field_name = field_name;
      }

      // Set the owned ID if valid
      uint64_t target_id = owned_ref.target_object_id;
      if (target_id != 0) {
        if (objects_in_ir.find(target_id) != objects_in_ir.end()) {
          hg_ref.owned_id = target_id;
          refs_with_valid_owned++;
        } else {
          if (total_references < 10 || total_references % 10000 == 0) {
            PERFETTO_DLOG("Target ID %" PRIu64
                          " from reference not found in IR objects",
                          target_id);
          }
        }
      }

      // Set field type name
      std::string owner_class_name;
      if (class_id != 0) {
        auto class_info_it = ast.classes.find(class_id);
        if (class_info_it != ast.classes.end()) {
          owner_class_name = class_info_it->second.name;
        }
      }

      // Try to determine field type name from owned object
      if (target_id != 0) {
        auto type_it = ast.object_to_class.find(target_id);
        if (type_it != ast.object_to_class.end()) {
          auto class_it = ast.classes.find(type_it->second);
          if (class_it != ast.classes.end()) {
            hg_ref.field_type_name = class_it->second.name;
          }
        }
      }

      // If field type is still empty, use owner class name as fallback
      if (hg_ref.field_type_name.empty() && !owner_class_name.empty()) {
        hg_ref.field_type_name = owner_class_name;
      } else if (hg_ref.field_type_name.empty()) {
        // If still empty, use a default type name
        hg_ref.field_type_name = "java.lang.Object";
      }

      // Add the reference to IR
      ir.references.push_back(std::move(hg_ref));

      // Log sample of references for debugging
      if (total_references < 10 || total_references % 10000 == 0) {
        PERFETTO_DLOG("Added reference: owner=%" PRIu64
                      " (%s), owned=%s, field=%s",
                      owner_id, is_array ? "array" : "object",
                      hg_ref.owned_id.has_value()
                          ? std::to_string(*hg_ref.owned_id).c_str()
                          : "null",
                      hg_ref.field_name.c_str());
      }
    }
  }

  PERFETTO_DLOG(
      "Converted %zu references: %zu with valid owner, %zu with valid owned",
      total_references, refs_with_valid_owner, refs_with_valid_owned);
}

HeapGraphValue HprofAstConverter::ConvertFieldValue(const FieldValue& value) {
  PERFETTO_DLOG("Converting field value of type %d", value.type);

  HeapGraphValue hg_value;

  switch (value.type) {
    case FieldValue::BOOLEAN:
      hg_value.type = HeapGraphValue::BOOLEAN;
      hg_value.bool_value = value.bool_value;
      break;
    case FieldValue::BYTE:
      hg_value.type = HeapGraphValue::BYTE;
      hg_value.byte_value = value.byte_value;
      break;
    case FieldValue::CHAR:
      hg_value.type = HeapGraphValue::CHAR;
      hg_value.char_value = value.char_value;
      break;
    case FieldValue::SHORT:
      hg_value.type = HeapGraphValue::SHORT;
      hg_value.short_value = value.short_value;
      break;
    case FieldValue::INT:
      hg_value.type = HeapGraphValue::INT;
      hg_value.int_value = value.int_value;
      break;
    case FieldValue::FLOAT:
      hg_value.type = HeapGraphValue::FLOAT;
      hg_value.float_value = value.float_value;
      break;
    case FieldValue::LONG:
      hg_value.type = HeapGraphValue::LONG;
      hg_value.long_value = value.long_value;
      break;
    case FieldValue::DOUBLE:
      hg_value.type = HeapGraphValue::DOUBLE;
      hg_value.double_value = value.double_value;
      break;
    case FieldValue::OBJECT_ID:
      hg_value.type = HeapGraphValue::OBJECT_ID;
      hg_value.object_id_value = value.object_id_value;
      break;
    case FieldValue::NONE:
      hg_value.type = HeapGraphValue::NONE;
      break;
  }

  return hg_value;
}

std::string HprofAstConverter::DetermineClassKind(
    const std::string& class_name) const {
  PERFETTO_DLOG("Determining class kind for: %s", class_name.c_str());

  // Refined kind determination
  if (class_name.find("java.lang.") == 0)
    return "system";
  if (class_name.find("java.util.") == 0)
    return "system";
  if (class_name.find("java.concurrent.") == 0)
    return "system";
  if (class_name.find("jdk.internal.") == 0)
    return "system";
  if (class_name.find("sun.") == 0)
    return "system";
  if (class_name.find("com.sun.") == 0)
    return "system";
  if (class_name.find("android.") == 0)
    return "framework";
  if (class_name.find("com.android.") == 0)
    return "framework";
  if (class_name.find("androidx.") == 0)
    return "framework";

  return "app";
}

void HprofAstConverter::PrintConversionDiagnostics() {
  PERFETTO_DLOG("\nConversion Diagnostics:");
  PERFETTO_DLOG("----------------------");

  PERFETTO_DLOG("Total Classes Processed: %zu",
                diagnostics_.total_processed_classes);
  PERFETTO_DLOG("Unique Classes Processed: %zu",
                diagnostics_.unique_classes_processed);

  PERFETTO_DLOG("\nClass Kind Distribution:");
  for (const auto& [kind, count] : diagnostics_.class_kind_counts) {
    PERFETTO_DLOG("  %s: %zu", kind.c_str(), count);
  }

  PERFETTO_DLOG("\nSuperclass Chain Lengths:");
  for (const auto& [length, count] : diagnostics_.superclass_chain_lengths) {
    PERFETTO_DLOG("  %s: %zu", length.c_str(), count);
  }

  PERFETTO_DLOG("\nReferences:");
  PERFETTO_DLOG("  Generated References: %zu",
                diagnostics_.references_generated);
}

// TraceBlobViewIterator implementation
ArtHprofTokenizer::TraceBlobViewIterator::TraceBlobViewIterator(
    util::TraceBlobViewReader&& reader)
    : reader_(std::move(reader)) {}

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

std::streampos ArtHprofTokenizer::TraceBlobViewIterator::GetPosition() {
  return std::streampos(static_cast<std::streamoff>(current_offset_));
}

bool ArtHprofTokenizer::TraceBlobViewIterator::IsEof() const {
  return !reader_.SliceOff(current_offset_, 1);
}

bool ArtHprofTokenizer::TraceBlobViewIterator::IsValid() const {
  return true;
}

// ArtHprofTokenizer implementation
ArtHprofTokenizer::ArtHprofTokenizer(TraceProcessorContext* ctx)
    : context_(ctx) {}

ArtHprofTokenizer::~ArtHprofTokenizer() = default;

base::Status ArtHprofTokenizer::Parse(TraceBlobView blob) {
  reader_.PushBack(std::move(blob));

  if (std::holds_alternative<Detect>(sub_parser_)) {
    return std::get<Detect>(sub_parser_).Parse();
  } else if (std::holds_alternative<Streaming>(sub_parser_)) {
    return std::get<Streaming>(sub_parser_).Parse();
  } else {
    return std::get<NonStreaming>(sub_parser_).Parse();
  }
}

base::Status ArtHprofTokenizer::InitializeParserIfNeeded() {
  if (is_initialized_)
    return base::OkStatus();

  // Create the ByteIterator that wraps our reader
  byte_iterator_ = std::make_unique<TraceBlobViewIterator>(std::move(reader_));

  // Initialize the parser with our iterator
  parser_ = std::make_unique<HprofParser>(byte_iterator_.get());

  is_initialized_ = true;
  return base::OkStatus();
}

base::Status ArtHprofTokenizer::ProcessParsingResults() {
  // Initialize parser if needed
  auto status = InitializeParserIfNeeded();
  if (!status.ok())
    return status;

  if (!parser_result_) {
    parser_result_ = parser_->Parse();
  }

  if (!parser_result_) {
    return base::ErrStatus("Parsing failed");
  }

  if (parser_result_ && !ir_) {
    // Convert AST to IR
    converter_ = std::make_unique<HprofAstConverter>();
    ir_ = converter_->ConvertToIR(*parser_result_);

    // Check if IR conversion was successful
    if (!ir_) {
      return base::ErrStatus("Failed to convert AST to IR");
    }

    // Log some information about the IR to help diagnose issues
    PERFETTO_DLOG("IR contains %zu classes, %zu objects, %zu references",
                  ir_->classes.size(), ir_->objects.size(),
                  ir_->references.size());

    // Create and push the event
    const ArtHprofEvent event(*ir_);
    context_->sorter->PushArtHprofEvent(0, event);
  }

  return base::OkStatus();
}

base::Status ArtHprofTokenizer::NotifyEndOfFile() {
  is_complete_ = true;

  if (std::holds_alternative<Detect>(sub_parser_)) {
    return std::get<Detect>(sub_parser_).NotifyEndOfFile();
  } else if (std::holds_alternative<Streaming>(sub_parser_)) {
    return std::get<Streaming>(sub_parser_).NotifyEndOfFile();
  } else {
    return std::get<NonStreaming>(sub_parser_).NotifyEndOfFile();
  }
}

// Detect implementation
base::Status ArtHprofTokenizer::Detect::Parse() {
  auto it = tokenizer_->reader_.GetIterator();

  // Try to read the magic number to detect format
  auto header = it.MaybeRead(4);
  if (!header) {
    return base::OkStatus();  // Not enough data yet
  }

  uint32_t magic = 0;
  memcpy(&magic, header->data(), 4);

  // Check the endianness and set the correct parser
  if (magic == kHprofHeaderMagic) {
    tokenizer_->sub_parser_ = NonStreaming{tokenizer_};
    return std::get<NonStreaming>(tokenizer_->sub_parser_).Parse();
  } else {
    // Try to read as streaming format or use another detection method
    tokenizer_->sub_parser_ = Streaming{tokenizer_};
    return std::get<Streaming>(tokenizer_->sub_parser_).Parse();
  }
}

base::Status ArtHprofTokenizer::Detect::NotifyEndOfFile() const {
  return base::ErrStatus("HPROF format detection incomplete");
}

// NonStreaming implementation
base::Status ArtHprofTokenizer::NonStreaming::Parse() {
  if (is_parsing_)
    return base::OkStatus();  // Already parsing

  is_parsing_ = true;

  auto status = tokenizer_->ProcessParsingResults();
  if (!status.ok())
    return status;

  is_parsing_ = false;
  return base::OkStatus();
}

base::Status ArtHprofTokenizer::NonStreaming::NotifyEndOfFile() const {
  return tokenizer_->ProcessParsingResults();
}

// Streaming implementation
base::Status ArtHprofTokenizer::Streaming::Parse() {
  // In streaming mode, we might need to handle chunked data
  if (!header_parsed_) {
    auto it = tokenizer_->reader_.GetIterator();
    PERFETTO_CHECK(it.MaybeAdvance(it_offset_));

    // Read and validate header
    auto header = it.MaybeRead(kHprofHeaderLength);
    if (!header) {
      return base::OkStatus();  // Not enough data yet
    }

    // Process header and update state
    header_parsed_ = true;
    it_offset_ = it.file_offset();
  }

  // Process the rest of the data
  return tokenizer_->ProcessParsingResults();
}

base::Status ArtHprofTokenizer::Streaming::NotifyEndOfFile() {
  return tokenizer_->ProcessParsingResults();
}

}  // namespace perfetto::trace_processor::art_hprof
