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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_TOKENIZER_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace perfetto::trace_processor::art_hprof {

/**
 * Heap type enumeration from Android HPROF format.
 */
enum HprofHeapId {
  HPROF_HEAP_DEFAULT = 0,
  HPROF_HEAP_ZYGOTE = 1,
  HPROF_HEAP_APP = 2,
  HPROF_HEAP_IMAGE = 3,
  HPROF_HEAP_JIT = 4,
  HPROF_HEAP_APP_CACHE = 5,
  HPROF_HEAP_SYSTEM = 6
};

/**
 * Field type constants from HPROF format.
 */
enum FieldType {
  TYPE_OBJECT = 2,
  TYPE_BOOLEAN = 4,
  TYPE_CHAR = 5,
  TYPE_FLOAT = 6,
  TYPE_DOUBLE = 7,
  TYPE_BYTE = 8,
  TYPE_SHORT = 9,
  TYPE_INT = 10,
  TYPE_LONG = 11
};

/**
 * Tag constants from HPROF format.
 */
enum HprofTag {
  HPROF_UTF8 = 0x01,
  HPROF_LOAD_CLASS = 0x02,
  HPROF_UNLOAD_CLASS = 0x03,
  HPROF_FRAME = 0x04,
  HPROF_TRACE = 0x05,
  HPROF_ALLOC_SITES = 0x06,
  HPROF_HEAP_SUMMARY = 0x07,
  HPROF_START_THREAD = 0x0A,
  HPROF_END_THREAD = 0x0B,
  HPROF_HEAP_DUMP = 0x0C,
  HPROF_CPU_SAMPLES = 0x0D,
  HPROF_CONTROL_SETTINGS = 0x0E,
  HPROF_HEAP_DUMP_SEGMENT = 0x1C,
  HPROF_HEAP_DUMP_END = 0x2C
};

/**
 * Heap tag constants from HPROF format.
 */
enum HprofHeapTag {
  HPROF_ROOT_UNKNOWN = 0xFF,
  HPROF_ROOT_JNI_GLOBAL = 0x01,
  HPROF_ROOT_JNI_LOCAL = 0x02,
  HPROF_ROOT_JAVA_FRAME = 0x03,
  HPROF_ROOT_NATIVE_STACK = 0x04,
  HPROF_ROOT_STICKY_CLASS = 0x05,
  HPROF_ROOT_THREAD_BLOCK = 0x06,
  HPROF_ROOT_MONITOR_USED = 0x07,
  HPROF_ROOT_THREAD_OBJ = 0x08,
  HPROF_CLASS_DUMP = 0x20,
  HPROF_INSTANCE_DUMP = 0x21,
  HPROF_OBJ_ARRAY_DUMP = 0x22,
  HPROF_PRIM_ARRAY_DUMP = 0x23,
  HPROF_HEAP_DUMP_INFO = 0xFE,
  HPROF_ROOT_INTERNED_STRING = 0x89,
  HPROF_ROOT_FINALIZING = 0x8A,
  HPROF_ROOT_DEBUGGER = 0x8B,
  HPROF_ROOT_REFERENCE_CLEANUP = 0x8C,
  HPROF_ROOT_VM_INTERNAL = 0x8D,
  HPROF_ROOT_JNI_MONITOR = 0x8E
};

// Forward declarations
struct RootRecordData;
struct ClassDumpData;
struct InstanceDumpData;
struct ObjArrayDumpData;
struct PrimArrayDumpData;
struct HeapDumpInfoData;
struct HprofHeapRecord;
struct HprofRecord;
struct HeapDumpData;
struct Utf8StringData;
struct LoadClassData;

/**
 * Abstract interface for a byte iterator that reads from a data source.
 * This class provides an out-of-line virtual destructor to fix weak vtable
 * issues.
 */
class ByteIterator {
 public:
  virtual ~ByteIterator();

  /**
   * Read an unsigned 1-byte value.
   */
  virtual bool ReadU1(uint8_t& value) = 0;

  /**
   * Read an unsigned 2-byte value.
   */
  virtual bool ReadU2(uint16_t& value) = 0;

  /**
   * Read an unsigned 4-byte value.
   */
  virtual bool ReadU4(uint32_t& value) = 0;

  /**
   * Read an ID of specified size.
   */
  virtual bool ReadId(uint64_t& value, uint32_t id_size) = 0;

  /**
   * Read a string of specified length.
   */
  virtual bool ReadString(std::string& str, size_t length) = 0;

  /**
   * Read bytes of specified length.
   */
  virtual bool ReadBytes(std::vector<uint8_t>& data, size_t length) = 0;

  /**
   * Skip specified number of bytes.
   */
  virtual bool SkipBytes(size_t count) = 0;

  /**
   * Get current position in the stream.
   */
  virtual size_t GetPosition() = 0;

  /**
   * Check if the end of the stream has been reached.
   */
  virtual bool IsEof() const = 0;

  /**
   * Check if the iterator is valid.
   */
  virtual bool IsValid() const = 0;
};

/**
 * Field information structure.
 */
struct FieldInfo {
  std::string name;
  uint8_t type = 0;
  uint64_t class_id = 0;
};

/**
 * Class information structure.
 */
struct ClassInfo {
  std::string name;
  uint64_t class_object_id = 0;
  uint64_t super_class_id = 0;
  uint32_t instance_size = 0;
  std::vector<FieldInfo> fields;
  bool is_string_class = false;
};

/**
 * Object reference structure.
 */
struct ObjectReference {
  std::string field_name;
  uint64_t target_object_id = 0;
};

/**
 * Field value with type information.
 */
struct FieldValue {
  enum class ValueType {
    NONE,
    BOOLEAN,
    BYTE,
    CHAR,
    SHORT,
    INT,
    FLOAT,
    LONG,
    DOUBLE,
    OBJECT_ID
  };

  ValueType type = ValueType::NONE;
  std::variant<std::monostate,
               bool,
               int8_t,
               char16_t,
               int16_t,
               int32_t,
               float,
               int64_t,
               double,
               uint64_t>
      value;

  // Default constructor
  FieldValue() : type(ValueType::NONE), value(std::monostate{}) {}

  // Type-specific constructors
  explicit FieldValue(bool val) : type(ValueType::BOOLEAN), value(val) {}
  explicit FieldValue(int8_t val) : type(ValueType::BYTE), value(val) {}
  explicit FieldValue(char16_t val) : type(ValueType::CHAR), value(val) {}
  explicit FieldValue(int16_t val) : type(ValueType::SHORT), value(val) {}
  explicit FieldValue(int32_t val) : type(ValueType::INT), value(val) {}
  explicit FieldValue(float val) : type(ValueType::FLOAT), value(val) {}
  explicit FieldValue(int64_t val) : type(ValueType::LONG), value(val) {}
  explicit FieldValue(double val) : type(ValueType::DOUBLE), value(val) {}
  explicit FieldValue(uint64_t val) : type(ValueType::OBJECT_ID), value(val) {}
};

/**
 * Field value record structure.
 */
struct FieldValueRecord {
  std::string field_name;
  FieldValue value;
};

/**
 * HPROF header structure.
 */
struct HprofHeader {
  std::string format;
  uint32_t identifier_size = 0;
  uint64_t timestamp = 0;
};

/**
 * UTF8 string data structure.
 */
struct Utf8StringData {
  uint64_t name_id = 0;
  std::string utf8_string;
};

/**
 * Load class data structure.
 */
struct LoadClassData {
  uint32_t class_serial_num = 0;
  uint64_t class_object_id = 0;
  uint32_t stack_trace_serial_num = 0;
  uint64_t class_name_id = 0;
  std::string class_name;
};

/**
 * Heap dump data structure.
 */
struct HeapDumpData {
  std::vector<HprofHeapRecord> records;
};

/**
 * Base record structure.
 */
struct HprofRecord {
  uint8_t tag = 0;
  uint32_t time = 0;
  uint32_t length = 0;

  // Payload for different record types
  std::variant<std::monostate, Utf8StringData, LoadClassData, HeapDumpData>
      data;
};

/**
 * Root record data structure.
 */
struct RootRecordData {
  uint8_t root_type = 0;
  uint64_t object_id = 0;
  uint32_t thread_id = 0;
  uint32_t frame_number = 0;
};

/**
 * Class dump data structure.
 */
struct ClassDumpData {
  uint64_t class_object_id = 0;
  uint32_t stack_trace_serial_num = 0;
  uint64_t super_class_object_id = 0;
  uint64_t class_loader_object_id = 0;
  uint64_t signers_object_id = 0;
  uint64_t protection_domain_object_id = 0;
  uint32_t instance_size = 0;
  std::vector<FieldInfo> static_fields;
  std::vector<FieldInfo> instance_fields;
  bool is_string_class = false;
};

/**
 * Instance dump data structure.
 */
struct InstanceDumpData {
  uint64_t object_id = 0;
  uint32_t stack_trace_serial_num = 0;
  uint64_t class_object_id = 0;
  std::vector<uint8_t> raw_instance_data;
  std::vector<ObjectReference> references;
  std::vector<FieldValueRecord> field_values;
  HprofHeapId heap_id = HPROF_HEAP_DEFAULT;
};

/**
 * Object array dump data structure.
 */
struct ObjArrayDumpData {
  uint64_t array_object_id = 0;
  uint32_t stack_trace_serial_num = 0;
  uint64_t array_class_object_id = 0;
  std::vector<uint64_t> elements;
  HprofHeapId heap_id = HPROF_HEAP_DEFAULT;
};

/**
 * Primitive array dump data structure.
 */
struct PrimArrayDumpData {
  uint64_t array_object_id = 0;
  uint32_t stack_trace_serial_num = 0;
  uint8_t element_type = 0;
  std::vector<uint8_t> elements;
  HprofHeapId heap_id = HPROF_HEAP_DEFAULT;
  bool is_compressed = false;
};

/**
 * Heap dump info data structure.
 */
struct HeapDumpInfoData {
  uint32_t heap_id = 0;
  uint64_t heap_name_string_id = 0;
  std::string heap_name;
};

/**
 * Heap record variant structure.
 */
struct HprofHeapRecord {
  HprofHeapTag tag;

  std::variant<std::monostate,
               RootRecordData,
               ClassDumpData,
               InstanceDumpData,
               ObjArrayDumpData,
               PrimArrayDumpData,
               HeapDumpInfoData>
      data;
};

/**
 * Parsed HPROF data.
 */
struct HprofData {
  HprofHeader header;
  std::vector<HprofRecord> records;

  // Maps for resolving references
  std::unordered_map<uint64_t, std::string> id_to_string_map;
  std::unordered_map<uint32_t, uint64_t> class_serial_to_id;
  std::unordered_map<uint64_t, ClassInfo> classes;
  std::unordered_map<uint64_t, uint64_t> object_to_class;
  std::unordered_map<uint64_t, std::vector<ObjectReference>> owner_to_owned;
  std::unordered_map<uint8_t, uint64_t>
      primitive_array_class_ids;  // Using FieldType as key
  uint64_t java_lang_class_object_id = 0;

  std::unordered_map<uint64_t, uint8_t> root_objects;

  // Counters for summary
  size_t string_count = 0;
  size_t class_count = 0;
  size_t heap_dump_count = 0;
  size_t class_instance_count = 0;
  size_t object_array_count = 0;
  size_t primitive_array_count = 0;
  size_t root_count = 0;
  size_t field_reference_count = 0;
  size_t heap_info_count = 0;
};

/**
 * Parser for HPROF heap dumps.
 */
class HprofParser {
 public:
  /**
   * Creates a new HprofParser with the given byte iterator.
   *
   * @param iterator Byte iterator for reading HPROF data
   */
  explicit HprofParser(ByteIterator* iterator)
      : byte_iterator_(iterator),
        identifier_size_(0),
        current_heap_(HPROF_HEAP_DEFAULT) {}

  /**
   * Destructor.
   */
  virtual ~HprofParser();

  /**
   * Parses the HPROF binary data into data structures.
   *
   * @return The parsed data.
   */
  HprofData Parse();

 private:
  ByteIterator* byte_iterator_;
  uint32_t identifier_size_;
  HprofData data_;
  HprofHeapId current_heap_;

  // Record header structure
  struct RecordHeader {
    uint8_t tag;
    uint32_t time;
    uint32_t length;
  };

  // Main parsing methods with centralized dispatch
  void ParseHeader(HprofData& data);
  bool HasMoreData() const;
  RecordHeader ReadRecordHeader();
  void ParseRecordByType(const RecordHeader& header, HprofData& data);
  void ParseUtf8Record(const RecordHeader& header, HprofData& data);
  void ParseLoadClassRecord(const RecordHeader& header, HprofData& data);
  void ParseHeapDumpRecord(const RecordHeader& header, HprofData& data);
  void SkipRecord(uint32_t length);

  // Heap sub-record parsing methods
  void ParseRootJniGlobal(HprofData& data);
  void ParseRootWithThread(HprofData& data, uint8_t sub_tag_value);
  void ParseSimpleRoot(HprofData& data, uint8_t sub_tag_value);
  void ParseThreadObjectRoot(HprofData& data);
  void ParseHeapDumpInfo(HprofData& data);
  void ParseClassDump(HprofData& data);
  void ParseInstanceDump(HprofData& data);
  void ParseObjectArrayDump(HprofData& data);
  void ParsePrimitiveArrayDump(HprofData& data);
  void SkipUnknownSubRecord(uint8_t sub_tag);
  static std::string NormalizeClassName(std::string name);

  // Helper methods
  bool IsStringClass(const std::string& class_name) const;
  size_t GetFieldTypeSize(uint8_t type) const;
  int8_t ReadByteValue(const std::vector<uint8_t>& data, size_t offset) const;
  bool ReadBooleanValue(const std::vector<uint8_t>& data, size_t offset) const;
  int16_t ReadShortValue(const std::vector<uint8_t>& data, size_t offset) const;
  char16_t ReadCharValue(const std::vector<uint8_t>& data, size_t offset) const;
  int32_t ReadIntValue(const std::vector<uint8_t>& data, size_t offset) const;
  float ReadFloatValue(const std::vector<uint8_t>& data, size_t offset) const;
  int64_t ReadLongValue(const std::vector<uint8_t>& data, size_t offset) const;
  double ReadDoubleValue(const std::vector<uint8_t>& data, size_t offset) const;
  uint64_t ReadObjectIDValue(const std::vector<uint8_t>& data,
                             size_t offset,
                             uint32_t id_size) const;
  FieldValue ExtractFieldValue(const std::vector<uint8_t>& data,
                               size_t offset,
                               uint8_t field_type);
  void ExtractInstanceFields(InstanceDumpData& instance_data,
                             const ClassInfo& class_info);
  void ExtractStringInstance(InstanceDumpData& instance_data,
                             const ClassInfo& class_info);
  std::vector<FieldInfo> GetFieldsForClassHierarchy(uint64_t class_object_id);
  size_t GetPosition() const;
  bool IsEof() const;
};

}  // namespace perfetto::trace_processor::art_hprof

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_TOKENIZER_H_
