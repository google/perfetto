#include "src/trace_processor/importers/art_hprof/art_hprof_tokenizer.h"
#include "src/trace_processor/sorter/trace_sorter.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <string>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <iomanip>
#include <optional>

namespace perfetto::trace_processor::art_hprof {
ByteIterator::~ByteIterator() {};

// AndroidHeapStats implementation
void HprofAst::AndroidHeapStats::AddObject(size_t size) {
    std::cout << "Adding object of size " << size << " to heap stats" << std::endl;
      object_count++;
      total_bytes += size;
}

HprofParser::~HprofParser() {
    //delete byte_iterator_;
}

HprofAst HprofParser::Parse() {
    std::cout << "Beginning to parse HPROF";

    if (!ParseHeader()) {
      PERFETTO_FATAL("Failed to parse HPROF header");
    }

    ParseRecords();

    // Post-processing: detect String classes and mark them
    if (detect_string_class_) {
      std::cout << "Post-processing: Detecting String classes" << std::endl;

      for (auto& [class_id, class_info] : ast_.classes) {
        if (IsStringClass(class_info.name)) {
          class_info.is_string_class = true;
          std::cout << "Detected String class: " << class_info.name << std::endl;

          // Check for count field which could indicate string compression
          for (const auto& field : class_info.fields) {
            if (field.name == "count" && field.type == TYPE_INT) {
              ast_.use_string_compression = true;
              class_info.use_string_compression = true;
              std::cout << "Detected string compression in: " << class_info.name << std::endl;
              break;
            }
          }
        }
      }
    }

    // Summary statistics
    std::cout << "\nParsing Summary:" << std::endl;
    std::cout << "---------------" << std::endl;
    std::cout << "String count: " << ast_.string_count << std::endl;
    std::cout << "Class count: " << ast_.class_count << std::endl;
    std::cout << "Heap dump count: " << ast_.heap_dump_count << std::endl;
    std::cout << "Class instance count: " << ast_.class_instance_count << std::endl;
    std::cout << "Object array count: " << ast_.object_array_count << std::endl;
    std::cout << "Primitive array count: " << ast_.primitive_array_count << std::endl;
    std::cout << "Root count: " << ast_.root_count << std::endl;
    std::cout << "Field reference count: " << ast_.field_reference_count << std::endl;
    std::cout << "Heap info count: " << ast_.heap_info_count << std::endl;

    return ast_;
}

bool HprofParser::IsStringClass(const std::string& class_name) const {
    std::cout << "Checking if class is String: " << class_name << std::endl;
    return class_name == "java.lang.String" ||
           class_name == "java/lang/String" ||
           class_name == "Ljava/lang/String;";
}

size_t HprofParser::GetFieldTypeSize(uint8_t type) const {
    std::cout << "Getting size for field type: " << static_cast<int>(type) << std::endl;
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

int8_t HprofParser::ReadByteValue(const std::vector<uint8_t>& data, size_t offset) const {
  if (offset < data.size()) {
    return static_cast<int8_t>(data[offset]);
  }
  return 0;
}

bool HprofParser::ReadBooleanValue(const std::vector<uint8_t>& data, size_t offset) const {
  if (offset < data.size()) {
    return data[offset] != 0;
  }
  return false;
}

int16_t HprofParser::ReadShortValue(const std::vector<uint8_t>& data, size_t offset) const {
  if (offset + 1 < data.size()) {
    // Assuming big-endian byte order for HPROF files
    return static_cast<int16_t>((static_cast<int16_t>(data[offset]) << 8) |
                            static_cast<int16_t>(data[offset + 1]));
  }
  return 0;
}

char16_t HprofParser::ReadCharValue(const std::vector<uint8_t>& data, size_t offset) const {
  if (offset + 1 < data.size()) {
    // Assuming big-endian byte order for HPROF files
      return static_cast<char16_t>((static_cast<char16_t>(data[offset]) << 8) |
                                   static_cast<char16_t>(data[offset + 1]));
  }
  return 0;
}

int32_t HprofParser::ReadIntValue(const std::vector<uint8_t>& data, size_t offset) const {
  if (offset + 3 < data.size()) {
    // Assuming big-endian byte order for HPROF files
    return (static_cast<int32_t>(data[offset]) << 24) |
           (static_cast<int32_t>(data[offset + 1]) << 16) |
           (static_cast<int32_t>(data[offset + 2]) << 8) |
           data[offset + 3];
  }
  return 0;
}

float HprofParser::ReadFloatValue(const std::vector<uint8_t>& data, size_t offset) const {
  if (offset + 3 < data.size()) {
      int32_t int_value = ReadIntValue(data, offset);
      float result;
      std::memcpy(&result, &int_value, sizeof(float));
      return result;
  }
  return 0.0f;
}

int64_t HprofParser::ReadLongValue(const std::vector<uint8_t>& data, size_t offset) const {
  if (offset + 7 < data.size()) {
    // Assuming big-endian byte order for HPROF files
    return (static_cast<int64_t>(data[offset]) << 56) |
           (static_cast<int64_t>(data[offset + 1]) << 48) |
           (static_cast<int64_t>(data[offset + 2]) << 40) |
           (static_cast<int64_t>(data[offset + 3]) << 32) |
           (static_cast<int64_t>(data[offset + 4]) << 24) |
           (static_cast<int64_t>(data[offset + 5]) << 16) |
           (static_cast<int64_t>(data[offset + 6]) << 8) |
           data[offset + 7];
  }
  return 0;
}

double HprofParser::ReadDoubleValue(const std::vector<uint8_t>& data, size_t offset) const {
  if (offset + 7 < data.size()) {
      int64_t long_value = ReadLongValue(data, offset);
      double result;
      std::memcpy(&result, &long_value, sizeof(double));
      return result;
  }
  return 0.0;
}

uint64_t HprofParser::ReadObjectIDValue(const std::vector<uint8_t>& data, size_t offset, uint32_t id_size) const {
  if (id_size == 4 && offset + 3 < data.size()) {
    return static_cast<uint64_t>(ReadIntValue(data, offset));
  } else if (id_size == 8 && offset + 7 < data.size()) {
    return static_cast<uint64_t>(ReadLongValue(data, offset));
  }
  return 0;
}

FieldValue HprofParser::ExtractFieldValue(const std::vector<uint8_t>& data, size_t offset, uint8_t field_type) {
  std::cout << "Extracting field of type " << static_cast<int>(field_type)
            << " at offset " << offset << std::endl;

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
      std::cerr << "Unknown field type: " << static_cast<int>(field_type) << std::endl;
      return FieldValue();
  }
}

void HprofParser::ExtractInstanceFields(InstanceDumpData& instance_data, const ClassInfo& class_info) {
  std::cout << "Extracting fields for instance of class: " << class_info.name << std::endl;

  // Calculate field offsets and extract values
  size_t offset = 0;
  for (const auto& field_info : class_info.fields) {
    // Skip if we've reached the end of data
    if (offset >= instance_data.raw_instance_data.size()) {
      std::cerr << "Warning: Reached end of instance data while processing fields" << std::endl;
      break;
    }

    // Extract field value
    FieldValue value = ExtractFieldValue(instance_data.raw_instance_data, offset, field_info.type);

    // Create field value record
    FieldValueRecord record;
    record.field_name = field_info.name;
    record.value = value;
    instance_data.field_values.push_back(record);

    // For object references, add to references collection
    if (field_info.type == TYPE_OBJECT && value.type == FieldValue::OBJECT_ID && value.object_id_value != 0) {
      ObjectReference ref;
      ref.field_name = field_info.name;
      ref.target_object_id = value.object_id_value;

      std::cout << "Found reference in field '" << field_info.name
                << "' to object " << ref.target_object_id << std::endl;

      // Add to instance references
      instance_data.references.push_back(ref);

      // Add to global owner-to-owned map
      ast_.owner_to_owned[instance_data.object_id].push_back(ref);
    }

    // Move to next field
    offset += GetFieldTypeSize(field_info.type);
  }

  std::cout << "Extracted " << instance_data.field_values.size() << " field values and "
            << instance_data.references.size() << " references" << std::endl;
}

void HprofParser::ExtractStringInstance(InstanceDumpData& instance_data, const ClassInfo& class_info) {
  if (!class_info.is_string_class) {
    return;
  }

  std::cout << "Attempting to extract string value from String instance" << std::endl;

  // Find the "value" field which contains the char array reference
  uint64_t char_array_id = 0;
  for (const auto& field_value : instance_data.field_values) {
    if ((field_value.field_name == "value" || field_value.field_name == "chars") &&
        field_value.value.type == FieldValue::OBJECT_ID) {
      char_array_id = field_value.value.object_id_value;
      break;
    }
  }

  if (char_array_id == 0) {
    std::cout << "String value field not found or null" << std::endl;
    return;
  }

  // In a real implementation, you would find the char array and extract its value
  // For this skeleton, we'll just note that we found a string reference
  std::cout << "Found string value array at object ID: " << char_array_id << std::endl;

  // Add special reference for string value array
  ObjectReference ref;
  ref.field_name = "stringValue";
  ref.target_object_id = char_array_id;
  instance_data.references.push_back(ref);
  ast_.owner_to_owned[instance_data.object_id].push_back(ref);

}

void HprofParser::UpdateHeapStats(HprofHeapId heap_id, size_t object_size) {
    std::cout << "Updating heap stats for heap ID: " << static_cast<int>(heap_id)
              << " with size: " << object_size << std::endl;
    ast_.android_heap_stats[heap_id].AddObject(object_size);

}

void HprofParser::SkipUnknownSubRecord(uint8_t sub_tag, [[maybe_unused]] std::streampos end_pos) {
    std::cout << "Skipping unknown sub-record with tag: 0x"
              << std::hex << static_cast<int>(sub_tag) << std::dec << std::endl;

    // Simple root records with just an object ID
    if (sub_tag >= 0x01 && sub_tag <= 0x0a) {
      byte_iterator_->SkipBytes(identifier_size_);
    } else {
      // For other unknown tags, just skip a byte
      byte_iterator_->SkipBytes(1);
    }
}

bool HprofParser::ParseHeader() {
    std::cout << "Parsing HPROF header" << std::endl;

    // Read format string until null terminator
    char c;
    ast_.header.format = "";
    while (byte_iterator_->ReadU1(reinterpret_cast<uint8_t&>(c)) && c != 0) {
      ast_.header.format.push_back(c);
    }

    // Read ID size
    if (!byte_iterator_->ReadU4(ast_.header.identifier_size)) {
      std::cerr << "Error: Failed to read ID size" << std::endl;
      return false;
    }

    identifier_size_ = ast_.header.identifier_size;

    // Read timestamp
    uint32_t high_time, low_time;
    if (!byte_iterator_->ReadU4(high_time) || !byte_iterator_->ReadU4(low_time)) {
      std::cerr << "Error: Failed to read timestamp" << std::endl;
      return false;
    }

    ast_.header.timestamp = (static_cast<uint64_t>(high_time) << 32) | low_time;

    std::cout << "Read HPROF header: format=" << ast_.header.format
              << ", idSize=" << identifier_size_ << std::endl;
    return true;
}

void HprofParser::ParseRecords() {
    std::cout << "Beginning to parse records" << std::endl;

    while (byte_iterator_->IsValid() && !byte_iterator_->IsEof()) {
      // Try to read the tag
      uint8_t tag;
      if (!byte_iterator_->ReadU1(tag)) {
        if (byte_iterator_->IsEof()) break;
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

    std::cout << "Finished parsing records" << std::endl;
}

void HprofParser::ParseRecord(uint8_t tag, uint32_t time, uint32_t length) {
    std::cout << "Parsing record with tag: 0x" << std::hex << static_cast<int>(tag)
              << std::dec << ", time: " << time << ", length: " << length << std::endl;

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
        std::cout << "Encountered HEAP_DUMP_END tag" << std::endl;
        record.data = std::monostate{};
        ast_.records.push_back(record);
        break;
      default:
        // Generic record - skip the payload
        std::cout << "Skipping unknown record payload of length " << length << std::endl;
        byte_iterator_->SkipBytes(length);
        record.data = std::monostate{};
        ast_.records.push_back(record);
        break;
    }
}

void HprofParser::ParseUtf8Record(HprofRecord& record) {
    std::cout << "Parsing UTF8 record" << std::endl;

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

    std::cout << "Read UTF8 string: ID=" << name_id
              << ", string='" << data.utf8_string << "'" << std::endl;

    record.data = data;
    ast_.records.push_back(record);

    // Store string for later reference
    ast_.id_to_string_map[data.name_id] = data.utf8_string;
    ast_.string_count++;
}

void HprofParser::ParseLoadClassRecord(HprofRecord& record) {
    std::cout << "Parsing LOAD_CLASS record" << std::endl;

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

      std::cout << "Class loaded: serial=" << data.class_serial_num
                << ", id=" << data.class_object_id
                << ", name='" << data.class_name << "'" << std::endl;

      // Store class info
      ClassInfo& class_info = ast_.classes[data.class_object_id];
      class_info.name = data.class_name;
      class_info.class_object_id = data.class_object_id;
      class_info.is_string_class = IsStringClass(data.class_name);
    } else {
      std::cout << "Class loaded but name not resolved: serial=" << data.class_serial_num
                << ", id=" << data.class_object_id << std::endl;
    }

    record.data = data;
    ast_.records.push_back(record);
    ast_.class_serial_to_id[data.class_serial_num] = data.class_object_id;
    ast_.class_count++;
}

void HprofParser::ParseHeapDumpRecord(HprofRecord& record) {
    std::cout << "Parsing HEAP_DUMP or HEAP_DUMP_SEGMENT record" << std::endl;

    HeapDumpData data;

    // Record the end position
    std::streampos end_pos = byte_iterator_->GetPosition();
    end_pos += record.length;

    // Parse heap dump sub-records
    while (byte_iterator_->GetPosition() < end_pos) {
      uint8_t sub_tag;
      if (!byte_iterator_->ReadU1(sub_tag)) {
        if (byte_iterator_->IsEof()) break;
        PERFETTO_FATAL("Failed to read heap dump sub-record tag");
      }

      std::cout << "Parsing heap sub-record with tag: 0x"
                << std::hex << static_cast<int>(sub_tag) << std::dec << std::endl;

      // Try to parse the sub-record, continue even if it fails
      if (!ParseHeapSubRecord(sub_tag, data.records)) {
        // Skip to the next sub-record based on tag type
        SkipUnknownSubRecord(sub_tag, end_pos);
      }

      // Safety check: if we've gone past the end position or hit EOF, break
      if (byte_iterator_->GetPosition() >= end_pos || byte_iterator_->IsEof()) break;
    }

    record.data = data;
    ast_.records.push_back(record);
    ast_.heap_dump_count++;
}

bool HprofParser::ParseHeapSubRecord(uint8_t sub_tag, std::vector<HprofHeapRecord>& sub_records) {
    HprofHeapRecord record;
    record.tag = static_cast<HprofHeapTag>(sub_tag);

      switch (sub_tag) {
        case HPROF_ROOT_JNI_GLOBAL:
          ParseRootJniGlobal(record);
          break;
        case HPROF_ROOT_JNI_LOCAL:
        case HPROF_ROOT_JAVA_FRAME:
        case HPROF_ROOT_THREAD_BLOCK:
          ParseRootWithThread(record);
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
          break;
        case HPROF_ROOT_THREAD_OBJ:
          ParseThreadObjectRoot(record);
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
          std::cerr << "Warning: Unknown heap dump sub-tag: 0x"
                    << std::hex << static_cast<int>(sub_tag) << std::dec << std::endl;
          return false; // Skip this sub-record but continue parsing
      }

      sub_records.push_back(record);
      return true;

}

void HprofParser::ParseRootJniGlobal(HprofHeapRecord& record) {
    std::cout << "Parsing JNI GLOBAL root" << std::endl;

    RootRecordData data;
    data.root_type = record.tag;

    uint64_t global_ref_id; // Temporary variable for the second ID
    if (!byte_iterator_->ReadId(data.object_id, identifier_size_) ||
        !byte_iterator_->ReadId(global_ref_id, identifier_size_)) {
      PERFETTO_FATAL("Failed to read JNI GLOBAL root");
    }

    std::cout << "JNI GLOBAL root: objectID=" << data.object_id
              << ", globalRefID=" << global_ref_id << std::endl;

    record.data = data;
    ast_.root_count++;
}

void HprofParser::ParseRootWithThread(HprofHeapRecord& record) {
    std::cout << "Parsing thread-related root" << std::endl;

    RootRecordData data;
    data.root_type = record.tag;

    if (!byte_iterator_->ReadId(data.object_id, identifier_size_) ||
        !byte_iterator_->ReadU4(data.thread_id) ||
        !byte_iterator_->ReadU4(data.frame_number)) {
      PERFETTO_FATAL("Failed to read frame root");
    }

    std::cout << "Thread-related root: objectID=" << data.object_id
              << ", threadID=" << data.thread_id
              << ", frameNumber=" << data.frame_number << std::endl;

    record.data = data;
    ast_.root_count++;
}

void HprofParser::ParseSimpleRoot(HprofHeapRecord& record) {
    std::cout << "Parsing simple root of type 0x"
              << std::hex << static_cast<int>(record.tag) << std::dec << std::endl;

    RootRecordData data;
    data.root_type = record.tag;

    if (!byte_iterator_->ReadId(data.object_id, identifier_size_)) {
      PERFETTO_FATAL("Failed to read simple root");
    }

    std::cout << "Simple root: objectID=" << data.object_id << std::endl;

    record.data = data;
    ast_.root_count++;
}

void HprofParser::ParseThreadObjectRoot(HprofHeapRecord& record) {
    std::cout << "Parsing thread object root" << std::endl;

    RootRecordData data;
    data.root_type = record.tag;

    if (!byte_iterator_->ReadId(data.object_id, identifier_size_) ||
        !byte_iterator_->ReadU4(data.thread_id) ||
        !byte_iterator_->ReadU4(data.frame_number)) {
      PERFETTO_FATAL("Failed to read thread object root");
    }

    std::cout << "Thread object root: objectID=" << data.object_id
              << ", threadID=" << data.thread_id
              << ", stackTraceSerial=" << data.frame_number << std::endl;

    record.data = data;
    ast_.root_count++;
}

void HprofParser::ParseHeapDumpInfo(HprofHeapRecord& record) {
    std::cout << "Parsing heap dump info" << std::endl;

    HeapDumpInfoData data;

    if (!byte_iterator_->ReadU4(data.heap_id) ||
        !byte_iterator_->ReadId(data.heap_name_string_id, identifier_size_)) {
      PERFETTO_FATAL("Failed to read heap dump info");
    }

    auto name_it = ast_.id_to_string_map.find(data.heap_name_string_id);
    if (name_it != ast_.id_to_string_map.end()) {
      data.heap_name = name_it->second;
    }

    std::cout << "Heap dump info: heapID=" << data.heap_id
              << ", heapName='" << data.heap_name << "'" << std::endl;

    // Set current heap for subsequent objects
    current_heap_ = static_cast<HprofHeapId>(data.heap_id);

    record.data = data;
    ast_.heap_info_count++;
}

void HprofParser::ParseClassDump(HprofHeapRecord& record) {
    std::cout << "Parsing class dump" << std::endl;

    ClassDumpData data;

    uint64_t reserved1, reserved2; // Temporary variables for reserved fields
    if (!byte_iterator_->ReadId(data.class_object_id, identifier_size_) ||
        !byte_iterator_->ReadU4(data.stack_trace_serial_num) ||
        !byte_iterator_->ReadId(data.super_class_object_id, identifier_size_) ||
        !byte_iterator_->ReadId(data.class_loader_object_id, identifier_size_) ||
        !byte_iterator_->ReadId(data.signers_object_id, identifier_size_) ||
        !byte_iterator_->ReadId(data.protection_domain_object_id, identifier_size_) ||
        !byte_iterator_->ReadId(reserved1, identifier_size_) ||
        !byte_iterator_->ReadId(reserved2, identifier_size_) ||
        !byte_iterator_->ReadU4(data.instance_size)) {
      PERFETTO_FATAL("Failed to read class dump header");
    }

    std::cout << "Class dump: classID=" << data.class_object_id
              << ", superClassID=" << data.super_class_object_id
              << ", instanceSize=" << data.instance_size << std::endl;

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

    std::cout << "Constant pool size: " << constant_pool_size << std::endl;

    for (uint16_t i = 0; i < constant_pool_size; i++) {
      uint16_t index;
      uint8_t type;
      if (!byte_iterator_->ReadU2(index) || !byte_iterator_->ReadU1(type)) {
        PERFETTO_FATAL("Failed to read constant pool entry");
      }

      // Skip value based on type
      size_t type_size = GetFieldTypeSize(type);
      std::cout << "Skipping constant pool entry: index=" << index
                << ", type=" << static_cast<int>(type)
                << ", size=" << type_size << std::endl;

      if (!byte_iterator_->SkipBytes(type_size)) {
        PERFETTO_FATAL("Failed to skip constant pool value");
      }
    }

    // Read static fields
    uint16_t static_field_count;
    if (!byte_iterator_->ReadU2(static_field_count)) {
      PERFETTO_FATAL("Failed to read static field count");
    }

    std::cout << "Static field count: " << static_field_count << std::endl;

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
      }

      std::cout << "Static field: name='" << field.name
                << "', type=" << static_cast<int>(type) << std::endl;

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

    std::cout << "Instance field count: " << instance_field_count << std::endl;

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
      }

      std::cout << "Instance field: name='" << field.name
                << "', type=" << static_cast<int>(type) << std::endl;

      data.instance_fields.push_back(field);

      // Add field to class info
      class_info.fields.push_back(field);

      // Track reference fields
      if (type == TYPE_OBJECT) {
        ast_.field_reference_count++;
      }
    }

    record.data = data;
}

void HprofParser::ParseInstanceDump(HprofHeapRecord& record) {
  std::cout << "Parsing instance dump" << std::endl;

  InstanceDumpData data;
  uint32_t data_length;

  if (!byte_iterator_->ReadId(data.object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(data.stack_trace_serial_num) ||
      !byte_iterator_->ReadId(data.class_object_id, identifier_size_) ||
      !byte_iterator_->ReadU4(data_length)) {
    PERFETTO_FATAL("Failed to read instance dump header");
  }

  std::cout << "Instance dump: objectID=" << data.object_id
            << ", classID=" << data.class_object_id
            << ", dataLength=" << data_length << std::endl;

  // Set heap ID (from current heap context)
  data.heap_id = current_heap_;

  // Store object to class mapping
  ast_.object_to_class[data.object_id] = data.class_object_id;

  // Read instance data
  if (!byte_iterator_->ReadBytes(data.raw_instance_data, data_length)) {
    PERFETTO_FATAL("Failed to read instance data");
  }

  std::cout << "Read " << data_length << " bytes of instance data" << std::endl;

  // Process fields if we have class info
  auto class_it = ast_.classes.find(data.class_object_id);
  if (class_it != ast_.classes.end()) {
    const ClassInfo& class_info = class_it->second;
    bool is_string_instance = class_info.is_string_class;
    //bool use_string_compression = class_info.use_string_compression;

    std::cout << "Processing fields for class: " << class_info.name
              << (is_string_instance ? " (String class)" : "") << std::endl;

    // After ExtractInstanceFields in ParseInstanceDump:
    if (is_string_instance) {
        ExtractStringInstance(data, class_info);
    }

    // Extract and process all instance fields
    ExtractInstanceFields(data, class_info);

    // Update heap statistics
    UpdateHeapStats(current_heap_, data_length);
  } else {
    std::cout << "Warning: Class info not found for class ID: " << data.class_object_id << std::endl;
  }

  record.data = data;
  ast_.class_instance_count++;
}

void HprofParser::ParseObjectArrayDump(HprofHeapRecord& record) {
    std::cout << "Parsing object array dump" << std::endl;

    ObjArrayDumpData data;
    uint32_t size;

    if (!byte_iterator_->ReadId(data.array_object_id, identifier_size_) ||
        !byte_iterator_->ReadU4(data.stack_trace_serial_num) ||
        !byte_iterator_->ReadU4(size) ||
        !byte_iterator_->ReadId(data.array_class_object_id, identifier_size_)) {
      PERFETTO_FATAL("Failed to read object array dump header");
    }

    std::cout << "Object array: objectID=" << data.array_object_id
              << ", classID=" << data.array_class_object_id
              << ", size=" << size << std::endl;

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

      // Store reference from array to element (in a full implementation)
      if (element_id != 0) { // Ignore null references
        ObjectReference ref;
        ref.field_name = "[" + std::to_string(i) + "]";
        ref.target_object_id = element_id;
        ast_.owner_to_owned[data.array_object_id].push_back(ref);
      }
    }

    std::cout << "Read " << size << " array elements" << std::endl;

    // Update heap statistics
    UpdateHeapStats(current_heap_, size * identifier_size_);

    record.data = data;
    ast_.object_array_count++;
}

void HprofParser::ParsePrimitiveArrayDump(HprofHeapRecord& record) {
    std::cout << "Parsing primitive array dump" << std::endl;

    PrimArrayDumpData data;
    uint32_t size;

    if (!byte_iterator_->ReadId(data.array_object_id, identifier_size_) ||
        !byte_iterator_->ReadU4(data.stack_trace_serial_num) ||
        !byte_iterator_->ReadU4(size) ||
        !byte_iterator_->ReadU1(data.element_type)) {
      PERFETTO_FATAL("Failed to read primitive array dump header");
    }

    std::cout << "Primitive array: objectID=" << data.array_object_id
              << ", type=" << static_cast<int>(data.element_type)
              << ", size=" << size << std::endl;

    // Set heap ID (from current heap context)
    data.heap_id = current_heap_;

    // Determine element size and read data
    size_t element_size = GetFieldTypeSize(data.element_type);
    size_t bytes_to_read = size * element_size;

    std::cout << "Reading " << bytes_to_read << " bytes of array data" << std::endl;

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
    std::cout << "Converting AST to HeapGraph IR" << std::endl;

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
    std::cout << "Converting classes to IR" << std::endl;

    std::unordered_set<uint64_t> processed_class_ids;

    for (const auto& [class_id, class_info] : ast.classes) {
      diagnostics_.total_processed_classes++;

      // Prevent duplicate class processing
      if (processed_class_ids.count(class_id)) continue;
      processed_class_ids.insert(class_id);
      diagnostics_.unique_classes_processed++;

      // Track class kind
      std::string kind = DetermineClassKind(class_info.name);
      diagnostics_.class_kind_counts[kind]++;

      std::cout << "Converting class: id=" << class_id
                << ", name='" << class_info.name
                << "', kind='" << kind << "'" << std::endl;

      // Create HeapGraphClass and add to IR
      HeapGraphClass hg_class;
      hg_class.name = class_info.name;
      hg_class.class_object_id = class_id;
      hg_class.kind = kind;

      // Add superclass reference if exists
      if (class_info.super_class_id != 0) {
        hg_class.superclass_id = class_info.super_class_id;
        std::cout << "  With superclass: " << class_info.super_class_id << std::endl;
      }

      ir.classes.push_back(std::move(hg_class));
    }

    std::cout << "Converted " << ir.classes.size() << " classes to IR" << std::endl;
}

void HprofAstConverter::ConvertObjects(const HprofAst& ast, HeapGraphIR& ir) {
    std::cout << "Converting objects to IR" << std::endl;

    // Process all records in the AST for objects
    size_t converted_objects = 0;

    for (const auto& record : ast.records) {
      // We're only interested in heap dump records
      if (record.tag != HPROF_HEAP_DUMP && record.tag != HPROF_HEAP_DUMP_SEGMENT) {
        continue;
      }

      // Process heap dump records
      if (std::holds_alternative<HeapDumpData>(record.data)) {
        const auto& heap_dump_data = std::get<HeapDumpData>(record.data);

        for (const auto& sub_record : heap_dump_data.records) {
          // We're only interested in instance dumps
          if (sub_record.tag != HPROF_INSTANCE_DUMP) {
            continue;
          }

          // Process instance dump
          const auto& instance_data = std::get<InstanceDumpData>(sub_record.data);

          std::cout << "Converting instance: objectID=" << instance_data.object_id
                    << ", classID=" << instance_data.class_object_id << std::endl;

          HeapGraphObject hg_object;
          hg_object.object_id = instance_data.object_id;
          hg_object.type_id = instance_data.class_object_id;

          // Generate a reference set ID for this object
          uint32_t ref_set_id = next_reference_set_id_++;
          object_to_reference_set_id_[instance_data.object_id] = ref_set_id;
          hg_object.reference_set_id = ref_set_id;

          // Find class info to get instance size
          auto class_it = ast.classes.find(instance_data.class_object_id);
          if (class_it != ast.classes.end()) {
            hg_object.self_size = class_it->second.instance_size;
          }

          // Set heap type based on heap ID
          switch (instance_data.heap_id) {
            case HPROF_HEAP_ZYGOTE:
              hg_object.heap_type = "zygote";
              break;
            case HPROF_HEAP_APP:
              hg_object.heap_type = "app";
              break;
            case HPROF_HEAP_IMAGE:
              hg_object.heap_type = "image";
              break;
            case HPROF_HEAP_JIT:
              hg_object.heap_type = "jit";
              break;
            case HPROF_HEAP_APP_CACHE:
              hg_object.heap_type = "app-cache";
              break;
            case HPROF_HEAP_SYSTEM:
              hg_object.heap_type = "system";
              break;
            case HPROF_HEAP_DEFAULT:
              hg_object.heap_type = "default";
              break;
          }

          // In skeleton implementation, we just log the object conversion
          std::cout << "  Heap type: " << (hg_object.heap_type ? *hg_object.heap_type : "unknown") << std::endl;
          std::cout << "  Self size: " << hg_object.self_size << " bytes" << std::endl;

          ir.objects.push_back(std::move(hg_object));
          converted_objects++;
        }
      }
    }

    std::cout << "Converted " << converted_objects << " objects to IR" << std::endl;
}

void HprofAstConverter::ConvertReferences(const HprofAst& ast, HeapGraphIR& ir) {
    std::cout << "Converting references to IR" << std::endl;

    for (const auto& [owner, owned_list] : ast.owner_to_owned) {
      // Find the reference set ID for the owner
      auto ref_set_id_it = object_to_reference_set_id_.find(owner);
      if (ref_set_id_it == object_to_reference_set_id_.end()) {
        std::cout << "Skipping references for owner without reference set ID: " << owner << std::endl;
        //continue;
      }

      uint32_t reference_set_id = 0;//ref_set_id_it->second;
      std::cout << "Processing references for owner: " << owner
                << ", refSetID: " << reference_set_id
                << ", reference count: " << owned_list.size() << std::endl;

      // Find the owner's class
      auto owner_class_it = ast.object_to_class.find(owner);
      std::string owner_class_name;
      if (owner_class_it != ast.object_to_class.end()) {
        auto class_info_it = ast.classes.find(owner_class_it->second);
        if (class_info_it != ast.classes.end()) {
          owner_class_name = class_info_it->second.name;
        }
      }

      for (const auto& owned : owned_list) {
        HeapGraphReference hg_ref;

        hg_ref.reference_set_id = reference_set_id;
        hg_ref.owner_id = owner;

        // Owned might be null (null reference)
        if (owned.target_object_id != 0) {
          hg_ref.owned_id = owned.target_object_id;
          diagnostics_.references_generated++;

          std::cout << "  Reference: " << owner << " -> " << owned.target_object_id
                    << " via " << owned.field_name << std::endl;
        }

        hg_ref.field_name = owned.field_name;

        // Try to get field type from class information
        auto type_it = ast.object_to_class.find(owned.target_object_id);
        if (type_it != ast.object_to_class.end()) {
          auto class_it = ast.classes.find(type_it->second);
          if (class_it != ast.classes.end()) {
            hg_ref.field_type_name = class_it->second.name;
          }
        }

        // If field type is empty, use owner class name
        if (hg_ref.field_type_name.empty() && !owner_class_name.empty()) {
          hg_ref.field_type_name = owner_class_name;
        }

        ir.references.push_back(std::move(hg_ref));
      }
    }

    std::cout << "Converted " << ir.references.size() << " references to IR" << std::endl;
}

HeapGraphValue HprofAstConverter::ConvertFieldValue(const FieldValue& value) {
    std::cout << "Converting field value of type " << value.type << std::endl;

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

std::string HprofAstConverter::DetermineClassKind(const std::string& class_name) const {
    std::cout << "Determining class kind for: " << class_name << std::endl;

    // Refined kind determination
    if (class_name.find("java.lang.") == 0) return "system";
    if (class_name.find("java.util.") == 0) return "system";
    if (class_name.find("java.concurrent.") == 0) return "system";
    if (class_name.find("jdk.internal.") == 0) return "system";
    if (class_name.find("sun.") == 0) return "system";
    if (class_name.find("com.sun.") == 0) return "system";
    if (class_name.find("android.") == 0) return "framework";
    if (class_name.find("com.android.") == 0) return "framework";
    if (class_name.find("androidx.") == 0) return "framework";
    return "app";
}

void HprofAstConverter::PrintConversionDiagnostics() {
    std::cout << "\nConversion Diagnostics:" << std::endl;
    std::cout << "----------------------" << std::endl;

    std::cout << "Total Classes Processed: " << diagnostics_.total_processed_classes << std::endl;
    std::cout << "Unique Classes Processed: " << diagnostics_.unique_classes_processed << std::endl;

    std::cout << "\nClass Kind Distribution:" << std::endl;
    for (const auto& [kind, count] : diagnostics_.class_kind_counts) {
      std::cout << "  " << kind << ": " << count << std::endl;
    }

    std::cout << "\nSuperclass Chain Lengths:" << std::endl;
    for (const auto& [length, count] : diagnostics_.superclass_chain_lengths) {
      std::cout << "  " << length << ": " << count << std::endl;
    }

    std::cout << "\nReferences:" << std::endl;
    std::cout << "  Generated References: " << diagnostics_.references_generated << std::endl;
  }


    constexpr uint32_t kHprofHeaderMagic = 0x4A415641; // "JAVA"
constexpr uint32_t kHprofHeaderLength = 20;
    // TraceBlobViewIterator implementation
ArtHprofTokenizer::TraceBlobViewIterator::TraceBlobViewIterator(
    util::TraceBlobViewReader&& reader)
    : reader_(std::move(reader)) {}

ArtHprofTokenizer::TraceBlobViewIterator::~TraceBlobViewIterator() = default;

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadU1(uint8_t& value) {
  auto slice = reader_.SliceOff(current_offset_, 1);
  if (!slice) return false;
  value = *slice->data();
  current_offset_ += 1;
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadU2(uint16_t& value) {
  uint8_t b1, b2;
  if (!ReadU1(b1) || !ReadU1(b2)) return false;
  value = static_cast<uint16_t>((static_cast<uint16_t>(b1) << 8) |
                               static_cast<uint16_t>(b2));
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadU4(uint32_t& value) {
  uint8_t b1, b2, b3, b4;
  if (!ReadU1(b1) || !ReadU1(b2) || !ReadU1(b3) || !ReadU1(b4)) return false;
  value = (static_cast<uint32_t>(b1) << 24) |
          (static_cast<uint32_t>(b2) << 16) |
          (static_cast<uint32_t>(b3) << 8) |
          static_cast<uint32_t>(b4);
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadId(uint64_t& value,
                                                    uint32_t id_size) {
  if (id_size == 4) {
    uint32_t id;
    if (!ReadU4(id)) return false;
    value = id;
    return true;
  } else if (id_size == 8) {
    uint32_t high, low;
    if (!ReadU4(high) || !ReadU4(low)) return false;
    value = (static_cast<uint64_t>(high) << 32) | low;
    return true;
  }
  return false;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadString(std::string& str,
                                                        size_t length) {
  auto slice = reader_.SliceOff(current_offset_, length);
  if (!slice) return false;

  str.resize(length);
  std::memcpy(&str[0], slice->data(), length);
  current_offset_ += length;
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadBytes(
    std::vector<uint8_t>& data, size_t length) {
  auto slice = reader_.SliceOff(current_offset_, length);
  if (!slice) return false;

  data.resize(length);
  std::memcpy(data.data(), slice->data(), length);
  current_offset_ += length;
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::SkipBytes(size_t count) {
  auto slice = reader_.SliceOff(current_offset_, count);
  if (!slice) return false;

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
  if (!status.ok()) return status;

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
                 ir_->classes.size(), ir_->objects.size(), ir_->references.size());

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
  if (!status.ok()) return status;

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
}
