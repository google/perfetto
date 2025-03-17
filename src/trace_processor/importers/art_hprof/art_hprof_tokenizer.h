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
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "src/trace_processor/importers/art_hprof/art_hprof_event.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"


namespace perfetto::trace_processor::art_hprof {
// Heap type enumeration
enum HprofHeapId {
    HPROF_HEAP_DEFAULT = 0,
    HPROF_HEAP_ZYGOTE = 1,
    HPROF_HEAP_APP = 2,
    HPROF_HEAP_IMAGE = 3,
    HPROF_HEAP_JIT = 4,
    HPROF_HEAP_APP_CACHE = 5,
    HPROF_HEAP_SYSTEM = 6
};

// Field type constants
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

// Tag constants
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

// Heap tag constants
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

// Abstracted file iterator interface
class ByteIterator {
 public:
  virtual ~ByteIterator();

  virtual bool ReadU1(uint8_t& value) = 0;
  virtual bool ReadU2(uint16_t& value) = 0;
  virtual bool ReadU4(uint32_t& value) = 0;
  virtual bool ReadId(uint64_t& value, uint32_t id_size) = 0;
  virtual bool ReadString(std::string& str, size_t length) = 0;
  virtual bool ReadBytes(std::vector<uint8_t>& data, size_t length) = 0;
  virtual bool SkipBytes(size_t count) = 0;
  virtual std::streampos GetPosition() = 0;
  virtual bool IsEof() const = 0;
  virtual bool IsValid() const = 0;
};

// Struct for field information
struct FieldInfo {
  std::string name;
  uint8_t type;
  uint64_t class_id = 0;
};

// Class information structure
struct ClassInfo {
  std::string name;
  uint64_t class_object_id = 0;
  uint64_t super_class_id = 0;
  uint32_t instance_size = 0;
  std::vector<FieldInfo> fields;
  bool is_string_class = false;
  bool use_string_compression = false;
};

// Object reference information
struct ObjectReference {
  std::string field_name;
  uint64_t target_object_id;
};

// Field value using a simple variant pattern
struct FieldValue {
  enum ValueType {
    NONE, BOOLEAN, BYTE, CHAR, SHORT, INT, FLOAT, LONG, DOUBLE, OBJECT_ID
  };

  ValueType type = NONE;
  union {
    bool bool_value;
    int8_t byte_value;
    char16_t char_value;
    int16_t short_value;
    int32_t int_value;
    float float_value;
    int64_t long_value;
    double double_value;
    uint64_t object_id_value;
  };

  // Default constructor
  FieldValue() {}

  // Type-specific constructors
    explicit FieldValue([[maybe_unused]] bool value) {}
    explicit FieldValue([[maybe_unused]] int8_t value) {}
    explicit FieldValue([[maybe_unused]] char16_t value) {}
    explicit FieldValue([[maybe_unused]] int16_t value) {}
    explicit FieldValue([[maybe_unused]] int32_t value) {}
    explicit FieldValue([[maybe_unused]] float value) {}
    explicit FieldValue([[maybe_unused]] int64_t value) {}
    explicit FieldValue([[maybe_unused]] double value) {}
    explicit FieldValue([[maybe_unused]] uint64_t value) {}
};

// Field value record
struct FieldValueRecord {
  std::string field_name;
  FieldValue value;
};

// Header node structure
struct HprofHeader {
  std::string format;
  uint32_t identifier_size;
  uint64_t timestamp;
};

// UTF8 String data
struct Utf8StringData {
  uint64_t name_id;
  std::string utf8_string;
};

// Load Class data
struct LoadClassData {
  uint32_t class_serial_num;
  uint64_t class_object_id;
  uint32_t stack_trace_serial_num;
  uint64_t class_name_id;
  std::string class_name;
};

// Heap dump data
struct HeapDumpData {
  std::vector<HprofHeapRecord> records;
};

// Base record structure
struct HprofRecord {
  uint8_t tag;
  uint32_t time;
  uint32_t length;

  // Payload for different record types
  std::variant<std::monostate, Utf8StringData, LoadClassData, HeapDumpData> data;
};

// Root record data
struct RootRecordData {
  uint8_t root_type;
  uint64_t object_id;
  uint32_t thread_id;
  uint32_t frame_number;
};

// Class Dump data
struct ClassDumpData {
  uint64_t class_object_id;
  uint32_t stack_trace_serial_num;
  uint64_t super_class_object_id;
  uint64_t class_loader_object_id;
  uint64_t signers_object_id;
  uint64_t protection_domain_object_id;
  uint32_t instance_size;
  std::vector<FieldInfo> static_fields;
  std::vector<FieldInfo> instance_fields;
  bool is_string_class;
};

// Instance Dump data
struct InstanceDumpData {
  uint64_t object_id;
  uint32_t stack_trace_serial_num;
  uint64_t class_object_id;
  std::vector<uint8_t> raw_instance_data;
  std::vector<ObjectReference> references;
  std::vector<FieldValueRecord> field_values;
  HprofHeapId heap_id;
};

// Object Array Dump data
struct ObjArrayDumpData {
  uint64_t array_object_id;
  uint32_t stack_trace_serial_num;
  uint64_t array_class_object_id;
  std::vector<uint64_t> elements;
  HprofHeapId heap_id;
};

// Primitive Array Dump data
struct PrimArrayDumpData {
  uint64_t array_object_id;
  uint32_t stack_trace_serial_num;
  uint8_t element_type;
  std::vector<uint8_t> elements;
  HprofHeapId heap_id;
  bool is_compressed;
};

// Heap Dump Info data
struct HeapDumpInfoData {
  uint32_t heap_id;
  uint64_t heap_name_string_id;
  std::string heap_name;
};

// Heap record variants
struct HprofHeapRecord {
  HprofHeapTag tag;

  std::variant<
    std::monostate,
    RootRecordData,
    ClassDumpData,
    InstanceDumpData,
    ObjArrayDumpData,
    PrimArrayDumpData,
    HeapDumpInfoData
  > data;
};

// Simplified AST structure
struct HprofAst {
  HprofHeader header;
  std::vector<HprofRecord> records;

  // Maps for resolving references
  std::unordered_map<uint64_t, std::string> id_to_string_map;
  std::unordered_map<uint32_t, uint64_t> class_serial_to_id;
  std::unordered_map<uint64_t, ClassInfo> classes;
  std::unordered_map<uint64_t, uint64_t> object_to_class;
  std::unordered_map<uint64_t, std::vector<ObjectReference>> owner_to_owned;

  // Structure to track Android heap stats
  struct AndroidHeapStats {
    size_t object_count = 0;
    size_t total_bytes = 0;

    void AddObject(size_t size);
  };

  std::unordered_map<HprofHeapId, AndroidHeapStats> android_heap_stats;

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

  // Track Android-specific information
  bool use_string_compression = false;
};

class HprofParser {
 private:
  ByteIterator* byte_iterator_;
  uint32_t identifier_size_;
  HprofAst ast_;
  HprofHeapId current_heap_;
  bool detect_string_class_;

  // ----------- Helper methods -----------
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
  uint64_t ReadObjectIDValue(const std::vector<uint8_t>& data, size_t offset, uint32_t id_size) const;
  FieldValue ExtractFieldValue(const std::vector<uint8_t>& data, size_t offset, uint8_t field_type);
  void ExtractInstanceFields(InstanceDumpData& instance_data, const ClassInfo& class_info);
  void ExtractStringInstance(InstanceDumpData& instance_data, const ClassInfo& class_info);
  void UpdateHeapStats(HprofHeapId heap_id, size_t object_size);
  void SkipUnknownSubRecord(uint8_t sub_tag, std::streampos end_pos);

  // ----------- Main parsing methods -----------
  bool ParseHeader();
  void ParseRecords();
  void ParseRecord(uint8_t tag, uint32_t time, uint32_t length);

  // ----------- Record type specific parsing methods -----------
  void ParseUtf8Record(HprofRecord& record);
  void ParseLoadClassRecord(HprofRecord& record);
  void ParseHeapDumpRecord(HprofRecord& record);
  bool ParseHeapSubRecord(uint8_t sub_tag, std::vector<HprofHeapRecord>& sub_records);

  // ----------- Heap sub-record parsing methods -----------
  void ParseRootJniGlobal(HprofHeapRecord& record);
  void ParseRootWithThread(HprofHeapRecord& record);
  void ParseSimpleRoot(HprofHeapRecord& record);
  void ParseThreadObjectRoot(HprofHeapRecord& record);
  void ParseHeapDumpInfo(HprofHeapRecord& record);
  void ParseClassDump(HprofHeapRecord& record);
  void ParseInstanceDump(HprofHeapRecord& record);
  void ParseObjectArrayDump(HprofHeapRecord& record);
  void ParsePrimitiveArrayDump(HprofHeapRecord& record);

 public:
HprofParser(ByteIterator* iterator, bool detect_string_classes = true)
    : byte_iterator_(iterator),
      identifier_size_(0),
      current_heap_(HPROF_HEAP_DEFAULT),
      detect_string_class_(detect_string_classes) {
}


  ~HprofParser();
  HprofAst Parse();
};

class HprofAstConverter {
 private:
  // Diagnostic tracking
  struct ConversionDiagnostics {
    std::unordered_map<std::string, size_t> class_kind_counts;
    std::unordered_map<std::string, size_t> superclass_chain_lengths;
    size_t total_processed_classes = 0;
    size_t unique_classes_processed = 0;
    size_t references_generated = 0;
  };

  ConversionDiagnostics diagnostics_;
  uint32_t next_reference_set_id_ = 1;
  std::unordered_map<uint64_t, uint32_t> object_to_reference_set_id_;

  void ConvertClasses(const HprofAst& ast, HeapGraphIR& ir);
  void ConvertObjects(const HprofAst& ast, HeapGraphIR& ir);
  void ConvertReferences(const HprofAst& ast, HeapGraphIR& ir);
  HeapGraphValue ConvertFieldValue(const FieldValue& value);
  std::string DetermineClassKind(const std::string& class_name) const;
  void PrintConversionDiagnostics();

 public:
  HeapGraphIR ConvertToIR(const HprofAst& ast);
};

class ArtHprofTokenizer : public ChunkedTraceReader {
 public:
  explicit ArtHprofTokenizer(TraceProcessorContext*);
  ~ArtHprofTokenizer() override;

  base::Status Parse(TraceBlobView) override;
  base::Status NotifyEndOfFile() override;

  void SetParserImpl(ArtHprofParser* parser_impl) {
    parser_impl_ = parser_impl;
  }

 private:
  using Iterator = util::TraceBlobViewReader::Iterator;

  // ByteIterator implementation for TraceBlobView
  class TraceBlobViewIterator : public ByteIterator {
   private:
    util::TraceBlobViewReader reader_;
    size_t current_offset_ = 0;

   public:
    explicit TraceBlobViewIterator(util::TraceBlobViewReader&& reader);
    ~TraceBlobViewIterator() override;

    bool ReadU1(uint8_t& value) override;
    bool ReadU2(uint16_t& value) override;
    bool ReadU4(uint32_t& value) override;
    bool ReadId(uint64_t& value, uint32_t id_size) override;
    bool ReadString(std::string& str, size_t length) override;
    bool ReadBytes(std::vector<uint8_t>& data, size_t length) override;
    bool SkipBytes(size_t count) override;
    std::streampos GetPosition() override;
    bool IsEof() const override;
    bool IsValid() const override;
  };

  struct Detect {
    base::Status Parse();
    base::Status NotifyEndOfFile() const;
    ArtHprofTokenizer* tokenizer_;
  };

  struct NonStreaming {
    base::Status Parse();
    base::Status NotifyEndOfFile() const;
    ArtHprofTokenizer* tokenizer_;
    bool is_parsing_ = false;
  };

  struct Streaming {
    base::Status Parse();
    base::Status NotifyEndOfFile();
    ArtHprofTokenizer* tokenizer_;
    size_t it_offset_ = 0;
    bool header_parsed_ = false;
  };

  using SubParser = std::variant<Detect, NonStreaming, Streaming>;

  // Initialize parsers if needed
  base::Status InitializeParserIfNeeded();

  // Process parsing results into events
  base::Status ProcessParsingResults();

  // Generate events from IR data
  void GenerateEventsFromIR(int64_t ts);

  TraceProcessorContext* const context_;
  util::TraceBlobViewReader reader_;
  SubParser sub_parser_ = Detect{this};
  ArtHprofParser* parser_impl_ = nullptr;

  // Parser components
  std::unique_ptr<ByteIterator> byte_iterator_;
  std::unique_ptr<HprofParser> parser_;
  std::optional<HprofAst> parser_result_;
  std::unique_ptr<HprofAstConverter> converter_;
  std::optional<HeapGraphIR> ir_;

  bool is_initialized_ = false;
  bool is_complete_ = false;
};

}  // namespace perfetto::trace_processor::art_hprof

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_TOKENIZER_H_
